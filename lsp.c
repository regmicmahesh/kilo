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
    free(lsp->language_id);
    lsp->root_uri = NULL;
    lsp->doc_uri = NULL;
    lsp->language_id = NULL;
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

/* --- string builder (always uses strlen for C strings) --- */

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

static void abStr(char **buf, size_t *len, size_t *cap, const char *s) {
    abGrow(buf, len, cap, s, strlen(s));
}

static void jsonEscapeAppend(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char tmp[8];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\'; tmp[1] = (char)c; tmp[2] = '\0';
            abStr(buf, len, cap, tmp);
        } else if (c == '\n') {
            abStr(buf, len, cap, "\\n");
        } else if (c == '\r') {
            abStr(buf, len, cap, "\\r");
        } else if (c == '\t') {
            abStr(buf, len, cap, "\\t");
        } else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            abStr(buf, len, cap, tmp);
        } else {
            tmp[0] = (char)c; tmp[1] = '\0';
            abStr(buf, len, cap, tmp);
        }
    }
}

static char *pathToFileUri(const char *path) {
    char resolved[PATH_MAX];
    const char *p;
    char *uri = NULL;
    size_t len = 0, cap = 0;

    if (realpath(path, resolved))
        p = resolved;
    else
        p = path;

    abStr(&uri, &len, &cap, "file://");
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '~') {
            char ch[2] = {(char)c, 0};
            abStr(&uri, &len, &cap, ch);
        } else if (c == ' ') {
            abStr(&uri, &len, &cap, "%20");
        } else {
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%%%02X", c);
            abStr(&uri, &len, &cap, tmp);
        }
        p++;
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

/* Map filename -> LSP languageId, or NULL if unsupported. */
static const char *detectLanguageId(const char *fn) {
    size_t n;
    if (!fn) return NULL;
    n = strlen(fn);
    if (n >= 5 && strcmp(fn + n - 5, ".tsx") == 0) return "typescriptreact";
    if (n >= 5 && strcmp(fn + n - 5, ".jsx") == 0) return "javascriptreact";
    if (n >= 4 && strcmp(fn + n - 4, ".ts") == 0) return "typescript";
    if (n >= 4 && strcmp(fn + n - 4, ".mjs") == 0) return "javascript";
    if (n >= 4 && strcmp(fn + n - 4, ".cjs") == 0) return "javascript";
    if (n >= 3 && strcmp(fn + n - 3, ".js") == 0) return "javascript";
    return NULL;
}

/* --- Process / RPC --- */

