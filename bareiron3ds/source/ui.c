// Screens for bareiron 3DS: a live top-down map of the world with the
// players on it (top screen) and a small dashboard (bottom screen).
//
// Everything is drawn in software into an RGB Canvas. The map is built
// from the same terrain generator the server uses, a few hundred columns
// at a time, so it never stalls the server.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "globals.h"
#include "tools.h"
#include "worldgen.h"
#include "registries.h"
#include "font8x8.h"
#include "ui.h"

// Defined in worldgen.c, not exported by its header
ChunkFeature getFeatureFromAnchor (ChunkAnchor anchor);

// ---------------------------------------------------------------------------
// Colors

typedef struct { uint8_t r, g, b; } Color;

#define RGB(r, g, b) ((Color){ r, g, b })

static const Color C_WHITE = { 255, 255, 255 };
static const Color C_TEXT_DIM = { 150, 158, 176 };
static const Color C_PANEL = { 34, 38, 48 };
static const Color C_BG = { 20, 22, 28 };
static const Color C_SHADOW = { 0, 0, 0 };
static const Color C_GOLD = { 255, 214, 80 };
static const Color C_GREEN = { 120, 220, 110 };
static const Color C_RED = { 255, 110, 100 };

static const Color player_colors[] = {
  { 255, 107, 107 }, { 255, 170, 64 }, { 255, 221, 87 }, { 120, 220, 110 },
  { 80, 200, 235 }, { 125, 145, 255 }, { 200, 125, 255 }, { 255, 125, 200 }
};

// ---------------------------------------------------------------------------
// Drawing primitives

static inline void put (Canvas *c, int x, int y, Color col) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  uint8_t *p = c->px + (y * c->w + x) * 3;
  p[0] = col.r; p[1] = col.g; p[2] = col.b;
}

static inline void blend (Canvas *c, int x, int y, Color col, int alpha) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  if (alpha >= 255) { put(c, x, y, col); return; }
  if (alpha <= 0) return;
  uint8_t *p = c->px + (y * c->w + x) * 3;
  p[0] = (p[0] * (255 - alpha) + col.r * alpha) / 255;
  p[1] = (p[1] * (255 - alpha) + col.g * alpha) / 255;
  p[2] = (p[2] * (255 - alpha) + col.b * alpha) / 255;
}

static void fill_rect (Canvas *c, int x, int y, int w, int h, Color col, int alpha) {
  for (int j = y; j < y + h; j ++)
    for (int i = x; i < x + w; i ++) blend(c, i, j, col, alpha);
}

static void fill_round (Canvas *c, int x, int y, int w, int h, int r, Color col, int alpha) {
  for (int j = 0; j < h; j ++) {
    for (int i = 0; i < w; i ++) {
      // Distance into the nearest corner circle, if we are in a corner
      int cx = i < r ? r - i : (i >= w - r ? i - (w - r - 1) : 0);
      int cy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
      if (cx && cy) {
        float d = sqrtf((float)((cx - 0.5f) * (cx - 0.5f) + (cy - 0.5f) * (cy - 0.5f)));
        if (d > r + 0.5f) continue;
        if (d > r - 0.5f) { blend(c, x + i, y + j, col, alpha * (r + 0.5f - d)); continue; }
      }
      blend(c, x + i, y + j, col, alpha);
    }
  }
}

// Converts UTF-8 text to the font's code page 437. Returns the length.
static int to_cp437 (const char *s, uint8_t *out, int max) {
  int n = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p && n < max) {
    uint32_t cp = *p++;
    if (cp >= 0xC0 && (*p & 0xC0) == 0x80) {
      cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
    }
    uint8_t ch;
    switch (cp) {
      case 0xE1: ch = 0xA0; break; // á
      case 0xE0: ch = 0x85; break; // à
      case 0xE2: ch = 0x83; break; // â
      case 0xE3: ch = 'a'; break;  // ã (not in the font)
      case 0xE9: ch = 0x82; break; // é
      case 0xEA: ch = 0x88; break; // ê
      case 0xED: ch = 0xA1; break; // í
      case 0xF3: ch = 0xA2; break; // ó
      case 0xF4: ch = 0x93; break; // ô
      case 0xF5: ch = 'o'; break;  // õ (not in the font)
      case 0xFA: ch = 0xA3; break; // ú
      case 0xE7: ch = 0x87; break; // ç
      case 0xC7: ch = 0x80; break; // Ç
      case 0xC9: ch = 0x90; break; // É
      case 0xB7: ch = 0xFA; break; // ·
      default: ch = cp < 128 ? cp : '?';
    }
    out[n ++] = ch;
  }
  return n;
}

