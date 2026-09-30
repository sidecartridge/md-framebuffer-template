/* Host stand-in for the Pico SDK's pico/time.h: the test provides the clock
 * and runs the repeating timers itself. */
#ifndef HOST_SHIM_PICO_TIME_H
#define HOST_SHIM_PICO_TIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct repeating_timer repeating_timer_t;
typedef bool (*repeating_timer_callback_t)(repeating_timer_t *rt);
struct repeating_timer {
  int64_t delay_us;
  repeating_timer_callback_t callback;
  void *user_data;
};

uint32_t time_us_32(void);
bool add_repeating_timer_us(int64_t delay_us, repeating_timer_callback_t callback,
                            void *user_data, repeating_timer_t *out);

#endif
