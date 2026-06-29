#ifndef FILEIO_H
#define FILEIO_H

#include "editor.h"

int editorOpen(struct editorConfig *E, char *filename);
int editorSave(struct editorConfig *E);

#endif
