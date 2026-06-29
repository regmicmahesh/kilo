#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "undo.h"
#include "editor.h"
#include "output.h"

static long long tv_ms(const struct timeval *tv) {
    return (long long)tv->tv_sec * 1000LL + tv->tv_usec / 1000LL;
}

static void freeAtom(struct undoAtom *a) {
    free(a->text);
    a->text = NULL;
    a->len = 0;
}

static void freeGroup(struct undoGroup *g) {
    int i;
    if (!g->atoms) return;
    for (i = 0; i < g->natoms; i++)
        freeAtom(&g->atoms[i]);
    free(g->atoms);
    g->atoms = NULL;
    g->natoms = 0;
}

static void freeStack(struct undoGroup *stack, int len) {
    int i;
    for (i = 0; i < len; i++)
        freeGroup(&stack[i]);
}

void undoInit(struct undoState *u) {
    memset(u, 0, sizeof(*u));
}

void undoFree(struct undoState *u) {
    freeStack(u->undo, u->undo_len);
    free(u->undo);
    freeStack(u->redo, u->redo_len);
    free(u->redo);
    memset(u, 0, sizeof(*u));
}

static void clearRedo(struct editorConfig *E) {
    freeStack(E->undo.redo, E->undo.redo_len);
    free(E->undo.redo);
    E->undo.redo = NULL;
    E->undo.redo_len = 0;
    E->undo.redo_cap = 0;
}

static void ensureUndoCap(struct undoState *u, int need) {
    if (u->undo_cap >= need) return;
    int cap = u->undo_cap ? u->undo_cap * 2 : 16;
    while (cap < need) cap *= 2;
    u->undo = realloc(u->undo, sizeof(struct undoGroup) * cap);
    u->undo_cap = cap;
}

static void ensureRedoCap(struct undoState *u, int need) {
    if (u->redo_cap >= need) return;
    int cap = u->redo_cap ? u->redo_cap * 2 : 16;
    while (cap < need) cap *= 2;
    u->redo = realloc(u->redo, sizeof(struct undoGroup) * cap);
    u->redo_cap = cap;
}

static void dropOldestUndo(struct undoState *u) {
    if (u->undo_len <= 0) return;
    freeGroup(&u->undo[0]);
    memmove(u->undo, u->undo + 1, sizeof(struct undoGroup) * (u->undo_len - 1));
    u->undo_len--;
}

static char *dupText(const char *text, size_t len) {
    char *p;
    if (!text || len == 0) {
        p = malloc(1);
        if (p) p[0] = '\0';
        return p;
    }
    p = malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, text, len);
    p[len] = '\0';
    return p;
}

void undoBeginGroup(struct editorConfig *E) {
    struct undoGroup *g;

    if (E->undo.suspend) return;

    clearRedo(E);
    if (E->undo.undo_len >= UNDO_STACK_MAX)
        dropOldestUndo(&E->undo);

    ensureUndoCap(&E->undo, E->undo.undo_len + 1);
    g = &E->undo.undo[E->undo.undo_len++];
    memset(g, 0, sizeof(*g));
    g->before_cx = E->cx;
    g->before_cy = E->cy;
    g->before_coloff = E->coloff;
    g->before_rowoff = E->rowoff;
    gettimeofday(&g->tv, NULL);
}

void undoPushAtom(struct editorConfig *E, enum undoAtomType type,
                  int row, int col, const char *text, size_t len) {
    struct undoGroup *g;
    struct undoAtom *a;

    if (E->undo.suspend) return;
    if (E->undo.undo_len <= 0) return;

    g = &E->undo.undo[E->undo.undo_len - 1];
    g->atoms = realloc(g->atoms, sizeof(struct undoAtom) * (g->natoms + 1));
    a = &g->atoms[g->natoms++];
    a->type = type;
    a->row = row;
    a->col = col;
    a->len = len;
    a->text = dupText(text, len);
}

void undoEndGroup(struct editorConfig *E) {
    struct undoGroup *g;

    if (E->undo.suspend) return;
    if (E->undo.undo_len <= 0) return;

    g = &E->undo.undo[E->undo.undo_len - 1];
    /* Drop empty groups (e.g. failed edit). */
    if (g->natoms == 0) {
        freeGroup(g);
        E->undo.undo_len--;
        return;
    }
    g->after_cx = E->cx;
    g->after_cy = E->cy;
    g->after_coloff = E->coloff;
    g->after_rowoff = E->rowoff;
    gettimeofday(&g->tv, NULL);
}

