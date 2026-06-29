#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "output.h"
#include "buffer.h"
#include "syntax.h"
#include "lsp.h"

/* This function writes the whole screen using VT100 escape characters
 * starting from the logical state of the editor. */
void editorRefreshScreen(struct editorConfig *E) {
    int y;
    erow *r;
    char buf[64];
    struct abuf ab = ABUF_INIT;
    int gutter = editorGutterWidth(E);
    int textcols = editorTextCols(E);
    int cur_filerow = E->rowoff + E->cy;

    abAppend(&ab, "\x1b[?25l", 6); /* Hide cursor. */
    abAppend(&ab, "\x1b[H", 3); /* Go home. */
    for (y = 0; y < E->screenrows; y++) {
        int filerow = E->rowoff + y;
        int is_current = (filerow == cur_filerow);
        char gutterbuf[32];
        int gwritten;

        if (filerow >= E->numrows) {
            /* Empty gutter + tilde for lines past EOF. */
            snprintf(gutterbuf, sizeof(gutterbuf), "%*s", gutter, "");
            abAppend(&ab, "\x1b[90m", 5); /* bright black / grey */
            abAppend(&ab, gutterbuf, gutter);
            abAppend(&ab, "\x1b[39m", 5);
            if (E->numrows == 0 && y == E->screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- verison %s", KILO_VERSION);
                int padding = (textcols - welcomelen) / 2;
                if (padding < 0) padding = 0;
                if (padding) {
                    abAppend(&ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(&ab, " ", 1);
                if (welcomelen > textcols) welcomelen = textcols;
                abAppend(&ab, welcome, welcomelen);
                abAppend(&ab, "\x1b[0K\r\n", 6);
            } else {
                abAppend(&ab, "~\x1b[0K\r\n", 7);
            }
            continue;
        }

        /* Line number gutter (right-aligned), highlighted on current line. */
        gwritten = snprintf(gutterbuf, sizeof(gutterbuf), "%*d ",
                            gutter - 1, filerow + 1);
        if (gwritten > gutter) gwritten = gutter;
        if (is_current) {
            /* Reverse video + bold for the current line number. */
            abAppend(&ab, "\x1b[7m\x1b[1m", 8);
            abAppend(&ab, gutterbuf, gwritten);
            abAppend(&ab, "\x1b[0m", 4);
        } else {
            abAppend(&ab, "\x1b[90m", 5);
            abAppend(&ab, gutterbuf, gwritten);
            abAppend(&ab, "\x1b[39m", 5);
        }

        r = &E->row[filerow];

        /* Current line body: subtle reverse dim background via 7m on spaces
         * is too strong; use a soft inverted row prefix instead — apply
         * reverse only when drawing, with normal syntax colors preferred.
         * We use a dark background when the terminal supports it; fall back
         * to bold text for the whole current line when drawing HL_NORMAL. */
        if (is_current)
            abAppend(&ab, "\x1b[48;5;236m", 11); /* grey background */

        int len = r->rsize - E->coloff;
        int current_color = -1;
        if (len > 0) {
            if (len > textcols) len = textcols;
            char *c = r->render + E->coloff;
            unsigned char *hl = r->hl + E->coloff;
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NONPRINT) {
                    char sym;
                    abAppend(&ab, "\x1b[7m", 4);
                    if (c[j] <= 26)
                        sym = '@' + c[j];
                    else
                        sym = '?';
                    abAppend(&ab, &sym, 1);
                    abAppend(&ab, "\x1b[0m", 4);
                    if (is_current)
                        abAppend(&ab, "\x1b[48;5;236m", 11);
                    current_color = -1;
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(&ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(&ab, c + j, 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        char cbuf[16];
                        int clen = snprintf(cbuf, sizeof(cbuf), "\x1b[%dm", color);
                        current_color = color;
                        abAppend(&ab, cbuf, clen);
                    }
                    abAppend(&ab, c + j, 1);
                }
            }
        }

        /* Extend current-line background to the end of the text area. */
        if (is_current) {
            int pad = textcols - (len > 0 ? len : 0);
            if (pad < 0) pad = 0;
            while (pad--) abAppend(&ab, " ", 1);
            abAppend(&ab, "\x1b[49m", 5); /* reset background */
        }

        abAppend(&ab, "\x1b[39m", 5);
        abAppend(&ab, "\x1b[0K", 4);
        abAppend(&ab, "\r\n", 2);
    }

    /* Create a two rows status. First row: */
    abAppend(&ab, "\x1b[0K", 4);
    abAppend(&ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E->filename, E->numrows, E->dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus),
        "%d/%d", E->rowoff + E->cy + 1, E->numrows);
    if (len > E->screencols) len = E->screencols;
    abAppend(&ab, status, len);
    while (len < E->screencols) {
        if (E->screencols - len == rlen) {
            abAppend(&ab, rstatus, rlen);
            break;
        } else {
            abAppend(&ab, " ", 1);
            len++;
        }
    }
    abAppend(&ab, "\x1b[0m\r\n", 6);

    /* Second row depends on E->statusmsg and the status message update time. */
    abAppend(&ab, "\x1b[0K", 4);
    int msglen = strlen(E->statusmsg);
    if (msglen && time(NULL) - E->statusmsg_time < 5)
        abAppend(&ab, E->statusmsg, msglen <= E->screencols ? msglen : E->screencols);

    /* Cursor in text area (1-based), shifted right by the gutter. */
    int j;
    int cx = 1;
    int filerow = E->rowoff + E->cy;
    erow *row = (filerow >= E->numrows) ? NULL : &E->row[filerow];
    if (row) {
        for (j = E->coloff; j < (E->cx + E->coloff); j++) {
            if (j < row->size && row->chars[j] == TAB) cx += 7 - ((cx) % 8);
            cx++;
        }
    }
    cx += gutter;

    /* Completion popup near the cursor. */
    if (E->lsp.completion.active && E->lsp.completion.nitems > 0) {
        struct lspCompletion *comp = &E->lsp.completion;
        int visible = comp->nitems < LSP_COMPLETION_VISIBLE
                          ? comp->nitems
                          : LSP_COMPLETION_VISIBLE;
        int start = 0;
        int maxw = 20;
        int i, row_screen, col_screen;

        if (comp->selected >= LSP_COMPLETION_VISIBLE)
            start = comp->selected - LSP_COMPLETION_VISIBLE + 1;
        for (i = 0; i < comp->nitems; i++) {
            int w = (int)strlen(comp->items[i].label);
            if (w > maxw) maxw = w;
        }
        if (maxw > textcols - 2) maxw = textcols - 2;
        if (maxw < 8) maxw = 8;

        row_screen = E->cy + 2; /* 1-based, prefer below cursor line */
        if (row_screen + visible > E->screenrows)
            row_screen = E->cy > 0 ? E->cy : 1; /* draw above */
        if (row_screen < 1) row_screen = 1;
        col_screen = cx;
        if (col_screen + maxw + 2 > E->screencols)
            col_screen = E->screencols - maxw - 2;
        if (col_screen < gutter + 1) col_screen = gutter + 1;

        for (i = 0; i < visible; i++) {
            int idx = start + i;
            const char *lab = comp->items[idx].label ? comp->items[idx].label : "";
            char item[256];
            int n, pad;
            int is_sel = (idx == comp->selected);

            snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row_screen + i, col_screen);
            abAppend(&ab, buf, strlen(buf));
            if (is_sel) {
                /* Bright blue background, bold white text — clear selection. */
                abAppend(&ab, "\x1b[48;5;33m\x1b[1m\x1b[97m", 18);
            } else {
                /* Dim grey background, normal light text. */
                abAppend(&ab, "\x1b[48;5;236m\x1b[37m", 15);
            }
            /* Marker so selection is obvious even without color. */
            n = snprintf(item, sizeof(item), "%c%.*s", is_sel ? '>' : ' ', maxw, lab);
            for (pad = n; pad < maxw + 2 && pad < (int)sizeof(item) - 1; pad++)
                item[pad] = ' ';
            item[pad] = '\0';
            abAppend(&ab, item, pad);
            abAppend(&ab, "\x1b[0m", 4);
        }
    }

    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E->cy + 1, cx);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6); /* Show cursor. */
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/* Set an editor status message for the second line of the status, at the
 * end of the screen. */
void editorSetStatusMessage(struct editorConfig *E, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E->statusmsg, sizeof(E->statusmsg), fmt, ap);
    va_end(ap);
    E->statusmsg_time = time(NULL);
}
