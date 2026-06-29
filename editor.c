#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"
#include "syntax.h"
#include "output.h"
#include "undo.h"
#include "gui.h"

void updateWindowSize(struct editorConfig *E) {
    guiUpdateEditorSize(E);
}

/* Digits needed for the largest line number, plus one padding column. */
int editorGutterWidth(const struct editorConfig *E) {
    int lines = E->numrows > 0 ? E->numrows : 1;
    int digits = 1;
    while (lines >= 10) {
        digits++;
        lines /= 10;
    }
    return digits + 1; /* trailing space between gutter and text */
}

int editorTextCols(const struct editorConfig *E) {
    int g = editorGutterWidth(E);
    int cols = E->screencols - g;
    return cols > 1 ? cols : 1;
}

void initEditor(struct editorConfig *E) {
    E->cx = 0;
    E->cy = 0;
    E->rowoff = 0;
    E->coloff = 0;
    E->numrows = 0;
    E->row = NULL;
    E->dirty = 0;
    E->filename = NULL;
    E->syntax = NULL;
    E->statusmsg[0] = '\0';
    E->statusmsg_time = 0;
    E->screenrows = 40;
    E->screencols = 100;
    E->sel_active = 0;
    E->sel_anchor_row = E->sel_anchor_col = 0;
    E->sel_caret_row = E->sel_caret_col = 0;
    memset(&E->ctx_menu, 0, sizeof(E->ctx_menu));
    E->ctx_menu.hover = -1;
    undoInit(&E->undo);
    lspInit(&E->lsp);
    updateWindowSize(E);
}

