# tests/Makefile.inc — included by the root Makefile.
# Provides: test, test-offline, test-loader, test-maskrom,
#           test-deps, test-clean

BATS      ?= bats
PYTHON    ?= python3
TEST_DIR  := tests
BUILD_DIR := $(TEST_DIR)/build

# Absolute paths so LD_PRELOAD and TOOL work from any working directory.
ABS_TOOL   := $(abspath $(TARGET))
ABS_FIX    := $(abspath $(BUILD_DIR)/fixtures)
ABS_TEST   := $(abspath $(TEST_DIR))
ABS_BUILD  := $(abspath $(BUILD_DIR))
ABS_EMUSO  := $(abspath $(BUILD_DIR)/libusb_emu.so)

.PHONY: test test-offline test-loader test-maskrom test-deps test-clean

# ---------------------------------------------------------------------------
# Dependency check
# ---------------------------------------------------------------------------
test-deps:
	@command -v $(BATS)   >/dev/null 2>&1 || \
		{ echo "ERROR: bats not found (apt install bats)"; exit 1; }
	@command -v $(PYTHON) >/dev/null 2>&1 || \
		{ echo "ERROR: python3 not found"; exit 1; }
	@$(PYTHON) -c "import json, socket, struct, os" 2>/dev/null || \
		{ echo "ERROR: python3 standard modules not available"; exit 1; }

# ---------------------------------------------------------------------------
# Fixture generation
# ---------------------------------------------------------------------------
$(BUILD_DIR)/fixtures/.stamp: $(TEST_DIR)/fixtures/gen_fixtures.py
	@mkdir -p $(BUILD_DIR)/fixtures
	@$(PYTHON) $< $(BUILD_DIR)/fixtures
	@touch $@

# ---------------------------------------------------------------------------
# Build the libusb mock shared library
# ---------------------------------------------------------------------------
$(BUILD_DIR)/libusb_emu.so: $(TEST_DIR)/lib/libusb_emu.c | $(BUILD_DIR)
	$(CC) -shared -fPIC -o $@ $< \
		$(LIBUSB_CFLAGS) $(CSTD) -Wall -O2

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ---------------------------------------------------------------------------
# Test targets
# ---------------------------------------------------------------------------

test-offline: $(TARGET) $(BUILD_DIR)/fixtures/.stamp
	TOOL=$(ABS_TOOL) \
	FIXTURE_DIR=$(ABS_FIX) \
		$(BATS) $(ABS_TEST)/offline.bats

test-loader: $(TARGET) $(BUILD_DIR)/libusb_emu.so $(BUILD_DIR)/fixtures/.stamp
	TOOL=$(ABS_TOOL) \
	FIXTURE_DIR=$(ABS_FIX) \
	TEST_DIR=$(ABS_TEST) \
	BUILD_DIR=$(ABS_BUILD) \
	EMU_SO=$(ABS_EMUSO) \
		$(BATS) $(ABS_TEST)/loader.bats

test-maskrom: $(TARGET) $(BUILD_DIR)/libusb_emu.so $(BUILD_DIR)/fixtures/.stamp
	TOOL=$(ABS_TOOL) \
	FIXTURE_DIR=$(ABS_FIX) \
	TEST_DIR=$(ABS_TEST) \
	BUILD_DIR=$(ABS_BUILD) \
	EMU_SO=$(ABS_EMUSO) \
		$(BATS) $(ABS_TEST)/maskrom.bats

# Run all suites in order (offline first, then integration)
test: test-deps test-offline test-loader test-maskrom

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
test-clean:
	rm -rf $(BUILD_DIR)
