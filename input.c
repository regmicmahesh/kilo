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

void editorProcessKeypress(struct editorConfig *E) {
    static int quit_times = KILO_QUIT_TIMES;
    int c;
    int wheel;

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

    c = guiWaitKey();

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
