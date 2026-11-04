// Nintendo 3DS platform layer for bareiron.
//
// Sets up the screens, Wi-Fi sockets and SD card, then runs the normal
// bareiron server loop. bareiron calls task_yield() often; we use it to
// keep the HOME button working, read buttons and draw the screens.

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "globals.h"
#include "serialize.h"
#include "platform_3ds.h"
#include "ui.h"

#define SOC_BUFFER_SIZE 0x100000
#define FRAME_US 16000      // check buttons/HOME ~60 times per second
#define DRAW_US 100000      // redraw the screens 10 times per second
#define MAP_BUDGET_US 3000  // time per frame spent building the map
#define IP_CHECK_US 2000000 // look for a Wi-Fi address every 2 seconds

int bareiron_main ();

static u32 *soc_buffer = NULL;
static int64_t last_frame = 0, last_draw = 0, last_ip_check = -IP_CHECK_US;
static int draw_count = 0;
static bool screens_on = true;
static bool gsplcd_ready = false;
static char ip_text[20];
static bool has_ip = false;

static uint8_t top_px[TOP_W * TOP_H * 3];
static uint8_t bottom_px[BOTTOM_W * BOTTOM_H * 3];
static Canvas top_canvas = { TOP_W, TOP_H, top_px };
static Canvas bottom_canvas = { BOTTOM_W, BOTTOM_H, bottom_px };

static int64_t now_us () {
  u64 ticks = svcGetSystemTick();
  return (int64_t)(ticks / SYSCLOCK_ARM11) * 1000000LL +
    (int64_t)((ticks % SYSCLOCK_ARM11) * 1000000ULL / SYSCLOCK_ARM11);
}

int64_t get_program_time () {
  return now_us();
}

// The 3DS framebuffer is rotated: each column of the screen is stored as
// a row of 240 BGR pixels, starting from the bottom.
static void present (gfxScreen_t screen, Canvas *c) {
  u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, NULL, NULL);
  for (int x = 0; x < c->w; x ++) {
    u8 *dst = fb + x * 240 * 3 + (240 - 1) * 3;
    const u8 *src = c->px + x * 3;
    for (int y = 0; y < c->h; y ++) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst -= 3;
      src += c->w * 3;
    }
  }
}

static void draw_screens (int64_t now, bool bottom) {
  ui_draw_top(&top_canvas, now);
  present(GFX_TOP, &top_canvas);
  // The bottom screen is single-buffered and changes less, so it is
  // redrawn less often to leave more time for the server
  if (bottom) {
    ui_draw_bottom(&bottom_canvas, has_ip ? ip_text : NULL, now);
    present(GFX_BOTTOM, &bottom_canvas);
  }
  gfxFlushBuffers();
  gfxSwapBuffers();
}

static void draw_message (const char *title, const char *line1, const char *line2) {
  ui_draw_message(&bottom_canvas, title, line1, line2);
  present(GFX_BOTTOM, &bottom_canvas);
  gfxFlushBuffers();
  gfxSwapBuffers();
  gspWaitForVBlank();
}

static void set_screens (bool on) {
  if (!gsplcd_ready || screens_on == on) return;
  if (on) GSPLCD_PowerOnAllBacklights();
  else GSPLCD_PowerOffAllBacklights();
  screens_on = on;
}

static void cleanup () {
  set_screens(true);
  if (gsplcd_ready) gspLcdExit();
  if (soc_buffer) {
    socExit();
    free(soc_buffer);
    soc_buffer = NULL;
  }
  aptSetSleepAllowed(true);
  gfxExit();
}

static void check_ip (int64_t now) {
  if (now - last_ip_check < IP_CHECK_US) return;
  last_ip_check = now;
  struct in_addr ip = { .s_addr = (in_addr_t)gethostid() };
  has_ip = ip.s_addr != 0 && ip.s_addr != 0xFFFFFFFF;
  if (has_ip) snprintf(ip_text, sizeof(ip_text), "%s", inet_ntoa(ip));
}

static void save_and_exit () {
  set_screens(true);
  draw_message("Salvando...", "O mundo fica no cartão SD,", "em /3ds/bareiron3ds/");
  writePlayerDataToDisk();
  #ifdef DISK_SYNC_BLOCKS_ON_INTERVAL
    writeBlockChangesToDisk(0, block_changes_count);
  #endif
  exit(0);
}

void task_yield () {
  int64_t now = now_us();
  if (now - last_frame < FRAME_US) return;
  last_frame = now;

  // Returns false when the system asks the app to close
  if (!aptMainLoop()) save_and_exit();

  hidScanInput();
  u32 down = hidKeysDown();
  if (down & KEY_START) save_and_exit();
  if (down & KEY_SELECT) set_screens(!screens_on);
  else if (down) set_screens(true);

  if (!screens_on) return;

  ui_map_work(MAP_BUDGET_US);

  if (now - last_draw >= DRAW_US) {
    last_draw = now;
    check_ip(now);
    ui_update(now);
    draw_screens(now, draw_count ++ % 5 == 0);
  }
}

void platform_fatal (int code) {
  set_screens(true);
  char line[48];
  snprintf(line, sizeof(line), "Erro %d. Confira o Wi-Fi e o SD.", code);
  draw_message("O servidor parou", line, "Aperte START para sair.");
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    gspWaitForVBlank();
  }
  exit(code);
}

int main () {
  gfxInitDefault();
  gfxSetDoubleBuffering(GFX_BOTTOM, false);
  atexit(cleanup);

  // New 3DS: faster CPU and extra cache
  osSetSpeedupEnable(true);
  // Keep the server running with the lid closed
  aptSetSleepAllowed(false);
  gsplcd_ready = R_SUCCEEDED(gspLcdInit());

  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/bareiron3ds", 0777);

  draw_message("Abrindo o mundo...", "Isso leva só um segundo.", NULL);

  soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFER_SIZE);
  if (!soc_buffer || R_FAILED(socInit(soc_buffer, SOC_BUFFER_SIZE))) {
    free(soc_buffer);
    soc_buffer = NULL;
    platform_fatal(1);
  }

  // Runs until START is pressed or the system closes the app
  bareiron_main();
  save_and_exit();
  return 0;
}
