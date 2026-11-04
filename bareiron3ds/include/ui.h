#ifndef H_UI
#define H_UI

#include <stdint.h>

// A simple RGB image (3 bytes per pixel, row by row) that the UI draws into.
// The 3DS layer copies it to the real screen.
typedef struct {
  int w, h;
  uint8_t *px;
} Canvas;

#define TOP_W 400
#define TOP_H 240
#define BOTTOM_W 320
#define BOTTOM_H 240

// Computes a bit more of the map, for at most budget_us microseconds
void ui_map_work (int64_t budget_us);
// Notices players joining/leaving and keeps the map centered on them
void ui_update (int64_t now_us);
// Top screen: live map with the players
void ui_draw_top (Canvas *c, int64_t now_us);
// Bottom screen: server address and player list. ip is NULL without Wi-Fi.
void ui_draw_bottom (Canvas *c, const char *ip, int64_t now_us);
// Bottom screen: a message card (errors, saving)
void ui_draw_message (Canvas *c, const char *title, const char *line1, const char *line2);

#endif
