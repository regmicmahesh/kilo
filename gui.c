#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#ifdef __linux__
#include <GL/gl.h>
#endif
#endif

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#include "gui.h"
#include "editor.h"

#define FONT_PIXEL_HEIGHT 18.0f
#define ATLAS_W 512
#define ATLAS_H 512
#define FIRST_CHAR 32
#define NUM_CHARS 96 /* 32..127 */

static SDL_Window *window;
static SDL_GLContext glctx;
static int win_w = 1100, win_h = 720;
static int cell_w = 10, cell_h = 20;

static GLuint font_tex = 0;
static stbtt_bakedchar cdata[NUM_CHARS];
static unsigned char atlas[ATLAS_W * ATLAS_H];

static int quit_flag;
static int key_queue[64];
static int kq_r, kq_w;
static int wheel_lines;

static void kqPush(int k) {
    int n = (kq_w + 1) % 64;
    if (n == kq_r) return;
    key_queue[kq_w] = k;
    kq_w = n;
}

static int kqPop(void) {
    int k;
    if (kq_r == kq_w) return 0;
    k = key_queue[kq_r];
    kq_r = (kq_r + 1) % 64;
    return k;
}

static int loadFont(const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned char *ttf;
    long sz;
    int ok;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 20 * 1024 * 1024) { fclose(f); return -1; }
    ttf = malloc((size_t)sz);
    if (!ttf) { fclose(f); return -1; }
    if (fread(ttf, 1, (size_t)sz, f) != (size_t)sz) {
        free(ttf); fclose(f); return -1;
    }
    fclose(f);
    memset(atlas, 0, sizeof(atlas));
    ok = stbtt_BakeFontBitmap(ttf, 0, FONT_PIXEL_HEIGHT, atlas, ATLAS_W, ATLAS_H,
                              FIRST_CHAR, NUM_CHARS, cdata);
    free(ttf);
    if (ok <= 0) return -1;

    /* Approximate cell size from 'M' and line height. */
    {
        stbtt_aligned_quad q;
        float x = 0, y = 0;
        stbtt_GetBakedQuad(cdata, ATLAS_W, ATLAS_H, 'M' - FIRST_CHAR, &x, &y, &q, 1);
        cell_w = (int)ceilf(x);
        if (cell_w < 8) cell_w = 8;
        cell_h = (int)ceilf(FONT_PIXEL_HEIGHT * 1.25f);
        if (cell_h < 16) cell_h = 16;
    }

    glGenTextures(1, &font_tex);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS_W, ATLAS_H, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, atlas);
    return 0;
}

static const char *fontCandidates[] = {
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
    "/Library/Fonts/Courier New.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    NULL
};