static int text_width (const char *s, int scale) {
  uint8_t buf[64];
  return to_cp437(s, buf, sizeof(buf)) * 8 * scale;
}

static void draw_glyphs (Canvas *c, int x, int y, const uint8_t *s, int n, int scale, Color col, int alpha) {
  for (int k = 0; k < n; k ++) {
    const uint8_t *g = &font8x8[s[k] * 8];
    for (int row = 0; row < 8; row ++) {
      for (int bit = 0; bit < 8; bit ++) {
        if (!(g[row] >> (7 - bit) & 1)) continue;
        for (int sy = 0; sy < scale; sy ++)
          for (int sx = 0; sx < scale; sx ++)
            blend(c, x + k * 8 * scale + bit * scale + sx, y + row * scale + sy, col, alpha);
      }
    }
  }
}

// Draws text with a soft drop shadow, like Minecraft's own text
static void text (Canvas *c, int x, int y, const char *s, int scale, Color col) {
  uint8_t buf[64];
  int n = to_cp437(s, buf, sizeof(buf));
  int off = scale > 1 ? scale / 2 + 1 : 1;
  draw_glyphs(c, x + off, y + off, buf, n, scale, C_SHADOW, 110);
  draw_glyphs(c, x, y, buf, n, scale, col, 255);
}

static void text_alpha (Canvas *c, int x, int y, const char *s, Color col, int alpha) {
  uint8_t buf[64];
  int n = to_cp437(s, buf, sizeof(buf));
  draw_glyphs(c, x + 1, y + 1, buf, n, 1, C_SHADOW, alpha * 110 / 255);
  draw_glyphs(c, x, y, buf, n, 1, col, alpha);
}

