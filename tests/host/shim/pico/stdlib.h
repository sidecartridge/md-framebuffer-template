/* Host stand-in for the Pico SDK's pico/stdlib.h: only what the firmware
 * units under test use. Code placement attributes mean nothing on the host. */
#ifndef HOST_SHIM_PICO_STDLIB_H
#define HOST_SHIM_PICO_STDLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define __not_in_flash_func(func_name) func_name
#define __time_critical_func(func_name) func_name

#endif