/* Update the rendered version and the syntax highlight of a row. */
void editorUpdateRow(struct editorConfig *E, erow *row) {
    unsigned int tabs = 0, nonprint = 0;
    int j, idx;

    /* Create a version of the row we can directly print on the screen,
     * respecting tabs, substituting non printable characters with '?'. */
    free(row->render);
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == TAB) tabs++;

    unsigned long long allocsize =
        (unsigned long long)row->size + tabs * 8 + nonprint * 9 + 1;
    if (allocsize > UINT32_MAX) {
        printf("Some line of the edited file is too long for kilo\n");
        exit(1);
    }

    row->render = malloc(row->size + tabs * 8 + nonprint * 9 + 1);
    idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == TAB) {
            row->render[idx++] = ' ';
            while ((idx + 1) % 8 != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->rsize = idx;
    row->render[idx] = '\0';

    /* Update the syntax highlighting attributes of the row. */
    editorUpdateSyntax(E, row);
}

/* Insert a row at the specified position, shifting the other rows on the bottom
 * if required. */
void editorInsertRow(struct editorConfig *E, int at, char *s, size_t len) {
    if (at > E->numrows) return;
    E->row = realloc(E->row, sizeof(erow) * (E->numrows + 1));
    if (at != E->numrows) {
        memmove(E->row + at + 1, E->row + at, sizeof(E->row[0]) * (E->numrows - at));
        for (int j = at + 1; j <= E->numrows; j++) E->row[j].idx++;
    }
    E->row[at].size = len;
    E->row[at].chars = malloc(len + 1);
    memcpy(E->row[at].chars, s, len + 1);
    E->row[at].hl = NULL;
    E->row[at].hl_oc = 0;
    E->row[at].render = NULL;
    E->row[at].rsize = 0;
    E->row[at].idx = at;
    editorUpdateRow(E, E->row + at);
    E->numrows++;
    E->dirty++;
}

/* Free row's heap allocated stuff. */
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

/* Remove the row at the specified position, shifting the remainign on the
 * top. */
void editorDelRow(struct editorConfig *E, int at) {
    erow *row;

    if (at >= E->numrows) return;
    row = E->row + at;
    editorFreeRow(row);
    memmove(E->row + at, E->row + at + 1, sizeof(E->row[0]) * (E->numrows - at - 1));
    for (int j = at; j < E->numrows - 1; j++) E->row[j].idx++;
    E->numrows--;
    E->dirty++;
}

/* Turn the editor rows into a single heap-allocated string.
 * Returns the pointer to the heap-allocated string and populate the
 * integer pointed by 'buflen' with the size of the string, escluding
 * the final nulterm. */
char *editorRowsToString(struct editorConfig *E, int *buflen) {
    char *buf = NULL, *p;
    int totlen = 0;
    int j;

    /* Compute count of bytes */
    for (j = 0; j < E->numrows; j++)
        totlen += E->row[j].size + 1; /* +1 is for "\n" at end of every row */
    *buflen = totlen;
    totlen++; /* Also make space for nulterm */

    p = buf = malloc(totlen);
    for (j = 0; j < E->numrows; j++) {
        memcpy(p, E->row[j].chars, E->row[j].size);
        p += E->row[j].size;
        *p = '\n';
        p++;
    }
    *p = '\0';
    return buf;
}

/* Insert a character at the specified position in a row, moving the remaining
 * chars on the right if needed. */
void editorRowInsertChar(struct editorConfig *E, erow *row, int at, int c) {
    if (at > row->size) {
        /* Pad the string with spaces if the insert location is outside the
         * current length by more than a single character. */
        int padlen = at - row->size;
        /* In the next line +2 means: new char and null term. */
        row->chars = realloc(row->chars, row->size + padlen + 2);
        memset(row->chars + row->size, ' ', padlen);
        row->chars[row->size + padlen + 1] = '\0';
        row->size += padlen + 1;
    } else {
        /* If we are in the middle of the string just make space for 1 new
         * char plus the (already existing) null term. */
        row->chars = realloc(row->chars, row->size + 2);
        memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
        row->size++;
    }
    row->chars[at] = c;
    editorUpdateRow(E, row);
    E->dirty++;
}

/* Append the string 's' at the end of a row */
void editorRowAppendString(struct editorConfig *E, erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(row->chars + row->size, s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(E, row);
    E->dirty++;
}

/* Delete the character at offset 'at' from the specified row. */
void editorRowDelChar(struct editorConfig *E, erow *row, int at) {
    if (row->size <= at) return;
    memmove(row->chars + at, row->chars + at + 1, row->size - at);
    editorUpdateRow(E, row);
    row->size--;
    E->dirty++;
}

/* Insert the specified char at the current prompt position. */
void editorInsertChar(struct editorConfig *E, int c) {
    int filerow = E->rowoff + E->cy;
    int filecol = E->coloff + E->cx;
    erow *row = (filerow >= E->numrows) ? NULL : &E->row[filerow];
    int created_rows = 0;
    char ch = (char)c;
    int merged = 0;

    /* If the row where the cursor is currently located does not exist in our
     * logical representaion of the file, add enough empty rows as needed. */
    if (!row) {
        while (E->numrows <= filerow) {
            if (!E->undo.suspend) {
                if (!created_rows) undoBeginGroup(E);
                undoPushAtom(E, U_INSERT_ROW, E->numrows, 0, "", 0);
            }
            editorInsertRow(E, E->numrows, "", 0);
            created_rows++;
        }
    }
    row = &E->row[filerow];

    if (!E->undo.suspend) {
        if (created_rows) {
            undoPushAtom(E, U_INSERT_TEXT, filerow, filecol, &ch, 1);
        } else {
            /* Merge needs after-cursor updated after the edit; try merge on
             * position only first by checking without relying on after. */
            merged = undoTryMergeInsertChar(E, filerow, filecol, c);
            if (!merged) {
                undoBeginGroup(E);
                undoPushAtom(E, U_INSERT_TEXT, filerow, filecol, &ch, 1);
            }
        }
    }

    editorRowInsertChar(E, row, filecol, c);
    if (E->cx == editorTextCols(E) - 1)
        E->coloff++;
    else
        E->cx++;
    E->dirty++;

    if (!E->undo.suspend && E->undo.undo_len > 0)
        undoEndGroup(E);
}

/* Leading spaces/tabs on a row (for auto-indent on Enter). */
static int editorLeadingIndentLen(erow *row) {
    int i = 0;
    if (!row) return 0;
    while (i < row->size && (row->chars[i] == ' ' || row->chars[i] == '\t'))
        i++;
    return i;
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed.
 * The new line inherits leading whitespace from the line Enter was pressed on. */
void editorInsertNewline(struct editorConfig *E) {
    int filerow = E->rowoff + E->cy;
    int filecol = E->coloff + E->cx;
    erow *row = (filerow >= E->numrows) ? NULL : &E->row[filerow];
    char *indent = NULL;
    int indent_len = 0;
    int new_row;

    if (row) {
        indent_len = editorLeadingIndentLen(row);
        if (indent_len > 0) {
            indent = malloc(indent_len);
            if (indent)
                memcpy(indent, row->chars, indent_len);
            else
                indent_len = 0;
        }
    }

    if (!row) {
        if (filerow == E->numrows) {
            if (!E->undo.suspend) {
                undoBeginGroup(E);
                undoPushAtom(E, U_SPLIT_LINE, filerow, 0, NULL, 0);
            }
            editorInsertRow(E, filerow, "", 0);
            new_row = filerow;
            goto fixcursor;
        }
        free(indent);
        return;
    }
    /* If the cursor is over the current line size, we want to conceptually
     * think it's just over the last character. */
    if (filecol >= row->size) filecol = row->size;
    if (!E->undo.suspend) {
        undoBeginGroup(E);
        undoPushAtom(E, U_SPLIT_LINE, filerow, filecol, NULL, 0);
    }
    if (filecol == 0) {
        editorInsertRow(E, filerow, "", 0);
        new_row = filerow;
    } else {
        /* We are in the middle of a line. Split it between two rows. */
        editorInsertRow(E, filerow + 1, row->chars + filecol, row->size - filecol);
        row = &E->row[filerow];
        row->chars[filecol] = '\0';
        row->size = filecol;
        editorUpdateRow(E, row);
        new_row = filerow + 1;
    }
fixcursor:
    if (E->cy == E->screenrows - 1) {
        E->rowoff++;
    } else {
        E->cy++;
    }
    E->cx = 0;
    E->coloff = 0;

    /* Apply inherited leading whitespace on the new line (skip during
     * undo/redo or other suspended recording, e.g. paste). */
    if (!E->undo.suspend && indent_len > 0 && indent && new_row < E->numrows) {
        erow *nrow = &E->row[new_row];
        /* Avoid doubling indent when the split already kept leading spaces. */
        int existing = editorLeadingIndentLen(nrow);
        int need = indent_len;
        if (existing >= indent_len) {
            need = 0;
        } else if (existing > 0 &&
                   existing <= indent_len &&
                   memcmp(nrow->chars, indent, existing) == 0) {
            need = indent_len - existing;
        }
        if (need > 0) {
            const char *ins = indent + (indent_len - need);
            if (!E->undo.suspend)
                undoPushAtom(E, U_INSERT_TEXT, new_row, 0, ins, (size_t)need);
            {
                int prev = E->undo.suspend;
                E->undo.suspend = 1;
                undoApplyInsertText(E, new_row, 0, ins, (size_t)need);
                E->undo.suspend = prev;
            }
        }
        /* Place cursor after inherited indent. */
        {
            int i, rcol = 0;
            nrow = &E->row[new_row];
            for (i = 0; i < indent_len && i < nrow->size; i++) {
                if (nrow->chars[i] == '\t') {
                    rcol++;
                    while ((rcol + 1) % 8 != 0) rcol++;
                } else {
                    rcol++;
                }
            }
            E->cx = rcol;
            E->coloff = 0;
            if (E->cx >= editorTextCols(E)) {
                int shift = E->cx - editorTextCols(E) + 1;
                E->cx -= shift;
                E->coloff += shift;
            }
        }
    }

    free(indent);
    if (!E->undo.suspend)
        undoEndGroup(E);
}

/* Delete the char at the current prompt position. */
void editorDelChar(struct editorConfig *E) {
    int filerow = E->rowoff + E->cy;
    int filecol = E->coloff + E->cx;
    erow *row = (filerow >= E->numrows) ? NULL : &E->row[filerow];
    int merged = 0;

    if (!row || (filecol == 0 && filerow == 0)) return;
    if (filecol == 0) {
        /* Handle the case of column 0, we need to move the current line
         * on the right of the previous one. */
        int join_col = E->row[filerow - 1].size;
        if (!E->undo.suspend) {
            undoBeginGroup(E);
            undoPushAtom(E, U_JOIN_LINES, filerow - 1, join_col, NULL, 0);
        }
        filecol = join_col;
        editorRowAppendString(E, &E->row[filerow - 1], row->chars, row->size);
        editorDelRow(E, filerow);
        row = NULL;
        if (E->cy == 0)
            E->rowoff--;
        else
            E->cy--;
        E->cx = filecol;
        if (E->cx >= editorTextCols(E)) {
            int shift = (editorTextCols(E) - E->cx) + 1;
            E->cx -= shift;
            E->coloff += shift;
        }
        if (!E->undo.suspend)
            undoEndGroup(E);
    } else {
        char ch = row->chars[filecol - 1];
        int del_col = filecol - 1;

        if (!E->undo.suspend) {
            merged = undoTryMergeDeleteChar(E, filerow, del_col, (unsigned char)ch);
            if (!merged) {
                undoBeginGroup(E);
                undoPushAtom(E, U_DELETE_TEXT, filerow, del_col, &ch, 1);
            }
        }
        editorRowDelChar(E, row, del_col);
        if (E->cx == 0 && E->coloff)
            E->coloff--;
        else
            E->cx--;
        if (!E->undo.suspend)
            undoEndGroup(E);
    }
    if (row) editorUpdateRow(E, row);
    E->dirty++;
}

/* Handle cursor position change because arrow keys were pressed. */
void editorMoveCursor(struct editorConfig *E, int key) {
    int filerow = E->rowoff + E->cy;
    int filecol = E->coloff + E->cx;
    int rowlen;
    erow *row = (filerow >= E->numrows) ? NULL : &E->row[filerow];

    switch (key) {
    case ARROW_LEFT:
        if (E->cx == 0) {
            if (E->coloff) {
                E->coloff--;
            } else {
                if (filerow > 0) {
                    E->cy--;
                    E->cx = E->row[filerow - 1].size;
                    if (E->cx > editorTextCols(E) - 1) {
                        E->coloff = E->cx - editorTextCols(E) + 1;
                        E->cx = editorTextCols(E) - 1;
                    }
                }
            }
        } else {
            E->cx -= 1;
        }
        break;
    case ARROW_RIGHT:
        if (row && filecol < row->size) {
            if (E->cx == editorTextCols(E) - 1) {
                E->coloff++;
            } else {
                E->cx += 1;
            }
        } else if (row && filecol == row->size) {
            E->cx = 0;
            E->coloff = 0;
            if (E->cy == E->screenrows - 1) {
                E->rowoff++;
            } else {
                E->cy += 1;
            }
        }
        break;
    case ARROW_UP:
        if (E->cy == 0) {
            if (E->rowoff) E->rowoff--;
        } else {
            E->cy -= 1;
        }
        break;
    case ARROW_DOWN:
        if (filerow < E->numrows) {
            if (E->cy == E->screenrows - 1) {
                E->rowoff++;
            } else {
                E->cy += 1;
            }
        }
        break;
    }
    /* Fix cx if the current line has not enough chars. */
    filerow = E->rowoff + E->cy;
    filecol = E->coloff + E->cx;
    row = (filerow >= E->numrows) ? NULL : &E->row[filerow];
    rowlen = row ? row->size : 0;
    if (filecol > rowlen) {
        E->cx -= filecol - rowlen;
        if (E->cx < 0) {
            E->coloff += E->cx;
            E->cx = 0;
        }
    }
}

int editorFileWasModified(struct editorConfig *E) {
    return E->dirty;
}

void editorClearSelection(struct editorConfig *E) {
    E->sel_active = 0;
}

void editorSelectionNormalized(const struct editorConfig *E,
                               int *sr, int *sc, int *er, int *ec) {
    int ar = E->sel_anchor_row, ac = E->sel_anchor_col;
    int cr = E->sel_caret_row, cc = E->sel_caret_col;
    if (ar < cr || (ar == cr && ac <= cc)) {
        *sr = ar; *sc = ac; *er = cr; *ec = cc;
    } else {
        *sr = cr; *sc = cc; *er = ar; *ec = ac;
    }
}

int editorPosSelected(const struct editorConfig *E, int row, int col) {
    int sr, sc, er, ec;
    if (!E->sel_active) return 0;
    editorSelectionNormalized(E, &sr, &sc, &er, &ec);
    if (row < sr || row > er) return 0;
    if (sr == er) return col >= sc && col < ec;
    if (row == sr) return col >= sc;
    if (row == er) return col < ec;
    return 1; /* full middle lines */
}

const char *contextMenuLabels[] = {
    "Undo",
    "Redo",
    "Cut",
    "Copy",
    "Paste",
    "Select All",
    "Save",
    "Find..."
};
const int contextMenuActions[] = {
    CTX_UNDO, CTX_REDO, CTX_CUT, CTX_COPY,
    CTX_PASTE, CTX_SELECT_ALL, CTX_SAVE, CTX_FIND
};
const int contextMenuItemCount = 8;

void editorOpenContextMenu(struct editorConfig *E, int cell_col, int cell_row) {
    E->ctx_menu.active = 1;
    E->ctx_menu.col = cell_col;
    E->ctx_menu.row = cell_row;
    E->ctx_menu.hover = 0;
    E->ctx_menu.nitems = contextMenuItemCount;
    /* Keep menu on screen. */
    if (E->ctx_menu.row + E->ctx_menu.nitems >= E->screenrows + 2)
        E->ctx_menu.row = E->screenrows + 2 - E->ctx_menu.nitems;
    if (E->ctx_menu.row < 0) E->ctx_menu.row = 0;
    if (E->ctx_menu.col + 16 > E->screencols)
        E->ctx_menu.col = E->screencols - 16;
    if (E->ctx_menu.col < 0) E->ctx_menu.col = 0;
}

void editorCloseContextMenu(struct editorConfig *E) {
    E->ctx_menu.active = 0;
    E->ctx_menu.hover = -1;
}

int editorContextMenuClick(struct editorConfig *E, int cell_col, int cell_row) {
    int i;
    int width = 16;
    if (!E->ctx_menu.active) return CTX_NONE;
    for (i = 0; i < E->ctx_menu.nitems; i++) {
        int r = E->ctx_menu.row + i;
        int c0 = E->ctx_menu.col;
        if (cell_row == r && cell_col >= c0 && cell_col < c0 + width) {
            int act = contextMenuActions[i];
            editorCloseContextMenu(E);
            return act;
        }
    }
    editorCloseContextMenu(E);
    return CTX_NONE;
}

char *editorGetSelectedText(struct editorConfig *E) {
    int sr, sc, er, ec, r;
    size_t len = 0, cap = 0;
    char *buf = NULL;

    if (!E->sel_active || E->numrows <= 0) return NULL;
    editorSelectionNormalized(E, &sr, &sc, &er, &ec);
    if (sr == er && sc >= ec) return NULL;

    for (r = sr; r <= er; r++) {
        erow *row = &E->row[r];
        int from = (r == sr) ? sc : 0;
        int to = (r == er) ? ec : row->size;
        int n;
        if (from < 0) from = 0;
        if (to > row->size) to = row->size;
        if (to < from) to = from;
        n = to - from;
        if (len + (size_t)n + 2 > cap) {
            cap = (len + (size_t)n + 2) * 2 + 64;
            buf = realloc(buf, cap);
            if (!buf) return NULL;
        }
        if (n > 0) {
            memcpy(buf + len, row->chars + from, (size_t)n);
            len += (size_t)n;
        }
        if (r < er) {
            buf[len++] = '\n';
        }
    }
    if (!buf) {
        buf = malloc(1);
        if (buf) buf[0] = '\0';
        return buf;
    }
    buf[len] = '\0';
    return buf;
}

int editorDeleteSelection(struct editorConfig *E) {
    int sr, sc, er, ec, r;
    if (!E->sel_active || E->numrows <= 0) return 0;
    editorSelectionNormalized(E, &sr, &sc, &er, &ec);
    if (sr == er && sc >= ec) {
        editorClearSelection(E);
        return 0;
    }

    undoBeginGroup(E);
    if (sr == er) {
        /* Single line range. */
        erow *row = &E->row[sr];
        int n = ec - sc;
        if (n > 0 && sc < row->size) {
            char *old = malloc((size_t)n + 1);
            if (old) {
                int nn = n;
                if (sc + nn > row->size) nn = row->size - sc;
                memcpy(old, row->chars + sc, (size_t)nn);
                old[nn] = '\0';
                undoPushAtom(E, U_DELETE_TEXT, sr, sc, old, (size_t)nn);
                free(old);
            }
            {
                int prev = E->undo.suspend;
                E->undo.suspend = 1;
                undoApplyDeleteText(E, sr, sc, (size_t)(ec - sc));
                E->undo.suspend = prev;
            }
        }
    } else {
        /* Multi-line: delete from end toward start so indices stay valid. */
        /* 1) Delete tail on last line */
        if (er < E->numrows && ec > 0) {
            erow *row = &E->row[er];
            int n = ec;
            if (n > row->size) n = row->size;
            if (n > 0) {
                char *old = malloc((size_t)n + 1);
                if (old) {
                    memcpy(old, row->chars, (size_t)n);
                    old[n] = '\0';
                    undoPushAtom(E, U_DELETE_TEXT, er, 0, old, (size_t)n);
                    free(old);
                }
                {
                    int prev = E->undo.suspend;
                    E->undo.suspend = 1;
                    undoApplyDeleteText(E, er, 0, (size_t)n);
                    E->undo.suspend = prev;
                }
            }
        }
        /* 2) Delete whole middle lines */
        for (r = er - 1; r > sr; r--) {
            erow *row = &E->row[r];
            undoPushAtom(E, U_DELETE_ROW, r, 0, row->chars, (size_t)row->size);
            {
                int prev = E->undo.suspend;
                E->undo.suspend = 1;
                undoApplyDeleteRow(E, r);
                E->undo.suspend = prev;
            }
        }
        /* 3) Delete from sc to end on first line, then join with next */
        if (sr < E->numrows) {
            erow *row = &E->row[sr];
            if (sc < row->size) {
                int n = row->size - sc;
                char *old = malloc((size_t)n + 1);
                if (old) {
                    memcpy(old, row->chars + sc, (size_t)n);
                    old[n] = '\0';
                    undoPushAtom(E, U_DELETE_TEXT, sr, sc, old, (size_t)n);
                    free(old);
                }
                {
                    int prev = E->undo.suspend;
                    E->undo.suspend = 1;
                    undoApplyDeleteText(E, sr, sc, (size_t)n);
                    E->undo.suspend = prev;
                }
            }
            /* Join following line (former er remnant) */
            if (sr + 1 < E->numrows) {
                undoPushAtom(E, U_JOIN_LINES, sr, E->row[sr].size, NULL, 0);
                {
                    int prev = E->undo.suspend;
                    E->undo.suspend = 1;
                    undoApplyJoinLines(E, sr);
                    E->undo.suspend = prev;
                }
            }
        }
    }
    /* Place cursor at selection start */
    E->rowoff = sr;
    E->cy = 0;
    if (sr >= E->rowoff && sr < E->rowoff + E->screenrows)
        E->cy = sr - E->rowoff;
    else {
        E->rowoff = sr;
        E->cy = 0;
    }
    E->coloff = 0;
    E->cx = sc;
    if (E->cx >= editorTextCols(E)) {
        E->coloff = E->cx - editorTextCols(E) + 1;
        E->cx = editorTextCols(E) - 1;
    }
    E->dirty++;
    undoEndGroup(E);
    editorClearSelection(E);
    return 1;
}

void editorSelectAll(struct editorConfig *E) {
    int last_row, last_col;

    if (E->numrows <= 0) {
        E->sel_active = 0;
        E->sel_anchor_row = E->sel_anchor_col = 0;
        E->sel_caret_row = E->sel_caret_col = 0;
        E->cx = E->cy = 0;
        E->rowoff = E->coloff = 0;
        return;
    }

    last_row = E->numrows - 1;
    last_col = E->row[last_row].size;

    E->sel_active = 1;
    E->sel_anchor_row = 0;
    E->sel_anchor_col = 0;
    E->sel_caret_row = last_row;
    E->sel_caret_col = last_col;

    /* Move caret to end of buffer and scroll into view. */
    E->rowoff = last_row - E->screenrows + 1;
    if (E->rowoff < 0) E->rowoff = 0;
    E->cy = last_row - E->rowoff;
    E->coloff = 0;
    if (last_col < editorTextCols(E)) {
        E->cx = last_col;
    } else {
        E->coloff = last_col - editorTextCols(E) + 1;
        if (E->coloff < 0) E->coloff = 0;
        E->cx = last_col - E->coloff;
    }
}

/* Insert a full row as one undo unit (e.g. paste line / indent helpers). */
void editorUndoableInsertRow(struct editorConfig *E, int at, char *s, size_t len) {
    if (!E->undo.suspend) {
        undoBeginGroup(E);
        undoPushAtom(E, U_INSERT_ROW, at, 0, s, len);
    }
    editorInsertRow(E, at, s, len);
    if (!E->undo.suspend)
        undoEndGroup(E);
}

/* Delete a full row as one undo unit. */
void editorUndoableDeleteRow(struct editorConfig *E, int at) {
    erow *row;
    if (at < 0 || at >= E->numrows) return;
    row = &E->row[at];
    if (!E->undo.suspend) {
        undoBeginGroup(E);
        undoPushAtom(E, U_DELETE_ROW, at, 0, row->chars, row->size);
    }
    editorDelRow(E, at);
    if (!E->undo.suspend)
        undoEndGroup(E);
}

/* Insert a multi-character string at the cursor as one undo unit (paste).
 * Newlines in the string split lines via U_SPLIT_LINE atoms in the same group. */
void editorUndoableInsertText(struct editorConfig *E, const char *s, size_t len) {
    size_t i;
    int filerow, filecol;
    size_t start;

    if (!s || len == 0) return;

    if (!E->undo.suspend)
        undoBeginGroup(E);

    i = 0;
    while (i < len) {
        filerow = E->rowoff + E->cy;
        filecol = E->coloff + E->cx;

        if (s[i] == '\n') {
            if (!E->undo.suspend)
                undoPushAtom(E, U_SPLIT_LINE, filerow, filecol, NULL, 0);
            E->undo.suspend++;
            editorInsertNewline(E);
            E->undo.suspend--;
            i++;
            continue;
        }

        start = i;
        while (i < len && s[i] != '\n') i++;
        if (i > start) {
            if (filerow >= E->numrows) {
                while (E->numrows <= filerow) {
                    if (!E->undo.suspend)
                        undoPushAtom(E, U_INSERT_ROW, E->numrows, 0, "", 0);
                    E->undo.suspend++;
                    editorInsertRow(E, E->numrows, "", 0);
                    E->undo.suspend--;
                }
            }
            if (!E->undo.suspend)
                undoPushAtom(E, U_INSERT_TEXT, filerow, filecol, s + start, i - start);
            E->undo.suspend++;
            undoApplyInsertText(E, filerow, filecol, s + start, i - start);
            E->undo.suspend--;
            /* Advance cursor past inserted text on the line. */
            {
                int n = (int)(i - start);
                while (n--) {
                    if (E->cx == editorTextCols(E) - 1)
                        E->coloff++;
                    else
                        E->cx++;
                }
            }
            E->dirty++;
        }
    }

    if (!E->undo.suspend)
        undoEndGroup(E);
}