int guiInit(int width, int height, const char *title) {
    int i;
    win_w = width;
    win_h = height;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    window = SDL_CreateWindow(title ? title : "kilo",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              win_w, win_h,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_SetSwapInterval(1);

    {
        int dw, dh;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        win_w = dw;
        win_h = dh;
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);

    for (i = 0; fontCandidates[i]; i++) {
        if (loadFont(fontCandidates[i]) == 0)
            break;
    }
    if (font_tex == 0) {
        fprintf(stderr, "Could not load a monospaced TTF font.\n");
        return -1;
    }

    quit_flag = 0;
    kq_r = kq_w = 0;
    wheel_lines = 0;
    SDL_StartTextInput();
    return 0;
}

void guiShutdown(void) {
    if (font_tex) glDeleteTextures(1, &font_tex);
    if (glctx) SDL_GL_DeleteContext(glctx);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    window = NULL;
    glctx = NULL;
}

int guiPollQuit(void) { return quit_flag; }
int guiCellW(void) { return cell_w; }
int guiCellH(void) { return cell_h; }
int guiWinW(void) { return win_w; }
int guiWinH(void) { return win_h; }

void guiSetTitle(const char *title) {
    if (window) SDL_SetWindowTitle(window, title);
}

void guiUpdateEditorSize(struct editorConfig *E) {
    /* Two status rows at bottom. */
    int rows = (win_h / cell_h) - 2;
    int cols = win_w / cell_w;
    if (rows < 3) rows = 3;
    if (cols < 20) cols = 20;
    E->screenrows = rows;
    E->screencols = cols;
    if (E->cy >= E->screenrows) E->cy = E->screenrows - 1;
    if (E->cx >= editorTextCols(E)) E->cx = editorTextCols(E) - 1;
}

static void setOrtho(void) {
    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* Top-left origin */
    glOrtho(0, win_w, win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void guiBeginFrame(void) {
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    setOrtho();
    glClearColor(0.11f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void guiEndFrame(void) {
    SDL_GL_SwapWindow(window);
}

void guiFillRect(float x, float y, float w, float h, float r, float g, float b, float a) {
    glDisable(GL_TEXTURE_2D);
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
    glEnable(GL_TEXTURE_2D);
}

void guiFillCellRow(int col, int row, int ncols, float r, float g, float b, float a) {
    guiFillRect((float)(col * cell_w), (float)(row * cell_h),
                (float)(ncols * cell_w), (float)cell_h, r, g, b, a);
}

static void drawChar(float x, float y, int ch, float r, float g, float b) {
    stbtt_aligned_quad q;
    float fx = x, fy = y + FONT_PIXEL_HEIGHT;
    if (ch < FIRST_CHAR || ch >= FIRST_CHAR + NUM_CHARS) ch = '?';
    stbtt_GetBakedQuad(cdata, ATLAS_W, ATLAS_H, ch - FIRST_CHAR, &fx, &fy, &q, 1);
    glColor4f(r, g, b, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(q.s0, q.t0); glVertex2f(q.x0, q.y0);
    glTexCoord2f(q.s1, q.t0); glVertex2f(q.x1, q.y0);
    glTexCoord2f(q.s1, q.t1); glVertex2f(q.x1, q.y1);
    glTexCoord2f(q.s0, q.t1); glVertex2f(q.x0, q.y1);
    glEnd();
}

void guiDrawText(float x, float y, const char *text, int len, float r, float g, float b) {
    int i;
    float cx = x;
    if (!text) return;
    if (len < 0) len = (int)strlen(text);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glEnable(GL_TEXTURE_2D);
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\t') {
            cx += cell_w * 4;
            continue;
        }
        drawChar(cx, y, ch, r, g, b);
        cx += cell_w; /* monospaced advance for editor alignment */
    }
}

void guiDrawTextCell(int col, int row, const char *text, int len, float r, float g, float b) {
    guiDrawText((float)(col * cell_w), (float)(row * cell_h), text, len, r, g, b);
}

/* Map SDL key to editor KEY_ACTION / ASCII. */
static int mapKey(const SDL_KeyboardEvent *ke) {
    SDL_Keycode sym = ke->keysym.sym;
    Uint16 mod = ke->keysym.mod;
    int ctrl = (mod & KMOD_CTRL) || (mod & KMOD_GUI);

    if (ctrl) {
        switch (sym) {
        case SDLK_a: return CTRL_A;
        case SDLK_c: return CTRL_C;
        case SDLK_f: return CTRL_F;
        case SDLK_h: return CTRL_H;
        case SDLK_l: return CTRL_L;
        case SDLK_n: return CTRL_N;
        case SDLK_p: return CTRL_P;
        case SDLK_q: return CTRL_Q;
        case SDLK_s: return CTRL_S;
        case SDLK_u: return CTRL_U;
        case SDLK_y: return CTRL_Y;
        case SDLK_z: return CTRL_Z;
        case SDLK_TAB: return TAB; /* Ctrl+Tab → accept completion */
        default: break;
        }
    }

    switch (sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return ENTER;
    case SDLK_TAB: return TAB;
    case SDLK_ESCAPE: return ESC;
    case SDLK_BACKSPACE: return BACKSPACE;
    case SDLK_DELETE: return DEL_KEY;
    case SDLK_LEFT: return ARROW_LEFT;
    case SDLK_RIGHT: return ARROW_RIGHT;
    case SDLK_UP: return ARROW_UP;
    case SDLK_DOWN: return ARROW_DOWN;
    case SDLK_HOME: return HOME_KEY;
    case SDLK_END: return END_KEY;
    case SDLK_PAGEUP: return PAGE_UP;
    case SDLK_PAGEDOWN: return PAGE_DOWN;
    default: break;
    }
    return 0;
}

static void handleEvent(const SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT:
        quit_flag = 1;
        kqPush(CTRL_Q);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED) {
            SDL_GL_GetDrawableSize(window, &win_w, &win_h);
        }
        break;
    case SDL_MOUSEWHEEL:
        wheel_lines += (e->wheel.y > 0) ? -3 : 3;
        break;
    case SDL_TEXTINPUT: {
        const char *t = e->text.text;
        while (*t) {
            unsigned char c = (unsigned char)*t++;
            if (c >= 32 && c < 127)
                kqPush(c);
        }
        break;
    }
    case SDL_KEYDOWN: {
        int k = mapKey(&e->key);
        if (k) kqPush(k);
        break;
    }
    default:
        break;
    }
}

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handleEvent(&e);
}

int guiPollKey(void) {
    pump();
    return kqPop();
}

int guiWaitKey(void) {
    for (;;) {
        int k = guiPollKey();
        if (k) return k;
        if (quit_flag) return CTRL_Q;
        SDL_Delay(8);
    }
}

int guiConsumeWheelLines(void) {
    int w = wheel_lines;
    wheel_lines = 0;
    return w;
}
