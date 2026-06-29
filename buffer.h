#ifndef BUFFER_H
#define BUFFER_H

/* Heap-allocated append buffer used to build screen updates. */
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len);
void abFree(struct abuf *ab);

#endif
