#ifndef OUTPUT_H
#define OUTPUT_H

#include "editor.h"

void editorRefreshScreen(struct editorConfig *E);
void editorSetStatusMessage(struct editorConfig *E, const char *fmt, ...);

#endif