static uint32_t hash32 (uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352d;
  x ^= x >> 15; x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

static Color shade (Color col, int factor256) {
  int r = col.r * factor256 / 256, g = col.g * factor256 / 256, b = col.b * factor256 / 256;
  return RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
}

// ---------------------------------------------------------------------------
// Players

static Color player_color (const char *name) {
  uint32_t h = 5381;
  for (int i = 0; i < 16 && name[i]; i ++) h = h * 33 + (uint8_t)name[i];
  return player_colors[hash32(h) % (sizeof(player_colors) / sizeof(player_colors[0]))];
}

static int player_online (int i) {
  return player_data[i].client_fd != -1 && player_data[i].name[0];
}

static void player_name (int i, char *out) {
  memcpy(out, player_data[i].name, 16);
  out[16] = 0;
}

// Rounded square with the first letter of the name
static void avatar (Canvas *c, int x, int y, int size, const char *name) {
  Color col = player_color(name);
  fill_round(c, x, y, size, size, size / 4, col, 255);
  uint8_t ch[1];
  to_cp437(name, ch, 1);
  if (ch[0] >= 'a' && ch[0] <= 'z') ch[0] -= 32;
  draw_glyphs(c, x + (size - 8) / 2 + 1, y + (size - 8) / 2 + 1, ch, 1, 1, shade(col, 90), 255);
}

// ---------------------------------------------------------------------------
// Map

#define MAP_SCALE 2
#define MAP_W (TOP_W / MAP_SCALE)
#define MAP_H (TOP_H / MAP_SCALE)

enum {
  T_UNKNOWN, T_GRASS, T_LEAVES, T_LEAVES_TOP, T_SAND, T_SNOW, T_ICE,
  T_MUD, T_MOSS, T_CACTUS, T_BUSH, T_LILY, T_WATER
};

static const Color tile_colors[] = {
  [T_UNKNOWN] = { 28, 31, 38 },
  [T_GRASS] = { 118, 178, 66 },
  [T_LEAVES] = { 52, 128, 44 },
  [T_LEAVES_TOP] = { 66, 146, 52 },
  [T_SAND] = { 226, 212, 156 },
  [T_SNOW] = { 238, 244, 250 },
  [T_ICE] = { 168, 204, 250 },
  [T_MUD] = { 92, 78, 70 },
  [T_MOSS] = { 98, 132, 46 },
  [T_CACTUS] = { 40, 110, 40 },
  [T_BUSH] = { 150, 110, 60 },
  [T_LILY] = { 46, 140, 52 },
  [T_WATER] = { 56, 102, 220 }
};

typedef struct {
  uint8_t type;
  uint8_t height;
  uint8_t noise;
} Cell;

static Cell map[MAP_H][MAP_W];
static Cell map_old[MAP_H][MAP_W];
static int map_ox = 8 - MAP_W / 2, map_oz = 8 - MAP_H / 2;
static int map_cursor = 0;
static int map_pending = 1;

static Cell compute_cell (int x, int z) {
  short ax = div_floor(x, CHUNK_SIZE);
  short az = div_floor(z, CHUNK_SIZE);
  ChunkAnchor a = { .x = ax, .z = az, .hash = getChunkHash(ax, az), .biome = getChunkBiome(ax, az) };
  uint8_t h = getHeightAtFromHash(mod_abs(x, CHUNK_SIZE), mod_abs(z, CHUNK_SIZE), ax, az, a.hash, a.biome);

  Cell cell = { .height = h, .noise = hash32((uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u) & 15 };

  // Surface block, same rules as the terrain generator
  if (h >= 63) {
    if (a.biome == W_mangrove_swamp) cell.type = T_MUD;
    else if (a.biome == W_snowy_plains) cell.type = T_SNOW;
    else if (a.biome == W_desert || a.biome == W_beach) cell.type = T_SAND;
    else cell.type = T_GRASS;
  } else {
    cell.type = a.biome == W_snowy_plains ? T_ICE : T_WATER;
  }

  // Trees, cacti and other decorations
  ChunkFeature f = getFeatureFromAnchor(a);
  if (f.y != 255) {
    int dx = abs(x - f.x), dz = abs(z - f.z);
    switch (a.biome) {
      case W_plains:
        if (f.y >= 64 && dx < 3 && dz < 3) {
          int top = f.y - f.variant;
          cell.type = (dx < 2 && dz < 2) ? T_LEAVES_TOP : T_LEAVES;
          cell.height = (dx < 2 && dz < 2) ? top + 6 : top + 4;
        }
        break;
      case W_desert:
        if (dx == 0 && dz == 0) {
          cell.type = f.variant ? T_CACTUS : T_BUSH;
          cell.height = h + (f.variant ? 3 : 1);
        }
        break;
      case W_mangrove_swamp:
        if (h < 63 && dx == 0 && dz == 0) cell.type = T_LILY;
        else if (h >= 63 && dx + dz < 4) { cell.type = T_MOSS; cell.height = h + 1; }
        break;
      default: break;
    }
  }

  return cell;
}

static void map_set_origin (int ox, int oz) {
  int dx = ox - map_ox, dz = oz - map_oz;
  if (!dx && !dz) return;
  memcpy(map_old, map, sizeof(map));
  for (int r = 0; r < MAP_H; r ++) {
    for (int c = 0; c < MAP_W; c ++) {
      int orow = r + dz, ocol = c + dx;
      if (orow >= 0 && orow < MAP_H && ocol >= 0 && ocol < MAP_W) map[r][c] = map_old[orow][ocol];
      else map[r][c].type = T_UNKNOWN;
    }
  }
  map_ox = ox;
  map_oz = oz;
  map_cursor = 0;
  map_pending = 1;
}

void ui_map_work (int64_t budget_us) {
  if (!map_pending) return;
  int64_t start = get_program_time();
  // Fill rows starting from the middle, so the area around the players
  // shows up first
  while (map_cursor < MAP_W * MAP_H) {
    for (int k = 0; k < 64 && map_cursor < MAP_W * MAP_H; k ++, map_cursor ++) {
      int i = map_cursor / MAP_W;
      int r = MAP_H / 2 + ((i & 1) ? -(i + 1) / 2 : i / 2);
      int c = map_cursor % MAP_W;
      if (map[r][c].type == T_UNKNOWN) map[r][c] = compute_cell(map_ox + c, map_oz + r);
    }
    if (get_program_time() - start > budget_us) return;
  }
  map_pending = 0;
}

static Color cell_color (int r, int c) {
  Cell *cell = &map[r][c];
  Color col = tile_colors[cell->type];
  if (cell->type == T_UNKNOWN) {
    return ((r >> 2) + (c >> 2)) & 1 ? col : shade(col, 236);
  }
  int f = 256 + (cell->noise - 8) * 3;
  if (cell->type == T_WATER || cell->type == T_ICE) {
    // Deeper water is darker
    int depth = 63 - cell->height;
    if (depth > 6) depth = 6;
    f -= depth * 14;
  } else if (r > 0 && map[r - 1][c].type != T_UNKNOWN) {
    // Light comes from the north, like Minecraft's own maps
    int hn = map[r - 1][c].height;
    if (cell->height > hn) f += 30;
    else if (cell->height < hn) f -= 38;
  }
  return shade(col, f);
}

// ---------------------------------------------------------------------------
// Joins, leaves and the map center

static uint8_t was_online[MAX_PLAYERS];
static char toast_text[48];
static Color toast_color;
static char toast_name[17];
static int64_t toast_time = -10000000;

void ui_update (int64_t now_us) {
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    int on = player_online(i);
    if (on != was_online[i]) {
      char name[17];
      player_name(i, name);
      snprintf(toast_text, sizeof(toast_text), on ? "%s entrou" : "%s saiu", name);
      memcpy(toast_name, name, sizeof(name));
      toast_color = player_color(name);
      toast_time = now_us;
      was_online[i] = on;
    }
  }

  // Follow the first player online; recenter when they get near an edge
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (!player_online(i)) continue;
    int px = player_data[i].x, pz = player_data[i].z;
    int cx = map_ox + MAP_W / 2, cz = map_oz + MAP_H / 2;
    if (abs(px - cx) > MAP_W / 2 - 28 || abs(pz - cz) > MAP_H / 2 - 18) {
      map_set_origin(px - MAP_W / 2, pz - MAP_H / 2);
    }
    break;
  }
}

