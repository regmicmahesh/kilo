#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"
#include "output.h"
#include "syntax.h"
#include "gui.h"
#include "undo.h"

#define KILO_QUERY_LEN 256
#define RE_NMATCH 10

struct searchMatch {
    int row;
    int col; /* offset in row->chars */
    int len; /* length in row->chars */
};

/* Map a character-column to a render-column (tabs expand). */
static int charToRenderCol(erow *row, int col) {
    int j, r = 0;
    if (col > row->size) col = row->size;
    for (j = 0; j < col; j++) {
        if (row->chars[j] == '\t') {
            r++;
            while ((r + 1) % 8 != 0) r++;
        } else {
            r++;
        }
    }
    return r;
}

static void restoreHl(struct editorConfig *E, int *saved_hl_line, char **saved_hl) {
    if (*saved_hl && *saved_hl_line >= 0 && *saved_hl_line < E->numrows) {
        erow *row = &E->row[*saved_hl_line];
        if (row->hl && row->rsize > 0)
            memcpy(row->hl, *saved_hl, row->rsize);
    }
    free(*saved_hl);
    *saved_hl = NULL;
    *saved_hl_line = -1;
}

static void highlightMatch(struct editorConfig *E, const struct searchMatch *m,
                           int *saved_hl_line, char **saved_hl) {
    erow *row;
    int rcol, rlen, j, end;

    restoreHl(E, saved_hl_line, saved_hl);
    if (!m || m->row < 0 || m->row >= E->numrows) return;
    row = &E->row[m->row];
    if (!row->hl || row->rsize == 0) return;

    rcol = charToRenderCol(row, m->col);
    end = charToRenderCol(row, m->col + m->len);
    rlen = end - rcol;
    if (rcol < 0) rcol = 0;
    if (rcol >= row->rsize) return;
    if (rcol + rlen > row->rsize) rlen = row->rsize - rcol;
    if (rlen <= 0) return;

    *saved_hl_line = m->row;
    *saved_hl = malloc(row->rsize);
    if (!*saved_hl) return;
    memcpy(*saved_hl, row->hl, row->rsize);
    for (j = 0; j < rlen; j++)
        row->hl[rcol + j] = HL_MATCH;
}

static void gotoMatch(struct editorConfig *E, const struct searchMatch *m) {
    int rcol;

    if (!m || m->row < 0) return;
    rcol = charToRenderCol(&E->row[m->row], m->col);
    E->cy = 0;
    E->cx = rcol;
    E->rowoff = m->row;
    E->coloff = 0;
    if (E->cx > editorTextCols(E)) {
        int diff = E->cx - editorTextCols(E);
        E->cx -= diff;
        E->coloff += diff;
    }
}

/* Find a match starting at/after (or before) (start_row, start_col) in chars. */
static int findMatch(struct editorConfig *E, regex_t *re, int start_row,
                     int start_col, int direction, struct searchMatch *out) {
    int n = E->numrows;
    int i;

    if (n == 0 || !re) return 0;

    if (direction >= 0) {
        int row = start_row;
        int col = start_col;
        if (row < 0) {
            row = 0;
            col = 0;
        }
        for (i = 0; i < n; i++) {
            int r = (row + i) % n;
            erow *line = &E->row[r];
            const char *s = line->chars ? line->chars : "";
            int off = (i == 0) ? col : 0;
            regmatch_t rm[RE_NMATCH];

            if (off > line->size) continue;
            while (off <= line->size) {
                if (regexec(re, s + off, RE_NMATCH, rm, 0) != 0)
                    break;
                /* Zero-length match: advance one char to avoid infinite loop. */
                if (rm[0].rm_so == rm[0].rm_eo) {
                    if (off >= line->size) break;
                    off++;
                    continue;
                }
                out->row = r;
                out->col = off + (int)rm[0].rm_so;
                out->len = (int)(rm[0].rm_eo - rm[0].rm_so);
                return 1;
            }
        }
    } else {
        /* Search backwards: scan each line for last match before start. */
        int row = start_row;
        int col = start_col;
        if (row < 0 || row >= n) {
            row = n - 1;
            col = E->row[row].size;
        }
        for (i = 0; i < n; i++) {
            int r = row - i;
            if (r < 0) r += n;
            erow *line = &E->row[r];
            const char *s = line->chars ? line->chars : "";
            int limit = (i == 0) ? col : line->size + 1;
            regmatch_t rm[RE_NMATCH];
            struct searchMatch best;
            int found = 0;
            int off = 0;

            best.row = r;
            best.col = -1;
            best.len = 0;
            while (off <= line->size) {
                if (regexec(re, s + off, RE_NMATCH, rm, 0) != 0)
                    break;
                if (rm[0].rm_so == rm[0].rm_eo) {
                    if (off >= line->size) break;
                    off++;
                    continue;
                }
                int mcol = off + (int)rm[0].rm_so;
                int mlen = (int)(rm[0].rm_eo - rm[0].rm_so);
                if (mcol < limit) {
                    best.col = mcol;
                    best.len = mlen;
                    found = 1;
                    off = mcol + 1;
                } else {
                    break;
                }
            }
            if (found) {
                *out = best;
                return 1;
            }
        }
    }
    return 0;
}

