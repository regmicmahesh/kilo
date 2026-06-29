#ifndef SEARCH_H
#define SEARCH_H

#include "editor.h"

void editorFind(struct editorConfig *E, int fd);
void editorFindReplace(struct editorConfig *E, int fd);

#endif
