#ifndef UNDO_H
#define UNDO_H

#include <sys/time.h>

struct editorConfig;

#define UNDO_STACK_MAX 1000
/* Consecutive inserts/deletes within this many ms merge into one undo unit. */
#define UNDO_GROUP_MS 1000

enum undoAtomType {
    U_INSERT_TEXT,  /* text was inserted at (row, col) */
    U_DELETE_TEXT,  /* text was deleted at (row, col); text holds removed chars */
    U_SPLIT_LINE,   /* newline inserted: line row split at col */
    U_JOIN_LINES,   /* backspace at col 0: line row+1 joined onto row at col */
    U_INSERT_ROW,   /* full row inserted at row (text = line contents) */
    U_DELETE_ROW    /* full row deleted at row (text = former contents) */
};

struct undoAtom {
    enum undoAtomType type;
    int row;
    int col;
    char *text;
    size_t len;
};

/* One user-visible undo/redo unit (may hold several atoms, e.g. paste). */
struct undoGroup {
    struct undoAtom *atoms;
    int natoms;
    int before_cx, before_cy, before_coloff, before_rowoff;
    int after_cx, after_cy, after_coloff, after_rowoff;
    struct timeval tv;
};

struct undoState {
    struct undoGroup *undo;
    int undo_len;
    int undo_cap;
    struct undoGroup *redo;
    int redo_len;
    int redo_cap;
    int suspend; /* non-zero while applying undo/redo or loading files */
};

void undoInit(struct undoState *u);
void undoFree(struct undoState *u);

/* Begin a new undo group capturing cursor-before. Cleared redo history. */
void undoBeginGroup(struct editorConfig *E);
/* Append an atom to the current (last) undo group. */
void undoPushAtom(struct editorConfig *E, enum undoAtomType type,
                  int row, int col, const char *text, size_t len);
/* Finish the group, storing cursor-after. Drops oldest if over UNDO_STACK_MAX. */
void undoEndGroup(struct editorConfig *E);

/* Try to merge a single inserted character into the last undo group.
 * Returns 1 if merged, 0 if caller should start a new group. */
int undoTryMergeInsertChar(struct editorConfig *E, int row, int col, int c);
/* Try to merge a single deleted character (backspace) into the last group. */
int undoTryMergeDeleteChar(struct editorConfig *E, int row, int col, int c);

void editorUndo(struct editorConfig *E);
void editorRedo(struct editorConfig *E);

/* Apply buffer mutations without recording (used by undo/redo). */
void undoApplyInsertText(struct editorConfig *E, int row, int col,
                         const char *text, size_t len);
void undoApplyDeleteText(struct editorConfig *E, int row, int col, size_t len);
void undoApplySplitLine(struct editorConfig *E, int row, int col);
void undoApplyJoinLines(struct editorConfig *E, int row);
void undoApplyInsertRow(struct editorConfig *E, int row, const char *text, size_t len);
void undoApplyDeleteRow(struct editorConfig *E, int row);

#endif
