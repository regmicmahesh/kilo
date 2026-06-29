#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lsp.h"
#include "editor.h"
#include "output.h"
#include "undo.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void lspInit(struct lspClient *lsp) {
    memset(lsp, 0, sizeof(*lsp));
    lsp->write_fd = -1;
    lsp->read_fd = -1;
    lsp->pid = -1;
    lsp->next_id = 1;
}

static void freeCompletionItems(struct lspCompletion *c) {
    int i;
    for (i = 0; i < c->nitems; i++) {
        free(c->items[i].label);
        free(c->items[i].insertText);
        c->items[i].label = NULL;
        c->items[i].insertText = NULL;
    }
    c->nitems = 0;
    c->selected = 0;
    c->active = 0;
}

void lspClearCompletion(struct editorConfig *E) {
    freeCompletionItems(&E->lsp.completion);
}

void lspFree(struct lspClient *lsp) {
    freeCompletionItems(&lsp->completion);
    free(lsp->root_uri);
    free(lsp->doc_uri);
    lsp->root_uri = NULL;
    lsp->doc_uri = NULL;
    if (lsp->write_fd >= 0) close(lsp->write_fd);
    if (lsp->read_fd >= 0) close(lsp->read_fd);
    if (lsp->pid > 0) {
        kill(lsp->pid, SIGTERM);
        waitpid(lsp->pid, NULL, 0);
    }
    lsp->write_fd = -1;
    lsp->read_fd = -1;
    lsp->pid = -1;
    lsp->ready = 0;
    lsp->enabled = 0;
}

/* --- JSON helpers --- */

static void abGrow(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        while (nc < *len + n + 1) nc *= 2;
        *buf = realloc(*buf, nc);
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void jsonEscapeAppend(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char tmp[8];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\'; tmp[1] = (char)c; tmp[2] = '\0';
            abGrow(buf, len, cap, tmp, 2);
        } else if (c == '\n') {
            abGrow(buf, len, cap, "\\n", 2);
        } else if (c == '\r') {
            abGrow(buf, len, cap, "\\r", 2);
        } else if (c == '\t') {
            abGrow(buf, len, cap, "\\t", 2);
        } else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            abGrow(buf, len, cap, tmp, 6);
        } else {
            tmp[0] = (char)c; tmp[1] = '\0';
            abGrow(buf, len, cap, tmp, 1);
        }
    }
}

static char *pathToFileUri(const char *path) {
    char resolved[PATH_MAX];
    const char *p;
    char *uri;
    size_t len = 0, cap = 0;

    if (realpath(path, resolved))
        p = resolved;
    else
        p = path;

    uri = NULL;
    abGrow(&uri, &len, &cap, "file://", 7);
    /* Simple encode: escape non-path-safe bytes minimally. */
    {
        const char *s = p;
        while (*s) {
            unsigned char c = (unsigned char)*s;
            if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '~') {
                abGrow(&uri, &len, &cap, s, 1);
            } else if (c == ' ') {
                abGrow(&uri, &len, &cap, "%20", 3);
            } else {
                char tmp[4];
                snprintf(tmp, sizeof(tmp), "%%%02X", c);
                abGrow(&uri, &len, &cap, tmp, 3);
            }
            s++;
        }
    }
    return uri;
}

static char *dirname_uri(const char *file_uri) {
    char *copy = strdup(file_uri);
    char *slash;
    if (!copy) return strdup("file:///");
    slash = strrchr(copy, '/');
    if (slash && slash > copy + 7) {
        *slash = '\0';
        return copy;
    }
    free(copy);
    return strdup("file:///");
}

/* --- Process / RPC --- */

static int lspWriteRaw(struct lspClient *lsp, const char *body, size_t body_len) {
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", body_len);
    if (lsp->write_fd < 0) return -1;
    if (write(lsp->write_fd, header, hlen) != hlen) return -1;
    if (write(lsp->write_fd, body, body_len) != (ssize_t)body_len) return -1;
    return 0;
}