static int withinGroupWindow(const struct undoGroup *g) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (tv_ms(&now) - tv_ms(&g->tv)) <= UNDO_GROUP_MS;
}

int undoTryMergeInsertChar(struct editorConfig *E, int row, int col, int c) {
    struct undoGroup *g;
    struct undoAtom *a;
    char *nt;
    char ch = (char)c;

    if (E->undo.suspend) return 0;
    if (E->undo.undo_len <= 0) return 0;
    /* Cannot merge if redo history exists — a new edit must not extend
     * a group that was partially undone (redo was cleared only on begin). */
    if (E->undo.redo_len > 0) return 0;

    g = &E->undo.undo[E->undo.undo_len - 1];
    if (g->natoms != 1) return 0;
    a = &g->atoms[0];
    if (a->type != U_INSERT_TEXT) return 0;
    if (a->row != row) return 0;
    if (a->col + (int)a->len != col) return 0;
    if (!withinGroupWindow(g)) return 0;

    nt = realloc(a->text, a->len + 2);
    if (!nt) return 0;
    a->text = nt;
    a->text[a->len++] = ch;
    a->text[a->len] = '\0';
    g->after_cx = E->cx;
    g->after_cy = E->cy;
    g->after_coloff = E->coloff;
    g->after_rowoff = E->rowoff;
    gettimeofday(&g->tv, NULL);
    return 1;
}

int undoTryMergeDeleteChar(struct editorConfig *E, int row, int col, int c) {
    struct undoGroup *g;
    struct undoAtom *a;
    char *nt;
    char ch = (char)c;

    if (E->undo.suspend) return 0;
    if (E->undo.undo_len <= 0) return 0;
    if (E->undo.redo_len > 0) return 0;

    g = &E->undo.undo[E->undo.undo_len - 1];
    if (g->natoms != 1) return 0;
    a = &g->atoms[0];
    if (a->type != U_DELETE_TEXT) return 0;
    if (a->row != row) return 0;
    /* Backspace removes the char before the cursor; successive backspaces
     * delete at decreasing columns, prepending to the deleted text. */
    if (col + 1 != a->col) return 0;
    if (!withinGroupWindow(g)) return 0;

    nt = realloc(a->text, a->len + 2);
    if (!nt) return 0;
    memmove(nt + 1, nt, a->len);
    nt[0] = ch;
    a->text = nt;
    a->len++;
    a->text[a->len] = '\0';
    a->col = col;
    g->after_cx = E->cx;
    g->after_cy = E->cy;
    g->after_coloff = E->coloff;
    g->after_rowoff = E->rowoff;
    gettimeofday(&g->tv, NULL);
    return 1;
}

/* --- Buffer apply helpers (no undo recording) --- */

static void ensureRow(struct editorConfig *E, int row) {
    while (E->numrows <= row)
        editorInsertRow(E, E->numrows, "", 0);
}

void undoApplyInsertText(struct editorConfig *E, int row, int col,
                         const char *text, size_t len) {
    size_t i;
    erow *r;

    ensureRow(E, row);
    r = &E->row[row];
    for (i = 0; i < len; i++)
        editorRowInsertChar(E, r, col + (int)i, (unsigned char)text[i]);
}

void undoApplyDeleteText(struct editorConfig *E, int row, int col, size_t len) {
    size_t i;
    erow *r;

    if (row < 0 || row >= E->numrows) return;
    r = &E->row[row];
    for (i = 0; i < len; i++) {
        if (col >= r->size) break;
        editorRowDelChar(E, r, col);
    }
}

void undoApplySplitLine(struct editorConfig *E, int row, int col) {
    erow *r;

    ensureRow(E, row);
    r = &E->row[row];
    if (col > r->size) col = r->size;
    if (col == 0) {
        editorInsertRow(E, row, "", 0);
    } else {
        editorInsertRow(E, row + 1, r->chars + col, r->size - col);
        r = &E->row[row];
        r->chars[col] = '\0';
        r->size = col;
        editorUpdateRow(E, r);
    }
}

void undoApplyJoinLines(struct editorConfig *E, int row) {
    erow *cur;

    if (row < 0 || row + 1 >= E->numrows) return;
    cur = &E->row[row + 1];
    editorRowAppendString(E, &E->row[row], cur->chars, cur->size);
    editorDelRow(E, row + 1);
}

void undoApplyInsertRow(struct editorConfig *E, int row, const char *text, size_t len) {
    editorInsertRow(E, row, (char *)text, len);
}

void undoApplyDeleteRow(struct editorConfig *E, int row) {
    editorDelRow(E, row);
}