// ---------------------------------------------------------------------------
// Top screen

static float edge (float ax, float ay, float bx, float by, float px, float py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static void fill_triangle (Canvas *c, float x0, float y0, float x1, float y1, float x2, float y2, Color col) {
  int minx = floorf(fminf(x0, fminf(x1, x2))), maxx = ceilf(fmaxf(x0, fmaxf(x1, x2)));
  int miny = floorf(fminf(y0, fminf(y1, y2))), maxy = ceilf(fmaxf(y0, fmaxf(y1, y2)));
  float area = edge(x0, y0, x1, y1, x2, y2);
  if (area == 0) return;
  for (int y = miny; y <= maxy; y ++) {
    for (int x = minx; x <= maxx; x ++) {
      // 4 samples per pixel for smooth edges
      int inside = 0;
      for (int s = 0; s < 4; s ++) {
        float px = x + 0.25f + 0.5f * (s & 1), py = y + 0.25f + 0.5f * (s >> 1);
        float w0 = edge(x1, y1, x2, y2, px, py) / area;
        float w1 = edge(x2, y2, x0, y0, px, py) / area;
        float w2 = edge(x0, y0, x1, y1, px, py) / area;
        if (w0 >= 0 && w1 >= 0 && w2 >= 0) inside ++;
      }
      if (inside) blend(c, x, y, col, inside * 64 - (inside == 4));
    }
  }
}

// Minecraft-map style arrow pointing where the player looks
static void player_arrow (Canvas *c, float x, float y, float yaw_deg, Color col, float size) {
  float a = yaw_deg * (float)M_PI / 180.0f;
  // Minecraft: yaw 0 looks to +Z (down on the map), 90 looks to -X (left)
  float fx = -sinf(a), fy = cosf(a);
  float sx = -fy, sy = fx;
  float o = size + 2.2f;
  fill_triangle(c, x + fx * o, y + fy * o,
    x - fx * o * 0.7f + sx * o * 0.8f, y - fy * o * 0.7f + sy * o * 0.8f,
    x - fx * o * 0.7f - sx * o * 0.8f, y - fy * o * 0.7f - sy * o * 0.8f, RGB(20, 20, 24));
  fill_triangle(c, x + fx * size, y + fy * size,
    x - fx * size * 0.7f + sx * size * 0.8f, y - fy * size * 0.7f + sy * size * 0.8f,
    x - fx * size * 0.7f - sx * size * 0.8f, y - fy * size * 0.7f - sy * size * 0.8f, col);
}

void ui_draw_top (Canvas *c, int64_t now_us) {
  // Terrain
  for (int r = 0; r < MAP_H; r ++) {
    for (int col = 0; col < MAP_W; col ++) {
      Color k = cell_color(r, col);
      for (int j = 0; j < MAP_SCALE; j ++) {
        uint8_t *p = c->px + ((r * MAP_SCALE + j) * c->w + col * MAP_SCALE) * 3;
        for (int i = 0; i < MAP_SCALE; i ++) { p[0] = k.r; p[1] = k.g; p[2] = k.b; p += 3; }
      }
    }
  }

  // Soft vignette at the top so the notifications stand out
  for (int y = 0; y < 36; y ++) fill_rect(c, 0, y, c->w, 1, C_SHADOW, (36 - y) * 2);

  // Players
  int online = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (!player_online(i)) continue;
    online ++;
    PlayerData *p = &player_data[i];
    char name[17];
    player_name(i, name);
    Color col = player_color(name);
    float sx = (p->x - map_ox) * MAP_SCALE + MAP_SCALE / 2.0f;
    float sy = (p->z - map_oz) * MAP_SCALE + MAP_SCALE / 2.0f;
    int off_map = sx < 6 || sy < 6 || sx > c->w - 6 || sy > c->h - 6;
    if (sx < 6) sx = 6;
    if (sy < 6) sy = 6;
    if (sx > c->w - 6) sx = c->w - 6;
    if (sy > c->h - 6) sy = c->h - 6;
    player_arrow(c, sx, sy, p->yaw * 180.0f / 127.0f, col, off_map ? 3.5f : 5.5f);

    // Name tag above the arrow
    int w = text_width(name, 1) + 8;
    int lx = (int)sx - w / 2, ly = (int)sy - 22;
    if (lx < 2) lx = 2;
    if (lx + w > c->w - 2) lx = c->w - 2 - w;
    if (ly < 2) ly = (int)sy + 10;
    fill_round(c, lx, ly, w, 13, 4, C_SHADOW, 140);
    text(c, lx + 4, ly + 3, name, 1, C_WHITE);
  }

  // "Waiting for players" pill
  if (!online) {
    const char *msg = "Esperando jogadores";
    int dots = (now_us / 400000) % 4;
    int w = text_width(msg, 1) + 24 + 16;
    int x = (c->w - w) / 2, y = c->h - 34;
    fill_round(c, x, y, w, 22, 11, C_BG, 200);
    float pulse = 0.5f + 0.5f * sinf(now_us / 250000.0f);
    fill_round(c, x + 10, y + 8, 6, 6, 3, C_GOLD, 120 + (int)(135 * pulse));
    text(c, x + 24, y + 7, msg, 1, C_WHITE);
    for (int d = 0; d < dots; d ++) fill_rect(c, x + 24 + text_width(msg, 1) + 2 + d * 4, y + 13, 2, 2, C_WHITE, 255);
  }

  // Join/leave notification, slides in from the top
  int64_t age = now_us - toast_time;
  if (age >= 0 && age < 4000000) {
    int alpha = age > 3500000 ? (int)(255 * (4000000 - age) / 500000) : 255;
    int slide = age < 200000 ? (int)(28 * (200000 - age) / 200000) : 0;
    int w = text_width(toast_text, 1) + 38;
    int x = (c->w - w) / 2, y = 8 - slide;
    fill_round(c, x, y, w, 24, 8, C_BG, alpha * 215 / 255);
    fill_round(c, x + 6, y + 5, 14, 14, 4, toast_color, alpha);
    uint8_t ch[1];
    to_cp437(toast_name, ch, 1);
    if (ch[0] >= 'a' && ch[0] <= 'z') ch[0] -= 32;
    draw_glyphs(c, x + 10, y + 9, ch, 1, 1, shade(toast_color, 90), alpha);
    text_alpha(c, x + 28, y + 8, toast_text, C_WHITE, alpha);
  }
}

