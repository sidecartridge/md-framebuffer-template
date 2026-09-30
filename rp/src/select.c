#include "select.h"

#include "hardware/sync.h"

#define SELECT_US_PER_MS 1000U
#define SELECT_DEBOUNCE_US ((uint32_t)SELECT_DEBOUNCE_MS * SELECT_US_PER_MS)
#define SELECT_LONG_RESET_US ((uint32_t)SELECT_LONG_RESET * SELECT_US_PER_MS)

static reset_callback_t reset_cb = NULL;
static reset_callback_t reset_long_cb = NULL;

// Edge history written by the GPIO interrupt. It lets select_poll() see a
// press that started and ended while core 0 was busy (an SD stall, a Wi-Fi
// wait). Times are time_us_32() values; differences stay valid for 35 min.
static volatile bool edgeSeen = false;
static volatile uint32_t lastEdgeUs = 0;
static volatile bool riseSeen = false;
static volatile uint32_t riseUs = 0;  // first rise of a candidate press
static volatile bool fallSeen = false;
static volatile uint32_t fallUs = 0;  // last fall after that rise

// Debounced state, owned by select_poll(). The level is also timed here, not
// only by the interrupt: the edge detector reads the pad before the input
// override, so it misses presses forced through the override (swd.py select).
static bool lastLevel = false;
static uint32_t lastLevelChangeUs = 0;
static bool pressed = false;
static bool longPressHandled = false;
static uint32_t pressStartUs = 0;

static void __not_in_flash_func(select_gpioIrq)(void) {
  uint32_t events = gpio_get_irq_event_mask(SELECT_GPIO) &
                    (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
  if (events == 0) {
    return;
  }
  gpio_acknowledge_irq(SELECT_GPIO, events);

  uint32_t now = time_us_32();
  edgeSeen = true;
  lastEdgeUs = now;
  if ((events & GPIO_IRQ_EDGE_RISE) != 0U && !riseSeen) {
    riseSeen = true;
    riseUs = now;
  }
  if ((events & GPIO_IRQ_EDGE_FALL) != 0U && riseSeen) {
    fallSeen = true;
    fallUs = now;
  }
}

static void select_clearPressHistory(void) {
  uint32_t ints = save_and_disable_interrupts();
  riseSeen = false;
  fallSeen = false;
  restore_interrupts(ints);
}

static void select_runShort(void) {
  if (reset_cb != NULL) {
    DPRINTF("Short press detected. Executing short press callback\n");
    reset_cb();
  }
}

static void select_runLong(void) {
  if (reset_long_cb != NULL) {
    DPRINTF("Long press detected. Executing long press callback\n");
    reset_long_cb();
  }
}

void select_configure() {
  // Configure the input ping for SELECT button
  gpio_init(SELECT_GPIO);
  gpio_set_dir(SELECT_GPIO, GPIO_IN);
  gpio_set_pulls(SELECT_GPIO, false, true);  // Pull down (false, true)
  gpio_pull_down(SELECT_GPIO);

  // A raw handler, so the CYW43 driver's own GPIO interrupt is not replaced.
  gpio_add_raw_irq_handler(SELECT_GPIO, select_gpioIrq);
  gpio_set_irq_enabled(SELECT_GPIO, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                       true);
  irq_set_enabled(IO_IRQ_BANK0, true);
}

bool select_detectPush() { return (gpio_get(SELECT_GPIO) != 0); }

void select_poll(void) {
  uint32_t now = time_us_32();
  bool level = select_detectPush();

  uint32_t ints = save_and_disable_interrupts();
  bool anyEdge = edgeSeen;
  uint32_t lastEdge = lastEdgeUs;
  bool rose = riseSeen;
  uint32_t rise = riseUs;
  bool fell = fallSeen;
  uint32_t fall = fallUs;
  restore_interrupts(ints);

  if (level != lastLevel) {
    lastLevel = level;
    lastLevelChangeUs = now;
  }
  bool settled = (now - lastLevelChangeUs) >= SELECT_DEBOUNCE_US &&
                 (!anyEdge || (now - lastEdge) >= SELECT_DEBOUNCE_US);

  if (!pressed) {
    if (!settled) {
      return;
    }
    if (level) {
      pressed = true;
      longPressHandled = false;
      pressStartUs = rose ? rise : lastLevelChangeUs;
      select_clearPressHistory();
      DPRINTF("SELECT button pushed\n");
      return;
    }
    // Released and stable: a whole press may have happened since the last
    // call. Shorter than the debounce window, it was a bounce.
    if (rose && fell && (fall - rise) >= SELECT_DEBOUNCE_US) {
      uint32_t heldMs = (fall - rise) / SELECT_US_PER_MS;
      select_clearPressHistory();
      DPRINTF("SELECT pressed and released while busy, %lu ms\n",
              (unsigned long)heldMs);
      if (heldMs >= SELECT_LONG_RESET) {
        select_runLong();
      } else {
        select_runShort();
      }
      return;
    }
    if (rose) {
      select_clearPressHistory();
    }
    return;
  }

  if (!longPressHandled && (now - pressStartUs) >= SELECT_LONG_RESET_US) {
    longPressHandled = true;
    select_runLong();
    return;
  }

  if (level || !settled) {
    return;
  }

  pressed = false;
  select_clearPressHistory();
  DPRINTF("SELECT button released after %lu ms\n",
          (unsigned long)((now - pressStartUs) / SELECT_US_PER_MS));
  if (!longPressHandled) {
    select_runShort();
  }
}

void select_setResetCallback(reset_callback_t reset) { reset_cb = reset; }
void select_setLongResetCallback(reset_callback_t resetLong) {
  reset_long_cb = resetLong;
}
