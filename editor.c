#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "editor.h"
#include "syntax.h"
#include "terminal.h"
#include "output.h"
#include "undo.h"

/* Signal handlers have a fixed signature, so we keep one pointer solely
 * for SIGWINCH. It is set once from initEditor and never used as shared
 * mutable app state elsewhere. */
static struct editorConfig *sigwinch_editor;

void updateWindowSize(struct editorConfig *E) {
    if (getWindowSize(STDIN_FILENO, STDOUT_FILENO,
                      &E->screenrows, &E->screencols) == -1) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    E->screenrows -= 2; /* Get room for status bar. */
}

static void handleSigWinCh(int unused __attribute__((unused))) {
    struct editorConfig *E = sigwinch_editor;
    if (!E) return;
    updateWindowSize(E);
    if (E->cy > E->screenrows) E->cy = E->screenrows - 1;
    if (E->cx > E->screencols) E->cx = E->screencols - 1;
    editorRefreshScreen(E);
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
    E->rawmode = 0;
    E->statusmsg[0] = '\0';
    E->statusmsg_time = 0;
    undoInit(&E->undo);
    sigwinch_editor = E;
    updateWindowSize(E);
    signal(SIGWINCH, handleSigWinCh);
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
    if (E->cx == E->screencols - 1)
        E->coloff++;
    else
        E->cx++;
    E->dirty++;

    if (!E->undo.suspend && E->undo.undo_len > 0)
        undoEndGroup(E);
}

/* Inserting a newline is slightly complex as we have to handle inserting a
 * newline in the middle of a line, splitting the line as needed. */
void editorInsertNewline(struct editorConfig *E) {
    int filerow = E->rowoff + E->cy;
    int filecol = E->coloff + E->cx;
    erow *row = (filerow >= E->numrows) ? NULL : &E->row[filerow];

    if (!row) {
        if (filerow == E->numrows) {
            if (!E->undo.suspend) {
                undoBeginGroup(E);
                undoPushAtom(E, U_SPLIT_LINE, filerow, 0, NULL, 0);
            }
            editorInsertRow(E, filerow, "", 0);
            goto fixcursor;
        }
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
    } else {
        /* We are in the middle of a line. Split it between two rows. */
        editorInsertRow(E, filerow + 1, row->chars + filecol, row->size - filecol);
        row = &E->row[filerow];
        row->chars[filecol] = '\0';
        row->size = filecol;
        editorUpdateRow(E, row);
    }
fixcursor:
    if (E->cy == E->screenrows - 1) {
        E->rowoff++;
    } else {
        E->cy++;
    }
    E->cx = 0;
    E->coloff = 0;
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
        if (E->cx >= E->screencols) {
            int shift = (E->screencols - E->cx) + 1;
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
                    if (E->cx > E->screencols - 1) {
                        E->coloff = E->cx - E->screencols + 1;
                        E->cx = E->screencols - 1;
                    }
                }
            }
        } else {
            E->cx -= 1;
        }
        break;
    case ARROW_RIGHT:
        if (row && filecol < row->size) {
            if (E->cx == E->screencols - 1) {
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
                    if (E->cx == E->screencols - 1)
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
