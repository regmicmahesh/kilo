#ifndef TERMINAL_H
#define TERMINAL_H

#include "editor.h"

void disableRawMode(struct editorConfig *E, int fd);
int enableRawMode(struct editorConfig *E, int fd);
int editorReadKey(int fd);
int getCursorPosition(int ifd, int ofd, int *rows, int *cols);
int getWindowSize(int ifd, int ofd, int *rows, int *cols);

#endif