/* Expand replace string: & and \0 = full match, \1..\9 = groups. */
static char *expandReplacement(const char *repl, const char *line,
                               regmatch_t *rm, size_t *out_len) {
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    const char *p;

    if (!out) return NULL;
    for (p = repl; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p >= '0' && *p <= '9') {
                int g = *p - '0';
                if (rm[g].rm_so >= 0 && rm[g].rm_eo >= rm[g].rm_so) {
                    size_t gl = (size_t)(rm[g].rm_eo - rm[g].rm_so);
                    while (len + gl + 1 > cap) {
                        cap *= 2;
                        out = realloc(out, cap);
                    }
                    memcpy(out + len, line + rm[g].rm_so, gl);
                    len += gl;
                }
            } else if (*p == 'n') {
                if (len + 2 > cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = '\n';
            } else if (*p == '\\') {
                if (len + 2 > cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = '\\';
            } else {
                if (len + 2 > cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                out[len++] = *p;
            }
        } else if (*p == '&') {
            if (rm[0].rm_so >= 0) {
                size_t gl = (size_t)(rm[0].rm_eo - rm[0].rm_so);
                while (len + gl + 1 > cap) {
                    cap *= 2;
                    out = realloc(out, cap);
                }
                memcpy(out + len, line + rm[0].rm_so, gl);
                len += gl;
            }
        } else {
            if (len + 2 > cap) {
                cap *= 2;
                out = realloc(out, cap);
            }
            out[len++] = *p;
        }
    }
    out[len] = '\0';
    *out_len = len;
    return out;
}

static int compilePattern(regex_t *re, const char *pattern, int *compiled,
                          char *err, size_t errlen) {
    int rc;

    if (*compiled) {
        regfree(re);
        *compiled = 0;
    }
    if (!pattern[0]) {
        if (err && errlen) err[0] = '\0';
        return -1;
    }
    rc = regcomp(re, pattern, REG_EXTENDED);
    if (rc != 0) {
        if (err && errlen)
            regerror(rc, re, err, errlen);
        return -1;
    }
    *compiled = 1;
    if (err && errlen) err[0] = '\0';
    return 0;
}

/* Replace match at (row,col,len) with expanded replacement; records undo atoms
 * into the current group. Returns new cursor column after replacement. */
static int replaceOne(struct editorConfig *E, regex_t *re,
                      const struct searchMatch *m, const char *repl) {
    erow *row;
    regmatch_t rm[RE_NMATCH];
    char *expanded;
    size_t elen;
    char *old;
    int prev_suspend;

    if (m->row < 0 || m->row >= E->numrows) return -1;
    row = &E->row[m->row];
    if (m->col < 0 || m->col + m->len > row->size) return -1;

    if (regexec(re, row->chars + m->col, RE_NMATCH, rm, 0) != 0)
        return -1;
    /* regexec is on substring; adjust so expandReplacement sees full line. */
    {
        int k;
        for (k = 0; k < RE_NMATCH; k++) {
            if (rm[k].rm_so >= 0) {
                rm[k].rm_so += m->col;
                rm[k].rm_eo += m->col;
            }
        }
    }

    expanded = expandReplacement(repl, row->chars, rm, &elen);
    if (!expanded) return -1;

    old = malloc(m->len + 1);
    if (!old) {
        free(expanded);
        return -1;
    }
    memcpy(old, row->chars + m->col, m->len);
    old[m->len] = '\0';

    undoPushAtom(E, U_DELETE_TEXT, m->row, m->col, old, (size_t)m->len);
    undoPushAtom(E, U_INSERT_TEXT, m->row, m->col, expanded, elen);

    prev_suspend = E->undo.suspend;
    E->undo.suspend = 1;
    undoApplyDeleteText(E, m->row, m->col, (size_t)m->len);
    undoApplyInsertText(E, m->row, m->col, expanded, elen);
    E->undo.suspend = prev_suspend;
    E->dirty++;

    free(old);
    free(expanded);
    return m->col + (int)elen;
}

void editorFind(struct editorConfig *E) {
    char query[KILO_QUERY_LEN + 1] = {0};
    int qlen = 0;
    int last_row = -1, last_col = 0, last_len = 0;
    int have_match = 0;
    int find_next = 0;
    int saved_hl_line = -1;
    char *saved_hl = NULL;
    int saved_cx = E->cx, saved_cy = E->cy;
    int saved_coloff = E->coloff, saved_rowoff = E->rowoff;
    regex_t re;
    int re_ok = 0;
    char re_err[64] = {0};

    while (1) {
        if (re_err[0])
            editorSetStatusMessage(E, "Search: %s [%s]", query, re_err);
        else
            editorSetStatusMessage(E,
                "Search (regex): %s  (ESC/Arrows/Enter)", query);
        editorRefreshScreen(E);

        int c = guiWaitKey();
        if (c == DEL_KEY || c == BACKSPACE) {
            if (qlen != 0) query[--qlen] = '\0';
            have_match = 0;
            find_next = 1;
            compilePattern(&re, query, &re_ok, re_err, sizeof(re_err));
        } else if (c == ESC || c == ENTER) {
            if (c == ESC) {
                E->cx = saved_cx; E->cy = saved_cy;
                E->coloff = saved_coloff; E->rowoff = saved_rowoff;
            }
            restoreHl(E, &saved_hl_line, &saved_hl);
            if (re_ok) regfree(&re);
            editorSetStatusMessage(E, "");
            return;
        } else if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_next = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_next = -1;
        } else if (isprint(c)) {
            if (qlen < KILO_QUERY_LEN) {
                query[qlen++] = c;
                query[qlen] = '\0';
                have_match = 0;
                find_next = 1;
                compilePattern(&re, query, &re_ok, re_err, sizeof(re_err));
            }
        }

        if (!re_ok) {
            restoreHl(E, &saved_hl_line, &saved_hl);
            continue;
        }

        if (!have_match) find_next = 1;
        if (find_next) {
            struct searchMatch m;
            int srow, scol;
            int dir = find_next;

            if (have_match) {
                if (dir > 0) {
                    srow = last_row;
                    scol = last_col + (last_len > 0 ? last_len : 1);
                } else {
                    srow = last_row;
                    scol = last_col;
                }
            } else {
                srow = E->rowoff + E->cy;
                scol = 0;
                /* Start from current file position mapped roughly. */
                if (srow < E->numrows) {
                    /* Use beginning of current line for first search. */
                    scol = 0;
                }
                if (dir < 0) {
                    srow = E->rowoff + E->cy;
                    scol = E->numrows ? E->row[srow < E->numrows ? srow : 0].size : 0;
                }
            }

            if (findMatch(E, &re, srow, scol, dir, &m)) {
                have_match = 1;
                last_row = m.row;
                last_col = m.col;
                last_len = m.len;
                highlightMatch(E, &m, &saved_hl_line, &saved_hl);
                gotoMatch(E, &m);
            } else {
                restoreHl(E, &saved_hl_line, &saved_hl);
            }
            find_next = 0;
        }
    }
}

