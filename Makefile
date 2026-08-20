# FireWire_Lives - build for the portable core, tests and host tools.
#
# The dext itself needs Xcode's DriverKit SDK and is built separately (see
# docs/building.md). Everything here builds with the Command Line Tools alone.

CC      ?= clang
CFLAGS  := -std=c11 -Iinclude -Wall -Wextra -Wpedantic -Wshadow \
           -Wconversion -Wsign-conversion -Wno-gnu-zero-variadic-macro-arguments \
           -O2 -g
LDFLAGS :=

BUILD   := build
CORE_SRC := $(wildcard src/core/*.c)
CORE_OBJ := $(patsubst src/core/%.c,$(BUILD)/core/%.o,$(CORE_SRC))

TEST_SRC := $(wildcard tests/*.c)
TEST_OBJ := $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))

TEST_BIN := $(BUILD)/fwtest
PROBE_BIN := $(BUILD)/fwprobe

.PHONY: all test probe clean asan

all: $(TEST_BIN) $(PROBE_BIN)

# ---- core objects ----
$(BUILD)/core/%.o: src/core/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests -c $< -o $@

$(TEST_BIN): $(CORE_OBJ) $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ---- fwprobe: enumerates OHCI controllers via IOKit ----
$(PROBE_BIN): tools/fwprobe/fwprobe.c $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $^ -o $@ -framework IOKit -framework CoreFoundation

test: $(TEST_BIN)
	@$(TEST_BIN)

# Exit 2 just means "no controller attached", which is informational, not a
# build failure.
probe: $(PROBE_BIN)
	@$(PROBE_BIN) || true

# Address + UB sanitizers. The config ROM parser handles untrusted device
# input, so this is the run that matters for the bounds checks.
asan:
	@mkdir -p $(BUILD)/asan
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Itests $(CORE_SRC) $(TEST_SRC) -o $(BUILD)/asan/fwtest
	@$(BUILD)/asan/fwtest

clean:
	rm -rf $(BUILD)