static void applyAtomForward(struct editorConfig *E, const struct undoAtom *a) {
    switch (a->type) {
    case U_INSERT_TEXT:
        undoApplyInsertText(E, a->row, a->col, a->text, a->len);
        break;
    case U_DELETE_TEXT:
        undoApplyDeleteText(E, a->row, a->col, a->len);
        break;
    case U_SPLIT_LINE:
        undoApplySplitLine(E, a->row, a->col);
        break;
    case U_JOIN_LINES:
        undoApplyJoinLines(E, a->row);
        break;
    case U_INSERT_ROW:
        undoApplyInsertRow(E, a->row, a->text, a->len);
        break;
    case U_DELETE_ROW:
        undoApplyDeleteRow(E, a->row);
        break;
    }
}

static void applyAtomInverse(struct editorConfig *E, const struct undoAtom *a) {
    switch (a->type) {
    case U_INSERT_TEXT:
        undoApplyDeleteText(E, a->row, a->col, a->len);
        break;
    case U_DELETE_TEXT:
        undoApplyInsertText(E, a->row, a->col, a->text, a->len);
        break;
    case U_SPLIT_LINE:
        undoApplyJoinLines(E, a->row);
        break;
    case U_JOIN_LINES:
        undoApplySplitLine(E, a->row, a->col);
        break;
    case U_INSERT_ROW:
        undoApplyDeleteRow(E, a->row);
        break;
    case U_DELETE_ROW:
        undoApplyInsertRow(E, a->row, a->text, a->len);
        break;
    }
}

static void restoreCursor(struct editorConfig *E, int cx, int cy,
                          int coloff, int rowoff) {
    E->cx = cx;
    E->cy = cy;
    E->coloff = coloff;
    E->rowoff = rowoff;
}

static struct undoGroup *dupGroup(const struct undoGroup *src) {
    struct undoGroup *g = calloc(1, sizeof(*g));
    int i;

    if (!g) return NULL;
    *g = *src;
    g->atoms = NULL;
    if (src->natoms > 0) {
        g->atoms = calloc(src->natoms, sizeof(struct undoAtom));
        if (!g->atoms) {
            free(g);
            return NULL;
        }
        for (i = 0; i < src->natoms; i++) {
            g->atoms[i] = src->atoms[i];
            g->atoms[i].text = dupText(src->atoms[i].text, src->atoms[i].len);
        }
    }
    return g;
}

void editorUndo(struct editorConfig *E) {
    struct undoGroup *g;
    struct undoGroup *copy;
    int i;

    if (E->undo.undo_len <= 0) {
        editorSetStatusMessage(E, "Nothing to undo");
        return;
    }

    g = &E->undo.undo[E->undo.undo_len - 1];
    E->undo.suspend = 1;
    for (i = g->natoms - 1; i >= 0; i--)
        applyAtomInverse(E, &g->atoms[i]);
    restoreCursor(E, g->before_cx, g->before_cy, g->before_coloff, g->before_rowoff);
    E->undo.suspend = 0;

    copy = dupGroup(g);
    freeGroup(g);
    E->undo.undo_len--;

    if (copy) {
        ensureRedoCap(&E->undo, E->undo.redo_len + 1);
        E->undo.redo[E->undo.redo_len++] = *copy;
        free(copy); /* atoms ownership transferred into redo stack entry */
    }
    E->dirty++;
    editorSetStatusMessage(E, "Undo");
}

void editorRedo(struct editorConfig *E) {
    struct undoGroup *g;
    struct undoGroup *copy;
    int i;

    if (E->undo.redo_len <= 0) {
        editorSetStatusMessage(E, "Nothing to redo");
        return;
    }

    g = &E->undo.redo[E->undo.redo_len - 1];
    E->undo.suspend = 1;
    for (i = 0; i < g->natoms; i++)
        applyAtomForward(E, &g->atoms[i]);
    restoreCursor(E, g->after_cx, g->after_cy, g->after_coloff, g->after_rowoff);
    E->undo.suspend = 0;

    copy = dupGroup(g);
    freeGroup(g);
    E->undo.redo_len--;

    if (copy) {
        if (E->undo.undo_len >= UNDO_STACK_MAX)
            dropOldestUndo(&E->undo);
        ensureUndoCap(&E->undo, E->undo.undo_len + 1);
        E->undo.undo[E->undo.undo_len++] = *copy;
        free(copy);
    }
    E->dirty++;
    editorSetStatusMessage(E, "Redo");
}