static int lspWriteRaw(struct lspClient *lsp, const char *body, size_t body_len) {
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        "Content-Length: %zu\r\n\r\n", body_len);
    if (lsp->write_fd < 0) return -1;
    if (write(lsp->write_fd, header, (size_t)hlen) != hlen) return -1;
    if (write(lsp->write_fd, body, body_len) != (ssize_t)body_len) return -1;
    return 0;
}

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
        timeout_ms = 3000;
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
    if (content_length < 0 || content_length > 16 * 1024 * 1024) return -1;

    body = malloc((size_t)content_length + 1);
    if (!body) return -1;
    while (got < (size_t)content_length) {
        fd_set rfds;
        struct timeval tv;
        ssize_t n;
        FD_ZERO(&rfds);
        FD_SET(lsp->read_fd, &rfds);
        tv.tv_sec = 3;
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

static int responseHasId(const char *body, int id) {
    char a[32], b[32];
    snprintf(a, sizeof(a), "\"id\":%d", id);
    snprintf(b, sizeof(b), "\"id\": %d", id);
    if (!strstr(body, a) && !strstr(body, b)) return 0;
    /* Responses have result or error at top level; skip server requests. */
    if (strstr(body, "\"result\"") || strstr(body, "\"error\"")) return 1;
    return 0;
}

static char *lspWaitResponse(struct lspClient *lsp, int id, int timeout_ms) {
    int spins = 0;
    while (spins++ < 80) {
        char *body;
        size_t len;
        if (lspReadMessage(lsp, &body, &len, timeout_ms) != 0)
            return NULL;
        if (responseHasId(body, id))
            return body;
        /* Drop notifications / other messages (e.g. window/logMessage). */
        free(body);
        timeout_ms = 3000;
    }
    return NULL;
}

static int lspNotify(struct lspClient *lsp, const char *method, const char *params_json) {
    char *body = NULL;
    size_t len = 0, cap = 0;
    int rc;
    abStr(&body, &len, &cap, "{\"jsonrpc\":\"2.0\",\"method\":\"");
    abStr(&body, &len, &cap, method);
    abStr(&body, &len, &cap, "\",\"params\":");
    abStr(&body, &len, &cap, params_json);
    abStr(&body, &len, &cap, "}");
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
    abStr(&body, &len, &cap, "{\"jsonrpc\":\"2.0\",\"id\":");
    abStr(&body, &len, &cap, idbuf);
    abStr(&body, &len, &cap, ",\"method\":\"");
    abStr(&body, &len, &cap, method);
    abStr(&body, &len, &cap, "\",\"params\":");
    abStr(&body, &len, &cap, params_json);
    abStr(&body, &len, &cap, "}");
    if (lspWriteRaw(lsp, body, len) != 0) {
        free(body);
        return NULL;
    }
    free(body);
    resp = lspWaitResponse(lsp, id, timeout_ms);
    return resp;
}

static void killSpawned(struct lspClient *lsp) {
    if (lsp->write_fd >= 0) { close(lsp->write_fd); lsp->write_fd = -1; }
    if (lsp->read_fd >= 0) { close(lsp->read_fd); lsp->read_fd = -1; }
    if (lsp->pid > 0) {
        kill(lsp->pid, SIGTERM);
        waitpid(lsp->pid, NULL, 0);
        lsp->pid = -1;
    }
}

/* Ensure nvm / homebrew node bins are visible when the editor was started
 * with a minimal PATH (common for GUI / tooling launches). */
static void augmentPathForNode(void) {
    const char *home = getenv("HOME");
    const char *old = getenv("PATH");
    char extra[PATH_MAX * 2];
    char combined[PATH_MAX * 4];
    extra[0] = '\0';
    if (home && home[0]) {
        /* Prefer a stable nvm current link if present, else common version dirs. */
        snprintf(extra, sizeof(extra),
                 "%s/.nvm/versions/node/v24.11.0/bin:"
                 "%s/.nvm/versions/node/v22.0.0/bin:"
                 "%s/.nvm/versions/node/v20.0.0/bin:"
                 "%s/.local/bin:/opt/homebrew/bin:/usr/local/bin",
                 home, home, home, home);
    } else {
        snprintf(extra, sizeof(extra),
                 "/opt/homebrew/bin:/usr/local/bin");
    }
    if (old && old[0])
        snprintf(combined, sizeof(combined), "%s:%s", extra, old);
    else
        snprintf(combined, sizeof(combined), "%s", extra);
    setenv("PATH", combined, 1);
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
        {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        augmentPathForNode();
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    lsp->pid = pid;
    lsp->write_fd = in_pipe[1];
    lsp->read_fd = out_pipe[0];

    /* Give the process a moment; fail fast if execvp failed. */
    usleep(50000);
    {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            lsp->pid = -1;
            close(lsp->write_fd); lsp->write_fd = -1;
            close(lsp->read_fd); lsp->read_fd = -1;
            return -1;
        }
    }
    return 0;
}

/* Try Node-based language servers for JS/TS. */
static int tryStartServer(struct lspClient *lsp) {
    char *attempts[][5] = {
        {"typescript-language-server", "--stdio", NULL, NULL, NULL},
        {"npx", "--yes", "typescript-language-server", "--stdio", NULL},
    };
    size_t i;
    for (i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        killSpawned(lsp);
        if (spawnServer(lsp, attempts[i]) == 0)
            return 0;
    }
    killSpawned(lsp);
    return -1;
}

int lspStart(struct editorConfig *E) {
    struct lspClient *lsp = &E->lsp;
    char *params = NULL;
    size_t plen = 0, pcap = 0;
    char *resp;
    const char *fn = E->filename ? E->filename : "";
    const char *lang;

    lspStop(E);
    lspInit(lsp);

    lang = detectLanguageId(fn);
    if (!lang) {
        lsp->enabled = 0;
        return 0;
    }
    lsp->language_id = strdup(lang);

    if (tryStartServer(lsp) != 0) {
        editorSetStatusMessage(E,
            "LSP: install with: npm i -g typescript typescript-language-server");
        return -1;
    }

    lsp->doc_uri = pathToFileUri(fn);
    lsp->root_uri = dirname_uri(lsp->doc_uri);
    lsp->version = 1;
    lsp->next_id = 1;

    /* Minimal initialize — lengths via abStr (no hardcoded wrong sizes). */
    abStr(&params, &plen, &pcap, "{\"processId\":null,\"rootUri\":\"");
    abStr(&params, &plen, &pcap, lsp->root_uri);
    abStr(&params, &plen, &pcap,
          "\",\"capabilities\":{\"textDocument\":{\"completion\":{"
          "\"completionItem\":{\"snippetSupport\":false}}}},"
          "\"workspaceFolders\":null}");

    resp = lspRequest(lsp, "initialize", params, 8000);
    free(params);
    if (!resp) {
        editorSetStatusMessage(E,
            "LSP: initialize failed (npm i -g typescript typescript-language-server)");
        lspStop(E);
        return -1;
    }
    free(resp);

    lspNotify(lsp, "initialized", "{}");
    lsp->ready = 1;
    lsp->enabled = 1;
    lspDidOpen(E);
    editorSetStatusMessage(E, "LSP: Node/TS language server ready (%s)", lang);
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
    const char *lang = E->lsp.language_id ? E->lsp.language_id : "javascript";

    snprintf(ver, sizeof(ver), "%d", E->lsp.version);
    abStr(&p, &len, &cap, "{\"textDocument\":{\"uri\":\"");
    abStr(&p, &len, &cap, E->lsp.doc_uri);
    if (with_text) {
        int buflen = 0;
        char *text = editorRowsToString(E, &buflen);
        abStr(&p, &len, &cap, "\",\"languageId\":\"");
        abStr(&p, &len, &cap, lang);
        abStr(&p, &len, &cap, "\",\"version\":");
        abStr(&p, &len, &cap, ver);
        abStr(&p, &len, &cap, ",\"text\":\"");
        if (text)
            jsonEscapeAppend(&p, &len, &cap, text, (size_t)buflen);
        free(text);
        abStr(&p, &len, &cap, "\"}}");
    } else {
        abStr(&p, &len, &cap, "\"}}");
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
    abStr(&p, &len, &cap, "{\"textDocument\":{\"uri\":\"");
    abStr(&p, &len, &cap, E->lsp.doc_uri);
    abStr(&p, &len, &cap, "\",\"version\":");
    abStr(&p, &len, &cap, ver);
    abStr(&p, &len, &cap, "},\"contentChanges\":[{\"text\":\"");
    if (text)
        jsonEscapeAppend(&p, &len, &cap, text, (size_t)buflen);
    free(text);
    abStr(&p, &len, &cap, "\"}]}");
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

static char *jsonExtractString(const char *json, const char *key) {
    char pattern[128];
    const char *p;
    char *out = NULL;
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
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') abStr(&out, &len, &cap, "\n");
            else if (*p == 't') abStr(&out, &len, &cap, "\t");
            else if (*p == 'r') abStr(&out, &len, &cap, "\r");
            else if (*p == '"' || *p == '\\' || *p == '/') {
                char ch[2] = {*p, 0};
                abStr(&out, &len, &cap, ch);
            } else if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                p += 4;
            } else {
                char ch[2] = {*p, 0};
                abStr(&out, &len, &cap, ch);
            }
            p++;
        } else {
            char ch[2] = {*p, 0};
            abStr(&out, &len, &cap, ch);
            p++;
        }
    }
    if (!out) {
        out = malloc(1);
        if (out) out[0] = '\0';
    }
    return out;
}

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
    p++;

    while (*p && *p != ']' && comp->nitems < LSP_COMPLETION_MAX) {
        const char *obj;
        char *label, *insertText;
        const char *end;

        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        obj = p;
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
            /* Skip snippet placeholders for simplicity — strip $0 etc. */
            int i = comp->nitems++;
            if (insertText) {
                char *d = insertText;
                char *s = insertText;
                while (*s) {
                    if (*s == '$' && (s[1] == '0' || s[1] == '{' || isdigit((unsigned char)s[1]))) {
                        if (s[1] == '{') {
                            while (*s && *s != '}') s++;
                            if (*s == '}') s++;
                        } else {
                            s++;
                            while (isdigit((unsigned char)*s)) s++;
                        }
                        continue;
                    }
                    *d++ = *s++;
                }
                *d = '\0';
            }
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

/* Keep only items whose label or insertText starts with the typed prefix
 * (e.g. document.al → only alert, ... not every Math member). */
static int itemMatchesPrefix(const struct lspCompletionItem *it,
                             const char *prefix, size_t plen) {
    const char *cands[2];
    int i;

    if (plen == 0) return 1;
    cands[0] = it->label;
    cands[1] = it->insertText;
    for (i = 0; i < 2; i++) {
        if (!cands[i]) continue;
        if (strncmp(cands[i], prefix, plen) == 0)
            return 1;
    }
    return 0;
}

static void filterCompletionByPrefix(struct lspCompletion *comp,
                                     const char *prefix, size_t plen) {
    int r, w;

    if (!comp || plen == 0) return;
    w = 0;
    for (r = 0; r < comp->nitems; r++) {
        if (itemMatchesPrefix(&comp->items[r], prefix, plen)) {
            if (w != r)
                comp->items[w] = comp->items[r];
            w++;
        } else {
            free(comp->items[r].label);
            free(comp->items[r].insertText);
            comp->items[r].label = NULL;
            comp->items[r].insertText = NULL;
        }
    }
    for (r = w; r < comp->nitems; r++) {
        comp->items[r].label = NULL;
        comp->items[r].insertText = NULL;
    }
    comp->nitems = w;
    if (comp->selected >= comp->nitems)
        comp->selected = 0;
    if (comp->nitems == 0)
        freeCompletionItems(comp);
}

static int isIdentChar(int c) {
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

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
    if (c == '.' || c == '(') return 1;
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

    lspDidChange(E);

    snprintf(linebuf, sizeof(linebuf), "%d", filerow);
    snprintf(charbuf, sizeof(charbuf), "%d", filecol);

    abStr(&params, &len, &cap, "{\"textDocument\":{\"uri\":\"");
    abStr(&params, &len, &cap, E->lsp.doc_uri);
    abStr(&params, &len, &cap, "\"},\"position\":{\"line\":");
    abStr(&params, &len, &cap, linebuf);
    abStr(&params, &len, &cap, ",\"character\":");
    abStr(&params, &len, &cap, charbuf);
    abStr(&params, &len, &cap, "}}");

    resp = lspRequest(&E->lsp, "textDocument/completion", params, 4000);
    free(params);
    if (!resp) {
        lspClearCompletion(E);
        return -1;
    }
    parseCompletionResult(&E->lsp.completion, resp);
    free(resp);

    E->lsp.completion.start_row = filerow;
    E->lsp.completion.start_col = completionStartCol(E, filerow, filecol);

    /* Filter to prefix typed so far (chars from word start to cursor). */
    if (filerow >= 0 && filerow < E->numrows) {
        erow *row = &E->row[filerow];
        int sc = E->lsp.completion.start_col;
        int ec = filecol;
        if (sc < 0) sc = 0;
        if (ec > row->size) ec = row->size;
        if (ec > sc && row->chars) {
            size_t plen = (size_t)(ec - sc);
            char *prefix = malloc(plen + 1);
            if (prefix) {
                memcpy(prefix, row->chars + sc, plen);
                prefix[plen] = '\0';
                filterCompletionByPrefix(&E->lsp.completion, prefix, plen);
                free(prefix);
            }
        }
    }

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
        char *old = malloc((size_t)del_len + 1);
        if (old) {
            memcpy(old, row->chars + start_col, (size_t)del_len);
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
    {
        int newcol = start_col + (int)tlen;
        if (filerow >= E->rowoff && filerow < E->rowoff + E->screenrows)
            E->cy = filerow - E->rowoff;
        else {
            E->rowoff = filerow;
            E->cy = 0;
        }
        if (newcol < editorTextCols(E)) {
            E->cx = newcol;
            E->coloff = 0;
        } else {
            E->coloff = newcol - editorTextCols(E) + 1;
            E->cx = editorTextCols(E) - 1;
        }
    }
    E->dirty++;
    undoEndGroup(E);
    lspClearCompletion(E);
    lspDidChange(E);
    return 1;
}