/* Read one LSP message with timeout_ms. Caller frees *out_body. */
static int lspReadMessage(struct lspClient *lsp, char **out_body, size_t *out_len,
                          int timeout_ms) {
    char header[4096];
    size_t hlen = 0;
    long content_length = -1;
    char *body;
    size_t got = 0;

    *out_body = NULL;
    *out_len = 0;
    if (lsp->read_fd < 0) return -1;

    /* Read headers until \r\n\r\n */
    while (hlen + 1 < sizeof(header)) {
        fd_set rfds;
        struct timeval tv;
        int rv;
        char c;

        FD_ZERO(&rfds);
        FD_SET(lsp->read_fd, &rfds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rv = select(lsp->read_fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) return -1;
        if (read(lsp->read_fd, &c, 1) != 1) return -1;
        header[hlen++] = c;
        if (hlen >= 4 && header[hlen - 4] == '\r' && header[hlen - 3] == '\n' &&
            header[hlen - 2] == '\r' && header[hlen - 1] == '\n')
            break;
        timeout_ms = 2000; /* subsequent bytes wait less aggressively as one msg */
    }
    header[hlen] = '\0';

    {
        char *p = header;
        while (*p) {
            if (!strncasecmp(p, "Content-Length:", 15)) {
                content_length = strtol(p + 15, NULL, 10);
                break;
            }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    if (content_length < 0 || content_length > 8 * 1024 * 1024) return -1;

    body = malloc((size_t)content_length + 1);
    if (!body) return -1;
    while (got < (size_t)content_length) {
        fd_set rfds;
        struct timeval tv;
        ssize_t n;
        FD_ZERO(&rfds);
        FD_SET(lsp->read_fd, &rfds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        if (select(lsp->read_fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            free(body);
            return -1;
        }
        n = read(lsp->read_fd, body + got, (size_t)content_length - got);
        if (n <= 0) {
            free(body);
            return -1;
        }
        got += (size_t)n;
    }
    body[content_length] = '\0';
    *out_body = body;
    *out_len = (size_t)content_length;
    return 0;
}

/* Wait for response with matching id; ignore notifications / other messages. */
static char *lspWaitResponse(struct lspClient *lsp, int id, int timeout_ms) {
    int spins = 0;
    while (spins++ < 50) {
        char *body;
        size_t len;
        char idbuf[32];

        if (lspReadMessage(lsp, &body, &len, timeout_ms) != 0)
            return NULL;
        snprintf(idbuf, sizeof(idbuf), "\"id\":%d", id);
        /* also allow "id": 1 with space */
        if (strstr(body, idbuf) ||
            (snprintf(idbuf, sizeof(idbuf), "\"id\": %d", id) > 0 && strstr(body, idbuf))) {
            /* Ensure it's a response (has result or error), not a request echoing id */
            if (strstr(body, "\"result\"") || strstr(body, "\"error\""))
                return body;
        }
        free(body);
    }
    return NULL;
}

static int lspNotify(struct lspClient *lsp, const char *method, const char *params_json) {
    char *body = NULL;
    size_t len = 0, cap = 0;
    int rc;
    abGrow(&body, &len, &cap, "{\"jsonrpc\":\"2.0\",\"method\":\"", 28);
    abGrow(&body, &len, &cap, method, strlen(method));
    abGrow(&body, &len, &cap, "\",\"params\":", 11);
    abGrow(&body, &len, &cap, params_json, strlen(params_json));
    abGrow(&body, &len, &cap, "}", 1);
    rc = lspWriteRaw(lsp, body, len);
    free(body);
    return rc;
}

static char *lspRequest(struct lspClient *lsp, const char *method, const char *params_json,
                        int timeout_ms) {
    int id = lsp->next_id++;
    char *body = NULL;
    size_t len = 0, cap = 0;
    char idbuf[16];
    char *resp;

    snprintf(idbuf, sizeof(idbuf), "%d", id);
    abGrow(&body, &len, &cap, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    abGrow(&body, &len, &cap, idbuf, strlen(idbuf));
    abGrow(&body, &len, &cap, ",\"method\":\"", 11);
    abGrow(&body, &len, &cap, method, strlen(method));
    abGrow(&body, &len, &cap, "\",\"params\":", 11);
    abGrow(&body, &len, &cap, params_json, strlen(params_json));
    abGrow(&body, &len, &cap, "}", 1);
    if (lspWriteRaw(lsp, body, len) != 0) {
        free(body);
        return NULL;
    }
    free(body);
    resp = lspWaitResponse(lsp, id, timeout_ms);
    return resp;
}

static int spawnServer(struct lspClient *lsp, char *const argv[]) {
    int in_pipe[2], out_pipe[2];
    pid_t pid;

    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        /* Keep stderr for debugging or silence it */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    lsp->pid = pid;
    lsp->write_fd = in_pipe[1];
    lsp->read_fd = out_pipe[0];
    return 0;
}

static int tryStartServer(struct lspClient *lsp) {
    /* Prefer python-lsp-server, then pyright, then jedi-language-server. */
    char *attempts[][4] = {
        {"pylsp", NULL, NULL, NULL},
        {"python3", "-m", "pylsp", NULL},
        {"pyright-langserver", "--stdio", NULL, NULL},
        {"jedi-language-server", NULL, NULL, NULL},
    };
    size_t i;
    for (i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        if (spawnServer(lsp, attempts[i]) != 0) continue;
        /* Probe with initialize quickly — if exec failed, read fails. */
        return 0;
    }
    return -1;
}

int lspStartPython(struct editorConfig *E) {
    struct lspClient *lsp = &E->lsp;
    char *params = NULL;
    size_t plen = 0, pcap = 0;
    char *resp;
    const char *fn = E->filename ? E->filename : "";

    lspStop(E);
    lspInit(lsp);

    /* Only for Python sources. */
    {
        size_t n = strlen(fn);
        if (n < 3 || strcmp(fn + n - 3, ".py") != 0) {
            lsp->enabled = 0;
            return 0;
        }
    }

    if (tryStartServer(lsp) != 0) {
        editorSetStatusMessage(E,
            "LSP: no Python server (install python-lsp-server: pip install python-lsp-server)");
        return -1;
    }

    lsp->doc_uri = pathToFileUri(fn);
    lsp->root_uri = dirname_uri(lsp->doc_uri);
    lsp->version = 1;

    abGrow(&params, &plen, &pcap,
           "{\"processId\":null,\"rootUri\":\"", 28);
    abGrow(&params, &plen, &pcap, lsp->root_uri, strlen(lsp->root_uri));
    abGrow(&params, &plen, &pcap,
           "\",\"capabilities\":{\"textDocument\":{\"completion\":{"
           "\"completionItem\":{\"snippetSupport\":false}}}},"
           "\"workspaceFolders\":null}", 120);

    resp = lspRequest(lsp, "initialize", params, 5000);
    free(params);
    if (!resp) {
        editorSetStatusMessage(E, "LSP: initialize failed (is pylsp installed?)");
        lspStop(E);
        return -1;
    }
    free(resp);

    lspNotify(lsp, "initialized", "{}");
    lsp->ready = 1;
    lsp->enabled = 1;
    lspDidOpen(E);
    editorSetStatusMessage(E, "LSP: Python language server ready");
    return 0;
}

void lspStop(struct editorConfig *E) {
    if (E->lsp.ready && E->lsp.doc_uri)
        lspDidClose(E);
    lspFree(&E->lsp);
    lspInit(&E->lsp);
}

static char *documentParams(struct editorConfig *E, int with_text) {
    char *p = NULL;
    size_t len = 0, cap = 0;
    char ver[16];

    snprintf(ver, sizeof(ver), "%d", E->lsp.version);
    abGrow(&p, &len, &cap, "{\"textDocument\":{\"uri\":\"", 24);
    abGrow(&p, &len, &cap, E->lsp.doc_uri, strlen(E->lsp.doc_uri));
    if (with_text) {
        int buflen = 0;
        char *text = editorRowsToString(E, &buflen);
        abGrow(&p, &len, &cap, "\",\"languageId\":\"python\",\"version\":", 34);
        abGrow(&p, &len, &cap, ver, strlen(ver));
        abGrow(&p, &len, &cap, ",\"text\":\"", 9);
        if (text)
            jsonEscapeAppend(&p, &len, &cap, text, (size_t)buflen);
        free(text);
        abGrow(&p, &len, &cap, "\"}}", 3);
    } else {
        abGrow(&p, &len, &cap, "\"}}", 3);
    }
    return p;
}

void lspDidOpen(struct editorConfig *E) {
    char *p;
    if (!E->lsp.ready) return;
    p = documentParams(E, 1);
    lspNotify(&E->lsp, "textDocument/didOpen", p);
    free(p);
}

void lspDidChange(struct editorConfig *E) {
    char *p = NULL;
    size_t len = 0, cap = 0;
    char ver[16];
    int buflen = 0;
    char *text;

    if (!E->lsp.ready) return;
    E->lsp.version++;
    snprintf(ver, sizeof(ver), "%d", E->lsp.version);
    text = editorRowsToString(E, &buflen);
    abGrow(&p, &len, &cap, "{\"textDocument\":{\"uri\":\"", 24);
    abGrow(&p, &len, &cap, E->lsp.doc_uri, strlen(E->lsp.doc_uri));
    abGrow(&p, &len, &cap, "\",\"version\":", 12);
    abGrow(&p, &len, &cap, ver, strlen(ver));
    abGrow(&p, &len, &cap,
           "},\"contentChanges\":[{\"text\":\"", 29);
    if (text)
        jsonEscapeAppend(&p, &len, &cap, text, (size_t)buflen);
    free(text);
    abGrow(&p, &len, &cap, "\"}]}", 4);
    lspNotify(&E->lsp, "textDocument/didChange", p);
    free(p);
}

void lspDidClose(struct editorConfig *E) {
    char *p;
    if (!E->lsp.ready || !E->lsp.doc_uri) return;
    p = documentParams(E, 0);
    lspNotify(&E->lsp, "textDocument/didClose", p);
    free(p);
}

/* Extract JSON string value after key; returns malloc'd unescaped string. */
static char *jsonExtractString(const char *json, const char *key) {
    char pattern[128];
    const char *p, *start;
    char *out;
    size_t len = 0, cap = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    start = p;
    out = NULL;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') abGrow(&out, &len, &cap, "\n", 1);
            else if (*p == 't') abGrow(&out, &len, &cap, "\t", 1);
            else if (*p == 'r') abGrow(&out, &len, &cap, "\r", 1);
            else if (*p == '"' || *p == '\\' || *p == '/')
                abGrow(&out, &len, &cap, p, 1);
            else if (*p == 'u' && p[1] && p[2] && p[3] && p[4])
                p += 4; /* skip unicode for labels */
            else
                abGrow(&out, &len, &cap, p, 1);
            p++;
        } else {
            abGrow(&out, &len, &cap, p, 1);
            p++;
        }
        (void)start;
    }
    if (!out) {
        out = malloc(1);
        if (out) out[0] = '\0';
    }
    return out;
}

/* Parse completion items from result JSON (array or {items:[]}). */
static void parseCompletionResult(struct lspCompletion *comp, const char *json) {
    const char *p;
    const char *items = strstr(json, "\"items\"");
    freeCompletionItems(comp);

    if (items) {
        p = strchr(items, '[');
    } else {
        p = strstr(json, "\"result\"");
        if (!p) return;
        p = strchr(p, '[');
    }
    if (!p) return;
    p++; /* inside array */

    while (*p && *p != ']' && comp->nitems < LSP_COMPLETION_MAX) {
        const char *obj;
        char *label, *insertText;
        const char *end;

        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        obj = p;
        /* Find matching end brace at depth 1 — simple scan */
        {
            int depth = 0;
            const char *q = p;
            int in_str = 0;
            end = NULL;
            for (; *q; q++) {
                if (in_str) {
                    if (*q == '\\' && q[1]) { q++; continue; }
                    if (*q == '"') in_str = 0;
                    continue;
                }
                if (*q == '"') { in_str = 1; continue; }
                if (*q == '{') depth++;
                else if (*q == '}') {
                    depth--;
                    if (depth == 0) { end = q; break; }
                }
            }
            if (!end) break;
        }
        {
            size_t objlen = (size_t)(end - obj + 1);
            char *objbuf = malloc(objlen + 1);
            if (!objbuf) break;
            memcpy(objbuf, obj, objlen);
            objbuf[objlen] = '\0';
            label = jsonExtractString(objbuf, "label");
            insertText = jsonExtractString(objbuf, "insertText");
            free(objbuf);
        }
        if (label) {
            int i = comp->nitems++;
            comp->items[i].label = label;
            comp->items[i].insertText = insertText ? insertText : strdup(label);
        } else {
            free(insertText);
        }
        p = end + 1;
    }
    if (comp->nitems > 0) {
        comp->active = 1;
        comp->selected = 0;
    }
}

static int isIdentChar(int c) {
    return isalnum(c) || c == '_';
}

/* Word start column for completion replace range. */
static int completionStartCol(struct editorConfig *E, int row, int col) {
    erow *r;
    if (row < 0 || row >= E->numrows) return col;
    r = &E->row[row];
    if (col > r->size) col = r->size;
    while (col > 0 && isIdentChar((unsigned char)r->chars[col - 1]))
        col--;
    return col;
}

int lspShouldTrigger(struct editorConfig *E, int c) {
    if (!E->lsp.enabled || !E->lsp.ready) return 0;
    if (c == '.') return 1;
    if (isIdentChar(c)) return 1;
    return 0;
}

int lspRequestCompletion(struct editorConfig *E) {
    int filerow = E->rowoff + E->cy;
    int filecol = E->coloff + E->cx;
    char *params = NULL;
    size_t len = 0, cap = 0;
    char linebuf[16], charbuf[16];
    char *resp;

    if (!E->lsp.ready) return -1;
    if (filerow < 0) return -1;

    /* Sync buffer to server before completing. */
    lspDidChange(E);

    snprintf(linebuf, sizeof(linebuf), "%d", filerow);
    snprintf(charbuf, sizeof(charbuf), "%d", filecol);

    abGrow(&params, &len, &cap, "{\"textDocument\":{\"uri\":\"", 24);
    abGrow(&params, &len, &cap, E->lsp.doc_uri, strlen(E->lsp.doc_uri));
    abGrow(&params, &len, &cap, "\"},\"position\":{\"line\":", 22);
    abGrow(&params, &len, &cap, linebuf, strlen(linebuf));
    abGrow(&params, &len, &cap, ",\"character\":", 13);
    abGrow(&params, &len, &cap, charbuf, strlen(charbuf));
    abGrow(&params, &len, &cap, "}}", 2);

    resp = lspRequest(&E->lsp, "textDocument/completion", params, 3000);
    free(params);
    if (!resp) {
        lspClearCompletion(E);
        return -1;
    }
    parseCompletionResult(&E->lsp.completion, resp);
    free(resp);

    E->lsp.completion.start_row = filerow;
    E->lsp.completion.start_col = completionStartCol(E, filerow, filecol);
    if (E->lsp.completion.nitems == 0)
        lspClearCompletion(E);
    return E->lsp.completion.active ? 0 : -1;
}

void lspCompletionNext(struct editorConfig *E) {
    struct lspCompletion *c = &E->lsp.completion;
    if (!c->active || c->nitems == 0) return;
    c->selected = (c->selected + 1) % c->nitems;
}

void lspCompletionPrev(struct editorConfig *E) {
    struct lspCompletion *c = &E->lsp.completion;
    if (!c->active || c->nitems == 0) return;
    c->selected = (c->selected - 1 + c->nitems) % c->nitems;
}

int lspCompletionAccept(struct editorConfig *E) {
    struct lspCompletion *c = &E->lsp.completion;
    const char *text;
    size_t tlen;
    int filerow, filecol;
    int start_col, del_len;
    erow *row;

    if (!c->active || c->nitems == 0) return 0;
    if (c->selected < 0 || c->selected >= c->nitems) return 0;

    text = c->items[c->selected].insertText;
    if (!text) text = c->items[c->selected].label;
    if (!text) return 0;
    tlen = strlen(text);

    filerow = E->rowoff + E->cy;
    filecol = E->coloff + E->cx;
    start_col = c->start_col;
    if (filerow != c->start_row) {
        lspClearCompletion(E);
        return 0;
    }
    if (filerow < 0 || filerow >= E->numrows) return 0;
    row = &E->row[filerow];
    if (start_col < 0) start_col = 0;
    if (start_col > filecol) start_col = filecol;
    if (filecol > row->size) filecol = row->size;
    del_len = filecol - start_col;

    undoBeginGroup(E);
    if (del_len > 0) {
        char *old = malloc(del_len + 1);
        if (old) {
            memcpy(old, row->chars + start_col, del_len);
            old[del_len] = '\0';
            undoPushAtom(E, U_DELETE_TEXT, filerow, start_col, old, (size_t)del_len);
            free(old);
        }
        {
            int prev = E->undo.suspend;
            E->undo.suspend = 1;
            undoApplyDeleteText(E, filerow, start_col, (size_t)del_len);
            E->undo.suspend = prev;
        }
    }
    undoPushAtom(E, U_INSERT_TEXT, filerow, start_col, text, tlen);
    {
        int prev = E->undo.suspend;
        E->undo.suspend = 1;
        undoApplyInsertText(E, filerow, start_col, text, tlen);
        E->undo.suspend = prev;
    }
    /* Move cursor to end of inserted text (character coords). */
    {
        int newcol = start_col + (int)tlen;
        E->rowoff = filerow > E->cy ? filerow - E->cy : 0;
        /* Keep cy; adjust cx/coloff for newcol */
        if (newcol < editorTextCols(E)) {
            E->cx = newcol;
            E->coloff = 0;
        } else {
            E->coloff = newcol - editorTextCols(E) + 1;
            E->cx = editorTextCols(E) - 1;
        }
        /* Prefer placing cursor on same screen row as filerow */
        if (filerow >= E->rowoff && filerow < E->rowoff + E->screenrows)
            E->cy = filerow - E->rowoff;
        else {
            E->rowoff = filerow;
            E->cy = 0;
        }
        E->cx = newcol - E->coloff;
        if (E->cx < 0) {
            E->coloff += E->cx;
            E->cx = 0;
        }
    }
    E->dirty++;
    undoEndGroup(E);
    lspClearCompletion(E);
    lspDidChange(E);
    return 1;
}
