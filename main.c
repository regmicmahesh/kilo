/* Kilo GUI — SDL2 + OpenGL port of antirez/kilo with multi-file structure,
 * undo/redo, find/replace, line numbers, and Node/TS LSP completions.
 *
 * Copyright (C) 2016 Salvatore Sanfilippo <antirez at gmail dot com>
 * GUI port modifications under the same BSD license terms.
 */

#include <stdio.h>
#include <stdlib.h>

#include "editor.h"
#include "fileio.h"
#include "gui.h"
#include "input.h"
#include "lsp.h"
#include "output.h"
#include "syntax.h"

int main(int argc, char **argv) {
    struct editorConfig E;

    if (argc != 2) {
        fprintf(stderr, "Usage: kilo <filename>\n");
        return 1;
    }

    if (guiInit(1100, 720, "kilo") != 0) {
        fprintf(stderr, "Failed to initialize GUI (SDL2/OpenGL).\n");
        return 1;
    }

    initEditor(&E);
    editorSelectSyntaxHighlight(&E, argv[1]);
    editorOpen(&E, argv[1]);
    lspStart(&E);
    editorSetStatusMessage(&E,
        "Ctrl-S save | Ctrl-F find | Ctrl-H replace | Ctrl-Z/Y undo | "
        "Ctrl-N/P completion | Tab accept | scroll wheel");

    while (!guiPollQuit()) {
        editorRefreshScreen(&E);
        editorProcessKeypress(&E);
    }

    lspStop(&E);
    guiShutdown();
    return 0;
}
