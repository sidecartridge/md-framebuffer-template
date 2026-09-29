/**
 * File: constants.h
 * Author: Diego Parrilla Santamaría
 * Date: November 2024, February 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Constants used in the placeholder file
 */

#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "hardware/vreg.h"

// Common macros
#define HEX_BASE 16
#define DEC_BASE 10

// Time macros
#define SEC_TO_MS 1000

// SELECT signal
#define SELECT_GPIO 5  // GPIO signal for SELECT

// SD card GPIO drive strength
#define SD_SPI_GPIO_DRIVE_STRENGTH GPIO_DRIVE_STRENGTH_2MA

// SD card SPI mode. Allowed values, from fatfs-sdk's my_spi.h and my_spi.c:
// 0: CPOL=0, CPHA=0. Sample on leading rising edge, shift on falling edge.
// 1: CPOL=0, CPHA=1. Sample on trailing falling edge, shift on rising edge.
// 2: CPOL=1, CPHA=0. Sample on leading falling edge, shift on rising edge.
// 3: CPOL=1, CPHA=1. Sample on trailing rising edge, shift on falling edge.
// The driver asserts spi_mode < 4. Measured on a SidecarTridge Multi-device at
// the default 24 MHz, reading 4 MB of raw sectors: mode 3 2,114 KB/s, mode 0
// 1,894 KB/s, the same data from both.
#define SD_SPI_MODE 3

// GPIO constants for the read address from the bus
#define READ_ADDR_GPIO_BASE 6     // Start of the GPIOs for the address
#define READ_ADDR_PIN_COUNT 16    // Number of GPIOs for the address
#define READ_SIGNAL_GPIO_BASE 27  // GPIO signal for READ
#define READ_SIGNAL_PIN_COUNT 1   // Number of GPIOs for the READ signal

// Write data to the bus
#define WRITE_DATA_GPIO_BASE \
  (READ_ADDR_GPIO_BASE)  // Start of the GPIOs for the data to write
#define WRITE_DATA_PIN_COUNT \
  (READ_ADDR_PIN_COUNT)  // Number of GPIOs for the data
#define WRITE_SIGNAL_GPIO_BASE \
  (READ_SIGNAL_GPIO_BASE + 1)  // GPIO signal for WRITE
#define WRITE_SIGNAL_PIN_COUNT 1

// FLASH and RAM sections constants.
#define ROM_BANKS 1  // Number of 64KB ROM banks to emulate (ROM4 only)
#define FLASH_ROM_LOAD_OFFSET \
  0x0  // Offset start in FLASH reserved for the ROM4 image. Survives reset
       // or poweroff. Only the first 64KB are used; ROM3 is no longer a
       // data bank (it is now used as the command channel).
#define FLASH_ROM4_LOAD_OFFSET FLASH_ROM_LOAD_OFFSET  // 64KB ROM4 block
#define ROM_SIZE_BYTES 0x10000                        // 64KBytes
#define ROM_SIZE_WORDS (ROM_SIZE_BYTES / 2)           // 32KWords
#define ROM_SIZE_LONGWORDS (ROM_SIZE_BYTES / 4)       // 16KLongWords

// Frequency constants.
#define SAMPLE_DIV_FREQ (1.f)         // Sample frequency division factor.
#define RP2040_CLOCK_FREQ_KHZ 225000  // Clock frequency in KHz (225MHz).

// Voltage constants.
#define RP2040_VOLTAGE VREG_VOLTAGE_1_10  // Voltage in 1.10 Volts.
#define VOLTAGE_VALUES                                                 \
  (const char *[]){"NOT VALID", "NOT VALID", "NOT VALID", "NOT VALID", \
                   "NOT VALID", "NOT VALID", "0.85v",     "0.90v",     \
                   "0.95v",     "1.00v",     "1.05v",     "1.10v",     \
                   "1.15v",     "1.20v",     "1.25v",     "1.30v",     \
                   "NOT VALID", "NOT VALID", "NOT VALID", "NOT VALID", \
                   "NOT VALID"}

// This is the APP KEY that will be used to identify the current app
// It mmust be a unique UUID4 for each app, and must be the one used in the
// app.json file as descriptor of the app
#ifndef CURRENT_APP_UUID_KEY
#define CURRENT_APP_UUID_KEY "PLACEHOLDER"
#endif

// Time macros
#define GET_CURRENT_TIME() \
  (((uint64_t)timer_hw->timerawh) << 32u | timer_hw->timerawl)
#define GET_CURRENT_TIME_INTERVAL_MS(start) \
  (uint32_t)((GET_CURRENT_TIME() - start) / \
             (((uint32_t)RP2040_CLOCK_FREQ_KHZ) / 1000))

// NOLINTBEGIN(readability-identifier-naming)
extern unsigned int __flash_binary_start;
extern unsigned int _rom_temp_start;
extern unsigned int _booster_app_flash_start;
extern unsigned int _config_flash_start;
extern unsigned int _global_lookup_flash_start;
extern unsigned int _global_config_flash_start;
// The 64 KB cartridge window. An array of unknown size, not a scalar: code
// reads and writes the whole window through this symbol, and a scalar
// declaration tells the compiler the object is 4 bytes, which makes every
// access past them undefined behaviour it may optimise on.
extern unsigned int __rom_in_ram_start__[];
// NOLINTEND(readability-identifier-naming)

#endif  // CONSTANTS_H