/* Drawing pieces shared by the screens (screens.c). */
#ifndef PRISM_APP_LOOK_H
#define PRISM_APP_LOOK_H
#include "ui.h"

#define LEFT	38
#define RIGHT	602

float fsin(float t);
float fcos(float t);
void spectrum_bar(float x0, float x1, float y, float h, int stops, int alpha);
void backdrop(void);
void top_bar(const char *name);
void top_right(const char *a, col_t ca, const char *b, col_t cb);
int  legend_item(int x, int which, const char *key, const char *label);
int  legend_icon(int x, int which, const char *label);	/* 0 cross, 1 square, 2 triangle, 3 circle */
void legend_line(void);
void draw_message(const char *head, const char *l1, const char *l2, const char *key, const char *label);
void disc(float cx, float cy, float ro, float ri, int al);
void card_shape(float cx, float cy, float w, float h, int al);
int  note_up(void);
const char *note_text(void);

#endif
