/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: Template code for the core emulation
 */

#include "emul.h"

#include <stdint.h>

// inclusw in the C file to avoid multiple definitions
#include "aconfig.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "display.h"
#include "ff.h"
#include "gconfig.h"
#include "memfunc.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"

#define SLEEP_LOOP_MS 100

enum {
  APP_MODE_SETUP = 255  // Setup
};

// Command handlers
static void cmdMenu(const char *arg);
static void cmdClear(const char *arg);
static void cmdExit(const char *arg);
static void cmdFirmware(const char *arg);
static void cmdHelp(const char *arg);
static void cmdBooster(const char *arg);
static void cmdSettings(const char *arg);
static void cmdPrint(const char *arg);
static void cmdSave(const char *arg);
static void cmdErase(const char *arg);
static void cmdGet(const char *arg);
static void cmdPutInt(const char *arg);
static void cmdPutBool(const char *arg);
static void cmdPutString(const char *arg);

// Command table
static const Command commands[] = {
    {"m", cmdMenu},
    {"h", cmdHelp},
    {"e", cmdExit},
    {"f", cmdFirmware},
    {"x", cmdBooster},
    {"?", cmdHelp},
    {"s", cmdSettings},
    {"settings", cmdSettings},
    {"print", cmdPrint},
    {"save", cmdSave},
    {"erase", cmdErase},
    {"get", cmdGet},
    {"put_int", cmdPutInt},
    {"put_bool", cmdPutBool},
    {"put_str", cmdPutString},
};

// Number of commands in the table
static const size_t numCommands = sizeof(commands) / sizeof(commands[0]);

// Keep active loop or exit
static bool keepActive = true;
static bool menuScreenActive = false;
static absolute_time_t menuRefreshTime;

// Polling tick used as the network poll callback so command handling stays
// alive during multi-second WiFi operations.
static void __not_in_flash_func(emul_pollTick)(void) {
  chandler_loop();
  term_loop();
}

#define MENU_REFRESH_TIME_MS 1000

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

static void showTitle() {
  term_printString(
      "\x1B"
      "E"
      "Microfirmware test app - " RELEASE_VERSION "\n");
}

static void menu(void) {
  menuScreenActive = true;
  showTitle();
  term_printString("\n\n");
  term_printString("[S]ettings     | [F]irmware launch\n");
  term_printString("[E]xit desktop | [X] Back to Booster\n\n");

  // Display network information
  term_printNetworkInfo();

  term_printString("\n");
  term_printString("Select an option: ");
  term_markMenuPromptCursor();
  menuRefreshTime = make_timeout_time_ms(MENU_REFRESH_TIME_MS);
}

// Command handlers
void cmdMenu(const char *arg) { menu(); }

void cmdHelp(const char *arg) {
  menuScreenActive = false;
  // term_printString("\x1B" "E" "Available commands:\n");
  term_printString("Available commands:\n");
  term_printString(" General:\n");
  term_printString("  clear   - Clear the terminal screen\n");
  term_printString("  exit    - Exit the terminal\n");
  term_printString("  f       - Launch user firmware on the Atari ST\n");
  term_printString("  help    - Show available commands\n");
}

void cmdClear(const char *arg) {
  menuScreenActive = false;
  term_clearScreen();
}

void cmdExit(const char *arg) {
  menuScreenActive = false;
  term_printString("Exiting terminal...\n");
  // Send continue to desktop command
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
}

void cmdFirmware(const char *arg) {
  menuScreenActive = false;
  term_printString("Launching user firmware on the Atari ST...\n");
  // Write CMD_START into the cartridge sentinel slot. The m68k's
  // check_commands macro polls the slot every vsync; on CMD_START it
  // beq's into rom_function, which jmp's to USERFW (target/atarist/src/
  // userfw.s). The default userfw demo prints
  // "Example firmware load..." via Cconws and returns.
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
}

void cmdBooster(const char *arg) {
  menuScreenActive = false;
  term_printString("Launching Booster app...\n");
  term_printString("The computer will boot shortly...\n\n");
  term_printString("If it doesn't boot, power it on and off.\n");
  resetDeviceAtBoot = false;  // Jump to the booster app
  keepActive = false;         // Exit the active loop
}

void cmdSettings(const char *arg) {
  menuScreenActive = false;
  term_cmdSettings(arg);
}

void cmdPrint(const char *arg) {
  menuScreenActive = false;
  term_cmdPrint(arg);
}

void cmdSave(const char *arg) {
  menuScreenActive = false;
  term_cmdSave(arg);
}

void cmdErase(const char *arg) {
  menuScreenActive = false;
  term_cmdErase(arg);
}

void cmdGet(const char *arg) {
  menuScreenActive = false;
  term_cmdGet(arg);
}

void cmdPutInt(const char *arg) {
  menuScreenActive = false;
  term_cmdPutInt(arg);
}

void cmdPutBool(const char *arg) {
  menuScreenActive = false;
  term_cmdPutBool(arg);
}

void cmdPutString(const char *arg) {
  menuScreenActive = false;
  term_cmdPutString(arg);
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString("Configuring network... please wait...\n");

  display_refresh();
}

void failure(const char *message) {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString(message);

  display_refresh();
}

