#ifndef SYNTAX_H
#define SYNTAX_H

#include "editor.h"

/* Syntax highlight types */
#define HL_NORMAL 0
#define HL_NONPRINT 1
#define HL_COMMENT 2
#define HL_MLCOMMENT 3
#define HL_KEYWORD1 4
#define HL_KEYWORD2 5
#define HL_STRING 6
#define HL_NUMBER 7
#define HL_MATCH 8

#define HL_HIGHLIGHT_STRINGS (1<<0)
#define HL_HIGHLIGHT_NUMBERS (1<<1)

struct editorSyntax {
    char **filematch;
    char **keywords;
    char singleline_comment_start[2];
    char multiline_comment_start[3];
    char multiline_comment_end[3];
    int flags;
};

int editorRowHasOpenComment(erow *row);
void editorUpdateSyntax(struct editorConfig *E, erow *row);
int editorSyntaxToColor(int hl);
void editorSelectSyntaxHighlight(struct editorConfig *E, char *filename);

#endif
