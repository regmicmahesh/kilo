#ifndef GUI_H
#define GUI_H

struct editorConfig;

/* Window / GL / font */
int guiInit(int width, int height, const char *title);
void guiShutdown(void);
int guiPollQuit(void); /* 1 if user closed window */

/* Cell metrics (pixels). */
int guiCellW(void);
int guiCellH(void);
int guiWinW(void);
int guiWinH(void);

/* Map window size to editor screenrows/screencols (accounts for status bars). */
void guiUpdateEditorSize(struct editorConfig *E);

/* Frame */
void guiBeginFrame(void);
void guiEndFrame(void);

/* Drawing in pixel coords (origin top-left). */
void guiFillRect(float x, float y, float w, float h, float r, float g, float b, float a);
void guiDrawText(float x, float y, const char *text, int len, float r, float g, float b);
void guiDrawTextCell(int col, int row, const char *text, int len, float r, float g, float b);
void guiFillCellRow(int col, int row, int ncols, float r, float g, float b, float a);

/* Blocking key read compatible with KEY_ACTION codes (and printable ASCII). */
int guiWaitKey(void);
/* Non-blocking: return 0 if none. */
int guiPollKey(void);

/* Mouse wheel: accumulate and consume as page scroll deltas (lines). */
int guiConsumeWheelLines(void);

/* Set window title (e.g. filename + dirty). */
void guiSetTitle(const char *title);

#endif
