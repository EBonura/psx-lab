#ifndef PLATFORM_H_
#define PLATFORM_H_

#ifdef __cplusplus
extern "C" {
#endif

// PICO-8 platform functions — implemented by the PS1 backend (main.cpp)

void P8music(int track, int fade, int mask);
void P8spr(int sprite, int x, int y, int cols, int rows, bool flipx, bool flipy);
bool P8btn(int b);
void P8sfx(int id);
void P8pal(int a, int b);
void P8pal_reset(void);
void P8circfill(int x, int y, int r, int c);
void P8rectfill(int x, int y, int x2, int y2, int c);
void P8print(const char* str, int x, int y, int c);
void P8line(int x, int y, int x2, int y2, int c);
int P8mget(int x, int y);
bool P8fget(int t, int f);
void P8camera(int x, int y);
void P8map(int mx, int my, int tx, int ty, int mw, int mh, int mask);

#ifdef __cplusplus
}
#endif

#endif
