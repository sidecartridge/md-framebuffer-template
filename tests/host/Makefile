# Host tests: plain C and Python checks of the firmware's pure logic, built
# with the host compiler (no Pico SDK) and run in seconds.
#
#   make -C tests/host test     # build and run everything; fails on any failure
#   make -C tests/host clean
#
# Every test_*.c here is one program: it includes the firmware units it tests
# from rp/src, with shim/ standing in for the few SDK headers they pull in, and
# exits non-zero when a check fails. Every test_*.py is a unittest module.
# Address and undefined-behaviour sanitizers make a write past a buffer fail
# the test instead of passing by luck.

CC ?= cc
PYTHON ?= python3
REPO := ../..
BUILD := build

CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -Wno-unknown-pragmas \
          -fsanitize=address,undefined -fno-omit-frame-pointer \
          -fno-sanitize-recover=undefined \
          -I shim -I $(REPO)/rp/src/include
LDFLAGS := -fsanitize=address,undefined -pthread

C_TESTS := $(patsubst %.c,$(BUILD)/%,$(wildcard test_*.c))
# Tests that also run with the 25 fps profile (rp/src/include/profile.h).
PROFILE_TESTS := test_audio_dma
C_TESTS += $(patsubst %,$(BUILD)/%-25fps,$(PROFILE_TESTS))

.PHONY: test c-tests py-tests clean FORCE

test: c-tests py-tests

c-tests: $(C_TESTS)
	@set -e; for t in $(C_TESTS); do echo "== $$t"; ./$$t; done

py-tests:
	@$(if $(wildcard test_*.py),$(PYTHON) -m unittest discover -s . -p 'test_*.py',echo "no Python tests")

# A test's own firmware sources are listed in its first line:
#   // units: rp/src/fb_blit.c
# Always rebuilt (it takes about a second): a test must never pass on a binary
# older than the firmware source it checks.
$(BUILD)/%: %.c FORCE | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< \
	  $(addprefix $(REPO)/,$(shell sed -n '1s|^// units: ||p' $<)) $(LDFLAGS)

$(BUILD)/%-25fps: %.c FORCE | $(BUILD)
	$(CC) $(CFLAGS) -DAPP_PROFILE=PROFILE_25FPS -o $@ $< \
	  $(addprefix $(REPO)/,$(shell sed -n '1s|^// units: ||p' $<)) $(LDFLAGS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

FORCE:
