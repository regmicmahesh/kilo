#include <stdlib.h>

#include "input.h"
#include "editor.h"
#include "fileio.h"
#include "output.h"
#include "search.h"
#include "terminal.h"
#include "undo.h"
#include "lsp.h"

#define KILO_QUIT_TIMES 3

/* Process events arriving from the standard input, which is, the user
 * is typing stuff on the terminal. */
void editorProcessKeypress(struct editorConfig *E, int fd) {
    /* When the file is modified, requires Ctrl-q to be pressed N times
     * before actually quitting. */
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey(fd);

    /* Completion popup navigation / accept. */
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
        /* Tab is Ctrl-I; many terminals also deliver Ctrl-Tab as Tab. */
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
        /* Other keys dismiss popup then fall through to normal handling,
         * except printable/trigger keys which may reopen it. */
        if (c != BACKSPACE && c != DEL_KEY && !(c >= 32 && c < 127))
            lspClearCompletion(E);
    }

    switch (c) {
    case ENTER:         /* Enter */
        lspClearCompletion(E);
        editorInsertNewline(E);
        if (E->lsp.enabled) lspDidChange(E);
        break;
    case CTRL_C:        /* Ctrl-c */
        /* We ignore ctrl-c, it can't be so simple to lose the changes
         * to the edited file. */
        break;
    case CTRL_Q:        /* Ctrl-q */
        /* Quit if the file was already saved. */
        if (E->dirty && quit_times) {
            editorSetStatusMessage(E, "WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        lspStop(E);
        exit(0);
        break;
    case CTRL_S:        /* Ctrl-s */
        editorSave(E);
        break;
    case CTRL_Z:        /* Ctrl-z undo */
        lspClearCompletion(E);
        editorUndo(E);
        if (E->lsp.enabled) lspDidChange(E);
        break;
    case CTRL_Y:        /* Ctrl-y redo */
        lspClearCompletion(E);
        editorRedo(E);
        if (E->lsp.enabled) lspDidChange(E);
        break;
    case CTRL_F:
        lspClearCompletion(E);
        editorFind(E, fd);
        break;
    case CTRL_H:        /* Ctrl-h: find and replace */
        lspClearCompletion(E);
        editorFindReplace(E, fd);
        break;
    case BACKSPACE:     /* Backspace */
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
            editorMoveCursor(E, c == PAGE_UP ? ARROW_UP :
                                            ARROW_DOWN);
        }
        break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        lspClearCompletion(E);
        editorMoveCursor(E, c);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        /* Just refresht the line as side effect. */
        break;
    case ESC:
        lspClearCompletion(E);
        break;
    case TAB:
        /* Insert tab character when no completion menu. */
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

    quit_times = KILO_QUIT_TIMES; /* Reset it to the original value. */
}