// ---------------------------------------------------------------------------
// Bottom screen

// Grass-on-dirt strip, drawn as big "pixels" like a Minecraft block
static void grass_header (Canvas *c, int h) {
  const int px = 4;
  for (int by = 0; by < h; by += px) {
    for (int bx = 0; bx < c->w; bx += px) {
      uint32_t n = hash32(bx * 31 + by * 7919);
      int grass_depth = 8 + (hash32(bx * 131) % 3) * px;
      Color col;
      if (by < grass_depth) col = (n & 3) == 0 ? RGB(96, 160, 52) : ((n & 3) == 1 ? RGB(130, 192, 72) : RGB(112, 176, 60));
      else col = (n & 3) == 0 ? RGB(110, 78, 52) : ((n & 3) == 1 ? RGB(134, 96, 66) : RGB(121, 86, 58));
      fill_rect(c, bx, by, px, px, col, 255);
    }
  }
  for (int y = 0; y < 4; y ++) fill_rect(c, 0, h - 4 + y, c->w, 1, C_SHADOW, 40 + y * 20);
}

void ui_draw_bottom (Canvas *c, const char *ip, int64_t now_us) {
  fill_rect(c, 0, 0, c->w, c->h, C_BG, 255);

  // Header
  grass_header(c, 40);
  text(c, 12, 13, "bareiron", 2, C_WHITE);
  text(c, 12 + text_width("bareiron ", 2), 13, "3DS", 2, C_GOLD);
  {
    const char *st = ip ? "ONLINE" : "SEM REDE";
    Color sc = ip ? C_GREEN : C_RED;
    int w = text_width(st, 1) + 22;
    int x = c->w - w - 10;
    fill_round(c, x, 12, w, 16, 8, C_SHADOW, 150);
    fill_round(c, x + 7, 17, 6, 6, 3, sc, 255);
    text(c, x + 17, 16, st, 1, C_WHITE);
  }

  // Address card
  fill_round(c, 10, 50, 300, 72, 8, C_PANEL, 255);
  if (ip) {
    text(c, 22, 60, "ENDEREÇO DO SERVIDOR", 1, C_TEXT_DIM);
    int scale = text_width(ip, 3) <= 276 ? 3 : 2;
    text(c, 22, scale == 3 ? 76 : 80, ip, scale, C_GOLD);
    char sub[48];
    snprintf(sub, sizeof(sub), "Minecraft Java 1.21.8 · porta %d", PORT);
    text(c, 22, 106, sub, 1, C_TEXT_DIM);
  } else {
    text(c, 22, 60, "SEM WI-FI", 1, C_RED);
    text(c, 22, 78, "Conecte o 3DS", 2, C_WHITE);
    text(c, 22, 98, "Configurações > Internet", 1, C_TEXT_DIM);
  }

  // Players card
  fill_round(c, 10, 130, 300, 86, 8, C_PANEL, 255);
  int online = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) online += player_online(i);
  text(c, 22, 140, "JOGADORES", 1, C_TEXT_DIM);
  {
    char count[16];
    snprintf(count, sizeof(count), "%d online", online);
    text(c, 298 - text_width(count, 1), 140, count, 1, online ? C_GREEN : C_TEXT_DIM);
  }
  if (!online) {
    const char *msg = "Ninguém ainda. Chama a galera!";
    text(c, (c->w - text_width(msg, 1)) / 2, 176, msg, 1, C_TEXT_DIM);
  } else {
    int row = 0, shown = 0;
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (!player_online(i)) continue;
      if (row == 4) {
        char more[24];
        snprintf(more, sizeof(more), "e mais %d", online - shown);
        text(c, 42, 156 + row * 15, more, 1, C_TEXT_DIM);
        break;
      }
      char name[17], pos[32];
      player_name(i, name);
      int y = 154 + row * 15;
      avatar(c, 22, y, 12, name);
      text(c, 42, y + 2, name, 1, C_WHITE);
      snprintf(pos, sizeof(pos), "%d, %d", player_data[i].x, player_data[i].z);
      text(c, 298 - text_width(pos, 1), y + 2, pos, 1, C_TEXT_DIM);
      row ++;
      shown ++;
    }
  }

  // Footer
  text(c, 12, 226, "START sair", 1, C_TEXT_DIM);
  text(c, 12, 226, "START", 1, C_WHITE);
  const char *sel = "SELECT apagar tela";
  int sx = c->w - 12 - text_width(sel, 1);
  text(c, sx, 226, sel, 1, C_TEXT_DIM);
  text(c, sx, 226, "SELECT", 1, C_WHITE);
}

void ui_draw_message (Canvas *c, const char *title, const char *line1, const char *line2) {
  fill_rect(c, 0, 0, c->w, c->h, C_BG, 255);
  grass_header(c, 40);
  text(c, 12, 13, "bareiron", 2, C_WHITE);
  text(c, 12 + text_width("bareiron ", 2), 13, "3DS", 2, C_GOLD);
  fill_round(c, 10, 70, 300, 100, 8, C_PANEL, 255);
  text(c, (c->w - text_width(title, 2)) / 2, 88, title, 2, C_WHITE);
  if (line1) text(c, (c->w - text_width(line1, 1)) / 2, 122, line1, 1, C_TEXT_DIM);
  if (line2) text(c, (c->w - text_width(line2, 1)) / 2, 140, line2, 1, C_TEXT_DIM);
}