void editorFindReplace(struct editorConfig *E) {
    char findbuf[KILO_QUERY_LEN + 1] = {0};
    char replbuf[KILO_QUERY_LEN + 1] = {0};
    int flen = 0, rlen = 0;
    int focus = 0; /* 0 = find field, 1 = replace field */
    int last_row = -1, last_col = 0, last_len = 0;
    int have_match = 0;
    int find_dir = 0;
    int saved_hl_line = -1;
    char *saved_hl = NULL;
    int saved_cx = E->cx, saved_cy = E->cy;
    int saved_coloff = E->coloff, saved_rowoff = E->rowoff;
    regex_t re;
    int re_ok = 0;
    char re_err[64] = {0};

    while (1) {
        if (re_err[0])
            editorSetStatusMessage(E,
                "Replace >Find: %s [%s]  Repl: %s  (Tab|Enter|Ctrl-A|ESC)",
                findbuf, re_err, replbuf);
        else
            editorSetStatusMessage(E,
                "Replace %sFind: %s %sRepl: %s  (Tab|Enter|Ctrl-A|ESC|arrows)",
                focus == 0 ? ">" : " ", findbuf,
                focus == 1 ? ">" : " ", replbuf);
        editorRefreshScreen(E);

        int c = guiWaitKey();

        if (c == ESC) {
            E->cx = saved_cx; E->cy = saved_cy;
            E->coloff = saved_coloff; E->rowoff = saved_rowoff;
            restoreHl(E, &saved_hl_line, &saved_hl);
            if (re_ok) regfree(&re);
            editorSetStatusMessage(E, "");
            return;
        }

        if (c == CTRL_A) {
            /* Replace all occurrences as a single undo group. */
            struct searchMatch m;
            int count = 0;
            int srow = 0, scol = 0;

            if (!re_ok) {
                editorSetStatusMessage(E, "Invalid or empty find pattern");
                editorRefreshScreen(E);
                continue;
            }
            restoreHl(E, &saved_hl_line, &saved_hl);
            undoBeginGroup(E);
            while (findMatch(E, &re, srow, scol, 1, &m)) {
                int newcol = replaceOne(E, &re, &m, replbuf);
                if (newcol < 0) break;
                count++;
                srow = m.row;
                scol = newcol;
                /* Safety against infinite replace on zero-width patterns. */
                if (m.len == 0) scol++;
                if (count > 1000000) break;
            }
            undoEndGroup(E);
            have_match = 0;
            if (count)
                editorSetStatusMessage(E, "Replaced %d occurrence(s)", count);
            else
                editorSetStatusMessage(E, "No matches to replace");
            editorRefreshScreen(E);
            /* Stay in mode; re-show prompt next loop iteration. */
            continue;
        }

        if (c == ENTER) {
            if (!re_ok || !have_match) {
                /* Try to find first match. */
                struct searchMatch m;
                if (re_ok && findMatch(E, &re, 0, 0, 1, &m)) {
                    have_match = 1;
                    last_row = m.row;
                    last_col = m.col;
                    last_len = m.len;
                    highlightMatch(E, &m, &saved_hl_line, &saved_hl);
                    gotoMatch(E, &m);
                } else {
                    editorSetStatusMessage(E, "No match");
                    editorRefreshScreen(E);
                    continue;
                }
            }
            {
                struct searchMatch m = {last_row, last_col, last_len};
                int newcol;

                restoreHl(E, &saved_hl_line, &saved_hl);
                undoBeginGroup(E);
                newcol = replaceOne(E, &re, &m, replbuf);
                undoEndGroup(E);
                if (newcol < 0) {
                    have_match = 0;
                    continue;
                }
                /* Move to next match after replacement. */
                if (findMatch(E, &re, m.row, newcol + (m.len == 0 ? 1 : 0), 1, &m)) {
                    have_match = 1;
                    last_row = m.row;
                    last_col = m.col;
                    last_len = m.len;
                    highlightMatch(E, &m, &saved_hl_line, &saved_hl);
                    gotoMatch(E, &m);
                } else {
                    have_match = 0;
                    editorSetStatusMessage(E, "Replaced (no more matches)");
                }
            }
            continue;
        }

        if (c == 9 /* TAB */) {
            focus = focus ? 0 : 1;
            continue;
        }

        if (c == ARROW_RIGHT || c == ARROW_DOWN) {
            find_dir = 1;
        } else if (c == ARROW_LEFT || c == ARROW_UP) {
            find_dir = -1;
        } else if (c == DEL_KEY || c == BACKSPACE) {
            if (focus == 0) {
                if (flen > 0) {
                    findbuf[--flen] = '\0';
                    have_match = 0;
                    compilePattern(&re, findbuf, &re_ok, re_err, sizeof(re_err));
                    find_dir = 1;
                }
            } else {
                if (rlen > 0) replbuf[--rlen] = '\0';
            }
        } else if (isprint(c)) {
            if (focus == 0) {
                if (flen < KILO_QUERY_LEN) {
                    findbuf[flen++] = c;
                    findbuf[flen] = '\0';
                    have_match = 0;
                    compilePattern(&re, findbuf, &re_ok, re_err, sizeof(re_err));
                    find_dir = 1;
                }
            } else {
                if (rlen < KILO_QUERY_LEN) {
                    replbuf[rlen++] = c;
                    replbuf[rlen] = '\0';
                }
            }
        }

        if (find_dir && re_ok) {
            struct searchMatch m;
            int srow, scol;

            if (have_match) {
                if (find_dir > 0) {
                    srow = last_row;
                    scol = last_col + (last_len > 0 ? last_len : 1);
                } else {
                    srow = last_row;
                    scol = last_col;
                }
            } else {
                srow = 0;
                scol = 0;
            }
            if (findMatch(E, &re, srow, scol, find_dir, &m)) {
                have_match = 1;
                last_row = m.row;
                last_col = m.col;
                last_len = m.len;
                highlightMatch(E, &m, &saved_hl_line, &saved_hl);
                gotoMatch(E, &m);
            }
            find_dir = 0;
        } else if (find_dir && !re_ok) {
            restoreHl(E, &saved_hl_line, &saved_hl);
            find_dir = 0;
        }
    }
}
