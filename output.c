#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "output.h"
#include "gui.h"
#include "syntax.h"
#include "lsp.h"

static void hlColor(int hl, float *r, float *g, float *b) {
    switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: *r = 0.4f; *g = 0.75f; *b = 0.75f; break;
    case HL_KEYWORD1:  *r = 0.95f; *g = 0.75f; *b = 0.35f; break;
    case HL_KEYWORD2:  *r = 0.45f; *g = 0.85f; *b = 0.45f; break;
    case HL_STRING:    *r = 0.85f; *g = 0.55f; *b = 0.85f; break;
    case HL_NUMBER:    *r = 0.95f; *g = 0.45f; *b = 0.45f; break;
    case HL_MATCH:     *r = 0.35f; *g = 0.65f; *b = 1.0f; break;
    case HL_NONPRINT:  *r = 0.7f; *g = 0.7f; *b = 0.7f; break;
    default:           *r = 0.90f; *g = 0.90f; *b = 0.92f; break;
    }
}

void editorRefreshScreen(struct editorConfig *E) {
    int y;
    int gutter = editorGutterWidth(E);
    int textcols = editorTextCols(E);
    int cur_filerow = E->rowoff + E->cy;
    char title[256];
    char gutterbuf[32];
    int status_row = E->screenrows;

    guiUpdateEditorSize(E);
    gutter = editorGutterWidth(E);
    textcols = editorTextCols(E);
    status_row = E->screenrows;

    snprintf(title, sizeof(title), "kilo — %s%s",
             E->filename ? E->filename : "[No Name]",
             E->dirty ? " *" : "");
    guiSetTitle(title);

    guiBeginFrame();

    /* Editor background */
    guiFillRect(0, 0, (float)guiWinW(), (float)(status_row * guiCellH()),
                0.11f, 0.12f, 0.15f, 1.0f);

    for (y = 0; y < E->screenrows; y++) {
        int filerow = E->rowoff + y;
        int is_current = (filerow == cur_filerow);
        int gwritten;

        if (filerow >= E->numrows) {
            snprintf(gutterbuf, sizeof(gutterbuf), "%*s", gutter, "");
            guiDrawTextCell(0, y, gutterbuf, gutter, 0.35f, 0.38f, 0.42f);
            guiDrawTextCell(gutter, y, "~", 1, 0.35f, 0.38f, 0.42f);
            if (E->numrows == 0 && y == E->screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kilo GUI editor — v%s (SDL/OpenGL)", KILO_VERSION);
                int padding = (textcols - welcomelen) / 2;
                if (padding < 0) padding = 0;
                guiDrawTextCell(gutter + padding, y, welcome, welcomelen,
                                0.7f, 0.75f, 0.85f);
            }
            continue;
        }

        /* Current line highlight across text area */
        if (is_current) {
            guiFillCellRow(0, y, E->screencols, 0.16f, 0.18f, 0.24f, 1.0f);
            guiFillCellRow(0, y, gutter, 0.20f, 0.35f, 0.55f, 1.0f);
        } else {
            guiFillCellRow(0, y, gutter, 0.13f, 0.14f, 0.17f, 1.0f);
        }

        gwritten = snprintf(gutterbuf, sizeof(gutterbuf), "%*d ",
                            gutter - 1, filerow + 1);
        if (gwritten > gutter) gwritten = gutter;
        if (is_current)
            guiDrawTextCell(0, y, gutterbuf, gwritten, 1.0f, 1.0f, 1.0f);
        else
            guiDrawTextCell(0, y, gutterbuf, gwritten, 0.45f, 0.48f, 0.55f);

        {
            erow *r = &E->row[filerow];
            int len = r->rsize - E->coloff;
            int j;
            if (len < 0) len = 0;
            if (len > textcols) len = textcols;
            for (j = 0; j < len; j++) {
                unsigned char *hl = r->hl ? r->hl + E->coloff : NULL;
                char ch = r->render[E->coloff + j];
                float cr, cg, cb;
                int hlt = hl ? hl[j] : HL_NORMAL;
                int filecol = E->coloff + j;
                int selected = editorPosSelected(E, filerow, filecol);

                if (selected)
                    guiFillCellRow(gutter + j, y, 1, 0.20f, 0.35f, 0.55f, 1.0f);

                if (hlt == HL_NONPRINT) {
                    char sym = (ch <= 26) ? (char)('@' + ch) : '?';
                    if (!selected)
                        guiFillCellRow(gutter + j, y, 1, 0.5f, 0.5f, 0.2f, 1.0f);
                    guiDrawTextCell(gutter + j, y, &sym, 1, 0.9f, 0.9f, 0.5f);
                } else {
                    hlColor(hlt, &cr, &cg, &cb);
                    if (hlt == HL_MATCH && !selected)
                        guiFillCellRow(gutter + j, y, 1, 0.15f, 0.25f, 0.45f, 1.0f);
                    if (selected) {
                        cr = 0.95f; cg = 0.97f; cb = 1.0f;
                    }
                    guiDrawTextCell(gutter + j, y, &ch, 1, cr, cg, cb);
                }
            }
            /* Highlight through end of line when selection covers EOL gap
             * (multi-line selection middle/start rows). */
            if (E->sel_active && len < textcols) {
                int sr, sc, er, ec;
                editorSelectionNormalized(E, &sr, &sc, &er, &ec);
                if (filerow >= sr && filerow < er) {
                    int from = len > 0 ? len : 0;
                    if (from < textcols)
                        guiFillCellRow(gutter + from, y, textcols - from,
                                        0.20f, 0.35f, 0.55f, 1.0f);
                }
            }
        }
    }

    /* Status bar */
    guiFillCellRow(0, status_row, E->screencols, 0.22f, 0.24f, 0.30f, 1.0f);
    {
        char status[128], rstatus[64];
        int len = snprintf(status, sizeof(status), " %s — %d lines%s ",
            E->filename ? E->filename : "[No Name]", E->numrows,
            E->dirty ? " (modified)" : "");
        int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d ",
            E->rowoff + E->cy + 1, E->numrows > 0 ? E->numrows : 1);
        if (len > E->screencols) len = E->screencols;
        guiDrawTextCell(0, status_row, status, len, 0.95f, 0.95f, 0.97f);
        if (rlen < E->screencols)
            guiDrawTextCell(E->screencols - rlen, status_row, rstatus, rlen,
                            0.75f, 0.78f, 0.85f);
    }

    /* Message bar */
    guiFillCellRow(0, status_row + 1, E->screencols, 0.14f, 0.15f, 0.18f, 1.0f);
    {
        int msglen = (int)strlen(E->statusmsg);
        if (msglen && time(NULL) - E->statusmsg_time < 8) {
            if (msglen > E->screencols) msglen = E->screencols;
            guiDrawTextCell(0, status_row + 1, E->statusmsg, msglen,
                            0.85f, 0.88f, 0.55f);
        }
    }

    /* Completion popup */
    if (E->lsp.completion.active && E->lsp.completion.nitems > 0) {
        struct lspCompletion *comp = &E->lsp.completion;
        int visible = comp->nitems < LSP_COMPLETION_VISIBLE
                          ? comp->nitems : LSP_COMPLETION_VISIBLE;
        int start = 0;
        int maxw = 20;
        int i, pop_row, pop_col;

        if (comp->selected >= LSP_COMPLETION_VISIBLE)
            start = comp->selected - LSP_COMPLETION_VISIBLE + 1;
        for (i = 0; i < comp->nitems; i++) {
            int w = (int)strlen(comp->items[i].label);
            if (w > maxw) maxw = w;
        }
        if (maxw > textcols - 2) maxw = textcols - 2;
        if (maxw < 8) maxw = 8;

        pop_row = E->cy + 1;
        if (pop_row + visible > E->screenrows)
            pop_row = E->cy - visible;
        if (pop_row < 0) pop_row = 0;
        pop_col = gutter + E->cx;
        if (pop_col + maxw + 2 > E->screencols)
            pop_col = E->screencols - maxw - 2;
        if (pop_col < gutter) pop_col = gutter;

        /* Shadow */
        for (i = 0; i < visible; i++)
            guiFillCellRow(pop_col + 1, pop_row + 1 + i, maxw + 2,
                            0.0f, 0.0f, 0.0f, 0.35f);

        for (i = 0; i < visible; i++) {
            int idx = start + i;
            const char *lab = comp->items[idx].label ? comp->items[idx].label : "";
            char item[256];
            int n, pad;
            int is_sel = (idx == comp->selected);

            if (is_sel)
                guiFillCellRow(pop_col, pop_row + i, maxw + 2,
                                0.15f, 0.40f, 0.75f, 1.0f);
            else
                guiFillCellRow(pop_col, pop_row + i, maxw + 2,
                                0.18f, 0.19f, 0.23f, 1.0f);

            n = snprintf(item, sizeof(item), "%c %.*s", is_sel ? '>' : ' ', maxw, lab);
            for (pad = n; pad < maxw + 2 && pad < (int)sizeof(item) - 1; pad++)
                item[pad] = ' ';
            item[pad] = '\0';
            if (is_sel)
                guiDrawTextCell(pop_col, pop_row + i, item, pad, 1.0f, 1.0f, 1.0f);
            else
                guiDrawTextCell(pop_col, pop_row + i, item, pad, 0.85f, 0.87f, 0.90f);
        }
    }

    /* Block cursor */
    {
        int ccol = gutter + E->cx;
        int crow = E->cy;
        guiFillCellRow(ccol, crow, 1, 0.85f, 0.88f, 0.95f, 0.85f);
        /* Redraw char under cursor in dark for contrast */
        if (cur_filerow < E->numrows) {
            erow *r = &E->row[cur_filerow];
            int idx = E->coloff + E->cx;
            if (idx >= 0 && idx < r->rsize) {
                char ch = r->render[idx];
                guiDrawTextCell(ccol, crow, &ch, 1, 0.1f, 0.1f, 0.12f);
            }
        }
    }

    guiEndFrame();
}

void editorSetStatusMessage(struct editorConfig *E, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E->statusmsg, sizeof(E->statusmsg), fmt, ap);
    va_end(ap);
    E->statusmsg_time = time(NULL);
}
