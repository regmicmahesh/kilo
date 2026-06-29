#ifndef LSP_H
#define LSP_H

struct editorConfig;

#define LSP_COMPLETION_MAX 64
#define LSP_COMPLETION_VISIBLE 8

struct lspCompletionItem {
    char *label;
    char *insertText;
};

struct lspCompletion {
    int active;
    struct lspCompletionItem items[LSP_COMPLETION_MAX];
    int nitems;
    int selected;
    int start_row;
    int start_col; /* replacement start in row->chars */
};

struct lspClient {
    int pid;
    int write_fd;   /* to server stdin */
    int read_fd;    /* from server stdout */
    int next_id;
    int ready;
    int enabled;
    char *root_uri;
    char *doc_uri;
    char *language_id; /* "javascript" or "typescript" */
    int version;
    struct lspCompletion completion;
};

void lspInit(struct lspClient *lsp);
void lspFree(struct lspClient *lsp);

/* Start Node/JS/TS LSP for .js/.jsx/.mjs/.cjs/.ts/.tsx files. */
int lspStart(struct editorConfig *E);
void lspStop(struct editorConfig *E);

void lspDidOpen(struct editorConfig *E);
void lspDidChange(struct editorConfig *E);
void lspDidClose(struct editorConfig *E);

int lspRequestCompletion(struct editorConfig *E);

void lspClearCompletion(struct editorConfig *E);
void lspCompletionNext(struct editorConfig *E);
void lspCompletionPrev(struct editorConfig *E);
int lspCompletionAccept(struct editorConfig *E);

int lspShouldTrigger(struct editorConfig *E, int c);

#endif
