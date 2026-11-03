// Nintendo 3DS platform layer for bareiron.
//
// Sets up the screens, Wi-Fi sockets and SD card, then runs the normal
// bareiron server loop. bareiron calls task_yield() often; we use it to
// keep the HOME button working, read buttons and redraw the top screen.

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "globals.h"
#include "serialize.h"
#include "platform_3ds.h"

#define SOC_BUFFER_SIZE 0x100000
#define FRAME_US 16000        // check buttons/HOME ~60 times per second
#define STATUS_US 500000      // redraw the top screen twice per second
#define TOP_COLS 50

int bareiron_main ();

static u32 *soc_buffer = NULL;
static PrintConsole top_console, bottom_console;
static int64_t last_frame = 0, last_status = 0;
static bool screens_on = true;
static bool gsplcd_ready = false;

static int64_t now_us () {
  u64 ticks = svcGetSystemTick();
  return (int64_t)(ticks / SYSCLOCK_ARM11) * 1000000LL +
    (int64_t)((ticks % SYSCLOCK_ARM11) * 1000000ULL / SYSCLOCK_ARM11);
}

int64_t get_program_time () {
  return now_us();
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

// Prints one line of the top screen, padded so old text gets erased
static void status_line (int row, const char *fmt, ...) {
  char line[TOP_COLS + 1];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  printf("\x1b[%d;1H%-*s", row, TOP_COLS - 1, line);
}

static void draw_status () {
  consoleSelect(&top_console);

  struct in_addr ip = { .s_addr = (in_addr_t)gethostid() };
  bool has_wifi = ip.s_addr != 0 && ip.s_addr != 0xFFFFFFFF;

  int row = 2;
  status_line(row++, "  \x1b[32mbareiron 3DS\x1b[0m");
  status_line(row++, "  Servidor de Minecraft Java 1.21.8");
  status_line(row++, "");
  if (has_wifi) {
    status_line(row++, "  No Minecraft, Multijogador > Adicionar servidor");
    status_line(row++, "  e use o endereco:");
    status_line(row++, "");
    status_line(row++, "      \x1b[33m%s\x1b[0m", inet_ntoa(ip));
    status_line(row++, "");
    status_line(row++, "  (porta %d, a padrao do Minecraft)", PORT);
  } else {
    status_line(row++, "  \x1b[31mSem Wi-Fi.\x1b[0m Conecte o 3DS a rede nas");
    status_line(row++, "  Configuracoes do Console. O servidor");
    status_line(row++, "  comeca sozinho quando a rede voltar.");
    status_line(row++, "");
    status_line(row++, "");
    status_line(row++, "");
  }
  status_line(row++, "");

  int online = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd != -1 && player_data[i].name[0]) online ++;
  }
  status_line(row++, "  Jogadores online: %d", online);

  int shown = 0;
  for (int i = 0; i < MAX_PLAYERS && shown < 10; i ++) {
    PlayerData *p = &player_data[i];
    if (p->client_fd == -1 || !p->name[0]) continue;
    char name[17];
    memcpy(name, p->name, 16);
    name[16] = 0;
    status_line(row++, "   %-16s x %6d  y %3d  z %6d", name, p->x, p->y, p->z);
    shown ++;
  }
  while (row < 27) status_line(row++, "");

  status_line(27, "  Mundo salvo no SD: /3ds/bareiron3ds/");
  status_line(29, "  \x1b[36mSTART\x1b[0m salvar e sair   \x1b[36mSELECT\x1b[0m apagar telas");

  consoleSelect(&bottom_console);
}

static void save_and_exit () {
  consoleSelect(&bottom_console);
  printf("\nSalvando o mundo...\n");
  writePlayerDataToDisk();
  #ifdef DISK_SYNC_BLOCKS_ON_INTERVAL
    writeBlockChangesToDisk(0, block_changes_count);
  #endif
  gfxFlushBuffers();
  gfxSwapBuffers();
  exit(0);
}

void task_yield () {
  int64_t time_now = now_us();
  if (time_now - last_frame < FRAME_US) return;
  last_frame = time_now;

  // Returns false when the system asks the app to close
  if (!aptMainLoop()) save_and_exit();

  hidScanInput();
  u32 down = hidKeysDown();
  if (down & KEY_START) save_and_exit();
  if (down & KEY_SELECT) set_screens(!screens_on);
  else if (down || hidKeysDownRepeat()) set_screens(true);

  if (time_now - last_status >= STATUS_US) {
    last_status = time_now;
    if (screens_on) draw_status();
  }

  gfxFlushBuffers();
  gfxSwapBuffers();
}

void platform_fatal (int code) {
  consoleSelect(&bottom_console);
  set_screens(true);
  printf("\n\x1b[31mErro %d.\x1b[0m O servidor parou.\n", code);
  printf("Confira o Wi-Fi e o cartao SD.\n");
  printf("Pressione START para sair.\n");
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START) break;
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
  }
  exit(code);
}

int main () {
  gfxInitDefault();
  consoleInit(GFX_TOP, &top_console);
  consoleInit(GFX_BOTTOM, &bottom_console);
  atexit(cleanup);

  // New 3DS: faster CPU and extra cache
  osSetSpeedupEnable(true);
  // Keep the server running with the lid closed
  aptSetSleepAllowed(false);
  gsplcd_ready = R_SUCCEEDED(gspLcdInit());

  mkdir("sdmc:/3ds", 0777);
  mkdir("sdmc:/3ds/bareiron3ds", 0777);

  consoleSelect(&bottom_console);

  soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFER_SIZE);
  if (!soc_buffer || R_FAILED(socInit(soc_buffer, SOC_BUFFER_SIZE))) {
    free(soc_buffer);
    soc_buffer = NULL;
    printf("Nao deu para iniciar a rede.\n");
    platform_fatal(1);
  }

  draw_status();

  // Runs until START is pressed or the system closes the app
  bareiron_main();
  save_and_exit();
  return 0;
}
