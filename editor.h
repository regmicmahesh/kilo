/* Core editor types and row/buffer operations. */
#ifndef EDITOR_H
#define EDITOR_H

#include <stddef.h>
#include <time.h>

#include "undo.h"
#include "lsp.h"

#define KILO_VERSION "0.1.0-gui"

struct editorSyntax;

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_oc;
} erow;

struct editorConfig {
    int cx, cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[128];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct undoState undo;
    struct lspClient lsp;
    /* Text selection in file coordinates (row, char col). Inclusive start,
     * exclusive end when normalized (see editorSelectionNormalized). */
    int sel_active;
    int sel_anchor_row;
    int sel_anchor_col;
    int sel_caret_row;
    int sel_caret_col;
};

enum KEY_ACTION {
    KEY_NULL = 0,
    CTRL_A = 1,
    CTRL_C = 3,
    CTRL_D = 4,
    CTRL_F = 6,
    CTRL_H = 8,
    TAB = 9,
    CTRL_L = 12,
    ENTER = 13,
    CTRL_N = 14,
    CTRL_P = 16,
    CTRL_Q = 17,
    CTRL_S = 19,
    CTRL_U = 21,
    CTRL_Y = 25,
    CTRL_Z = 26,
    ESC = 27,
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    MOUSE_CLICK = 2000
};

void initEditor(struct editorConfig *E);
void updateWindowSize(struct editorConfig *E);

int editorGutterWidth(const struct editorConfig *E);
int editorTextCols(const struct editorConfig *E);

void editorUpdateRow(struct editorConfig *E, erow *row);
void editorInsertRow(struct editorConfig *E, int at, char *s, size_t len);
void editorFreeRow(erow *row);
void editorDelRow(struct editorConfig *E, int at);
char *editorRowsToString(struct editorConfig *E, int *buflen);

void editorRowInsertChar(struct editorConfig *E, erow *row, int at, int c);
void editorRowAppendString(struct editorConfig *E, erow *row, char *s, size_t len);
void editorRowDelChar(struct editorConfig *E, erow *row, int at);

void editorInsertChar(struct editorConfig *E, int c);
void editorInsertNewline(struct editorConfig *E);
void editorDelChar(struct editorConfig *E);
void editorMoveCursor(struct editorConfig *E, int key);

void editorUndoableInsertRow(struct editorConfig *E, int at, char *s, size_t len);
void editorUndoableDeleteRow(struct editorConfig *E, int at);
void editorUndoableInsertText(struct editorConfig *E, const char *s, size_t len);

int editorFileWasModified(struct editorConfig *E);

void editorClearSelection(struct editorConfig *E);
void editorSelectAll(struct editorConfig *E);
/* Write ordered selection bounds (start <= end in document order). */
void editorSelectionNormalized(const struct editorConfig *E,
                               int *sr, int *sc, int *er, int *ec);
/* True if file position (row, col) lies inside the selection. */
int editorPosSelected(const struct editorConfig *E, int row, int col);

#endif