static void init(void) {
  // Set the command table
  term_setCommands(commands, numCommands);

  // Clear the screen
  term_clearScreen();

  // Display the menu
  menu();

  // Example 1: Move the cursor up one line.
  // VT52 sequence: ESC A (moves cursor up)
  // The escape sequence "\x1BA" will move the cursor up one line.
  // term_printString("\x1B" "A");
  // After moving up, print text that overwrites part of the previous line.
  // term_printString("Line 2 (modified by ESC A)\n");

  // Example 2: Move the cursor right one character.
  // VT52 sequence: ESC C (moves cursor right)
  // term_printString("\x1B" "C");
  // term_printString(" <-- Moved right with ESC C\n");

  // Example 3: Direct cursor addressing.
  // VT52 direct addressing uses ESC Y <row> <col>, where:
  //   row_char = row + 0x20, col_char = col + 0x20.
  // For instance, to move the cursor to row 0, column 10:
  //   row: 0 -> 0x20 (' ')
  //   col: 10 -> 0x20 + 10 = 0x2A ('*')
  // term_printString("\x1B" "Y" "\x20" "\x2A");
  // term_printString("Text at row 0, column 10 via ESC Y\n");

  // term_printString("\x1B" "Y" "\x2A" "\x20");

  display_refresh();
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = APP_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF(
        "APP_MODE_SETUP not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  // In this example, the flow will always start the configuration app first
  // The ROM Emulator app for example will check here if the start directly
  // in emulation mode is needed or not

  // 3. If we are here, it means the app is not in emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

  // Initialize the cartridge ROM4 read engine. ROM4 reads are served entirely
  // by chained DMAs feeding the PIO TX FIFO — no CPU/IRQ involvement.
  // Without this engine the cartridge image is unreadable from the m68k,
  // so a failure here is fatal: panic instead of stumbling on with a half-
  // configured PIO/DMA setup.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // Bring up the ROM3 command capture (PIO + DMA ring on GPIO 26) and the
  // command handler that polls the ring, parses the protocol, and dispatches
  // each command to the registered callbacks. commemul is similarly load-
  // bearing — without it the m68k can issue commands but the RP never sees
  // them, so any non-OK return is fatal.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(term_command_cb);

  // After this point, the remote computer can execute the code

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.
  // If you want to use a custom app in the remote computer, you can do it.
  // But it's easier to debug and code in the rp2040

  // Initialize the display
  display_setupU8g2();

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM, floppies, even
  // full hard disk files, configuration files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app continues and reports SD status in the terminal menu.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.
  // If there is no folder in the micro SD card, the app will create it.

  FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/test";  // MODIFY THIS TO YOUR FOLDER NAME
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("SD card unavailable (error %i). Continuing without SD.\n",
            sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // Pre-init the stuff
  // In this example it only prints the please wait message, but can be used as
  // a place to put other code that needs to be run before the network is
  // initialized
  preinit();

  // 6. Init the network, if needed
  // It's always a good idea to wait for the network to be ready
  // Get the WiFi mode from the settings
  // If you are developing code that does not use the network, you can
  // comment this section
  // It's important to note that the network parameters are taken from the
  // global configuration of the Booster app. The network parameters are
  // ready only for the microfirmware apps.
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  wifi_mode_t wifiModeValue = WIFI_MODE_STA;
  if (wifiMode == NULL) {
    DPRINTF("No WiFi mode found in the settings. No initializing.\n");
  } else {
    wifiModeValue = (wifi_mode_t)atoi(wifiMode->value);
    if (wifiModeValue != WIFI_MODE_AP) {
      DPRINTF("WiFi mode is STA\n");
      wifiModeValue = WIFI_MODE_STA;
      int err = network_wifiInit(wifiModeValue);
      if (err != 0) {
        DPRINTF("Error initializing the network: %i. No initializing.\n", err);
      } else {
        // Drain commands and run the terminal loop during WiFi polling so
        // commands sent during the (potentially multi-second) connect don't
        // pile up in the ROM3 ring.
        network_setPollingCallback(emul_pollTick);
        // Connect to the WiFi network
        int maxAttempts = 3;  // or any other number defined elsewhere
        int attempt = 0;
        err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;

        while ((attempt < maxAttempts) &&
               (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
          err = network_wifiStaConnect();
          attempt++;

          if ((err > 0) && (err < NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
            DPRINTF("Error connecting to the WiFi network: %i\n", err);
          }
        }

        if (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT) {
          DPRINTF("Timeout connecting to the WiFi network after %d attempts\n",
                  maxAttempts);
          // Optionally, return an error code here.
        }
        network_setPollingCallback(NULL);
      }
    } else {
      DPRINTF("WiFi mode is AP. No initializing.\n");
    }
  }

  // 7. Configure the SELECT button so menu status can show it immediately.
  select_configure();

  // 8. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init();

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to exit.
  // For testing purposes, this app only shows commands to manage the settings
  DPRINTF("Start the app loop here\n");
  while (getKeepActive()) {
#if PICO_CYW43_ARCH_POLL
    network_safePoll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(SLEEP_LOOP_MS));
#else
    sleep_ms(SLEEP_LOOP_MS);
#endif
    // Drain the ROM3 command ring → dispatch to registered callbacks.
    chandler_loop();

    // Run the terminal foreground (consume the published command, render
    // output, etc.).
    term_loop();

    if (menuScreenActive) {
      char *input = term_getInputBuffer();
      bool hasPendingInput = (input != NULL) && (input[0] != '\0');
      if (!hasPendingInput &&
          (absolute_time_diff_us(get_absolute_time(), menuRefreshTime) <= 0)) {
        term_refreshMenuLiveInfo();
        menuRefreshTime = make_timeout_time_ms(MENU_REFRESH_TIME_MS);
      }
    }
  }

  // 10. Send RESET computer command
  // Ok, so we are done with the setup but we want to reset the computer to
  // reboot in the same microfirmware app or start the booster app

  sleep_ms(SLEEP_LOOP_MS);
  // We must reset the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(SLEEP_LOOP_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Set emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         APP_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Jump to the booster app
    DPRINTF("Jumping to the booster app...\n");
    reset_jump_to_booster();
  }
}
