#include <stdlib.h>

#include "input.h"
#include "editor.h"
#include "fileio.h"
#include "output.h"
#include "search.h"
#include "undo.h"
#include "lsp.h"
#include "gui.h"

#define KILO_QUIT_TIMES 3

/* Place cursor from a click in cell coordinates (window grid). */
static void editorClickGoto(struct editorConfig *E, int cell_col, int cell_row) {
    int gutter = editorGutterWidth(E);
    int textcols = editorTextCols(E);
    int filerow;
    int filecol;
    int rowlen;

    /* Ignore clicks on status / message bars. */
    if (cell_row < 0 || cell_row >= E->screenrows)
        return;

    filerow = E->rowoff + cell_row;
    E->cy = cell_row;

    /* Click in gutter → start of line. */
    if (cell_col <= gutter) {
        E->cx = 0;
        E->coloff = 0;
    } else {
        int screen_x = cell_col - gutter;
        if (screen_x >= textcols) screen_x = textcols - 1;
        if (screen_x < 0) screen_x = 0;
        filecol = E->coloff + screen_x;
        if (filerow >= 0 && filerow < E->numrows) {
            rowlen = E->row[filerow].size;
            if (filecol > rowlen) filecol = rowlen;
        } else {
            filecol = 0;
        }
        /* Keep cursor on-screen in the text area. */
        if (filecol < E->coloff) {
            E->coloff = filecol;
            E->cx = 0;
        } else if (filecol >= E->coloff + textcols) {
            E->coloff = filecol - textcols + 1;
            if (E->coloff < 0) E->coloff = 0;
            E->cx = filecol - E->coloff;
        } else {
            E->cx = filecol - E->coloff;
        }
    }

    /* If click is past EOF, clamp to last line. */
    if (E->numrows == 0) {
        E->cy = 0;
        E->cx = 0;
        E->rowoff = 0;
        E->coloff = 0;
    } else if (filerow >= E->numrows) {
        filerow = E->numrows - 1;
        if (filerow < E->rowoff) {
            E->rowoff = filerow;
            E->cy = 0;
        } else if (filerow >= E->rowoff + E->screenrows) {
            E->rowoff = filerow - E->screenrows + 1;
            E->cy = E->screenrows - 1;
        } else {
            E->cy = filerow - E->rowoff;
        }
        rowlen = E->row[filerow].size;
        filecol = E->coloff + E->cx;
        if (filecol > rowlen) {
            filecol = rowlen;
            if (filecol < E->coloff) {
                E->coloff = filecol;
                E->cx = 0;
            } else {
                E->cx = filecol - E->coloff;
            }
        }
    }

    lspClearCompletion(E);
}

void editorProcessKeypress(struct editorConfig *E) {
    static int quit_times = KILO_QUIT_TIMES;
    int c;
    int wheel;
    int ccol, crow;

    /* Mouse wheel scrolling */
    wheel = guiConsumeWheelLines();
    while (wheel < 0) {
        editorMoveCursor(E, ARROW_UP);
        wheel++;
    }
    while (wheel > 0) {
        editorMoveCursor(E, ARROW_DOWN);
        wheel--;
    }

    while (guiConsumeClick(&ccol, &crow)) {
        editorClickGoto(E, ccol, crow);
        quit_times = KILO_QUIT_TIMES;
        return;
    }

    c = guiWaitKey();
    if (c == MOUSE_CLICK) {
        if (guiConsumeClick(&ccol, &crow))
            editorClickGoto(E, ccol, crow);
        quit_times = KILO_QUIT_TIMES;
        return;
    }

    if (E->lsp.completion.active) {
        if (c == CTRL_N || c == ARROW_DOWN) {
            lspCompletionNext(E);
            quit_times = KILO_QUIT_TIMES;
            return;
        }
        if (c == CTRL_P || c == ARROW_UP) {
            lspCompletionPrev(E);
            quit_times = KILO_QUIT_TIMES;
            return;
        }
        if (c == TAB) {
            lspCompletionAccept(E);
            quit_times = KILO_QUIT_TIMES;
            return;
        }
        if (c == ESC) {
            lspClearCompletion(E);
            quit_times = KILO_QUIT_TIMES;
            return;
        }
        if (c != BACKSPACE && c != DEL_KEY && !(c >= 32 && c < 127))
            lspClearCompletion(E);
    }

    switch (c) {
    case ENTER:
        lspClearCompletion(E);
        editorInsertNewline(E);
        if (E->lsp.enabled) lspDidChange(E);
        break;
    case CTRL_C:
        break;
    case CTRL_Q:
        if (E->dirty && quit_times) {
            editorSetStatusMessage(E, "WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        lspStop(E);
        guiShutdown();
        exit(0);
        break;
    case CTRL_S:
        editorSave(E);
        break;
    case CTRL_Z:
        lspClearCompletion(E);
        editorUndo(E);
        if (E->lsp.enabled) lspDidChange(E);
        break;
    case CTRL_Y:
        lspClearCompletion(E);
        editorRedo(E);
        if (E->lsp.enabled) lspDidChange(E);
        break;
    case CTRL_F:
        lspClearCompletion(E);
        editorFind(E);
        break;
    case CTRL_H:
        lspClearCompletion(E);
        editorFindReplace(E);
        break;
    case BACKSPACE:
    case DEL_KEY:
        editorDelChar(E);
        if (E->lsp.enabled) {
            lspDidChange(E);
            if (E->lsp.completion.active)
                lspRequestCompletion(E);
            else
                lspClearCompletion(E);
        }
        break;
    case PAGE_UP:
    case PAGE_DOWN:
        lspClearCompletion(E);
        if (c == PAGE_UP && E->cy != 0)
            E->cy = 0;
        else if (c == PAGE_DOWN && E->cy != E->screenrows - 1)
            E->cy = E->screenrows - 1;
        {
            int times = E->screenrows;
            while (times--)
                editorMoveCursor(E, c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        lspClearCompletion(E);
        editorMoveCursor(E, c);
        break;
    case CTRL_L:
        break;
    case ESC:
        lspClearCompletion(E);
        break;
    case TAB:
        editorInsertChar(E, '\t');
        if (E->lsp.enabled) lspDidChange(E);
        break;
    default:
        if (c >= 32 && c < 127) {
            editorInsertChar(E, c);
            if (E->lsp.enabled) {
                lspDidChange(E);
                if (lspShouldTrigger(E, c))
                    lspRequestCompletion(E);
                else
                    lspClearCompletion(E);
            }
        }
        break;
    }

    quit_times = KILO_QUIT_TIMES;
}
