/* Core editor types and row/buffer operations. */
#ifndef EDITOR_H
#define EDITOR_H

#include <stddef.h>
#include <termios.h>
#include <time.h>

#include "undo.h"
#include "lsp.h"

#define KILO_VERSION "0.0.1"

struct editorSyntax;

/* A single line of the file we are editing. */
typedef struct erow {
    int idx;            /* Row index in the file, zero-based. */
    int size;           /* Size of the row, excluding the null term. */
    int rsize;          /* Size of the rendered row. */
    char *chars;        /* Row content. */
    char *render;       /* Row content "rendered" for screen (for TABs). */
    unsigned char *hl;  /* Syntax highlight type for each character in render. */
    int hl_oc;          /* Row had open comment at end in last syntax highlight. */
} erow;

struct editorConfig {
    int cx, cy;         /* Cursor x and y position in characters */
    int rowoff;         /* Offset of row displayed. */
    int coloff;         /* Offset of column displayed. */
    int screenrows;     /* Number of rows that we can show */
    int screencols;     /* Number of cols that we can show */
    int numrows;        /* Number of rows */
    int rawmode;        /* Is terminal raw mode enabled? */
    erow *row;          /* Rows */
    int dirty;          /* File modified but not saved. */
    char *filename;     /* Currently open filename */
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax; /* Current syntax highlight, or NULL. */
    struct termios orig_termios; /* Saved terminal state for restore on exit. */
    struct undoState undo;       /* Undo / redo stacks. */
    struct lspClient lsp;        /* Language server (Python autocomplete). */
};

enum KEY_ACTION {
    KEY_NULL = 0,
    CTRL_A = 1,         /* Replace all (in find/replace mode) */
    CTRL_C = 3,
    CTRL_D = 4,
    CTRL_F = 6,
    CTRL_H = 8,         /* Find and replace */
    TAB = 9,            /* Also accept completion when menu open (Ctrl+Tab) */
    CTRL_L = 12,
    ENTER = 13,
    CTRL_N = 14,        /* Next completion */
    CTRL_P = 16,        /* Previous completion */
    CTRL_Q = 17,
    CTRL_S = 19,
    CTRL_U = 21,
    CTRL_Y = 25,        /* Redo */
    CTRL_Z = 26,        /* Undo */
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
    PAGE_DOWN
};

void initEditor(struct editorConfig *E);
void updateWindowSize(struct editorConfig *E);

/* Line-number gutter width (digits + padding) and text-area columns. */
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

/* High-level edits recorded as single undo units (paste, indent, etc.). */
void editorUndoableInsertRow(struct editorConfig *E, int at, char *s, size_t len);
void editorUndoableDeleteRow(struct editorConfig *E, int at);
void editorUndoableInsertText(struct editorConfig *E, const char *s, size_t len);

int editorFileWasModified(struct editorConfig *E);

#endif
