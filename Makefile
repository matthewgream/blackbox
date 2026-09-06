# blackbox — build + test the C library and the csv2json tool.
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -Iinclude
BUILD   ?= /tmp/blackbox-build

.PHONY: test test-none test-file test-flash test-unity csv2json-test clean

test: test-none test-file test-flash test-unity
	@echo "all tests passed"

# Unity / single-header build: the test TU itself pulls in the implementation (no src/blackbox.c).
test-unity:
	@mkdir -p $(BUILD)
	@echo "[test] unity (BLACKBOX_IMPLEMENTATION in the app TU)"
	@$(CC) $(CFLAGS) -DBLACKBOX_PERSIST=0 -DBLACKBOX_CLOCK=test_clock -DBLACKBOX_IMPLEMENTATION \
	    test/test_blackbox.c -o $(BUILD)/test_unity
	@$(BUILD)/test_unity

# BLACKBOX_PERSIST numeric values mirror blackbox.h (NONE=0, FILE=1).
test-none:
	@mkdir -p $(BUILD)
	@echo "[test] PERSIST_NONE"
	@$(CC) $(CFLAGS) -DBLACKBOX_PERSIST=0 -DBLACKBOX_CLOCK=test_clock \
	    src/blackbox.c test/test_blackbox.c -o $(BUILD)/test_none
	@$(BUILD)/test_none

test-file:
	@mkdir -p $(BUILD)
	@echo "[test] PERSIST_FILE"
	@$(CC) $(CFLAGS) -DBLACKBOX_PERSIST=1 -DBLACKBOX_CLOCK=test_clock \
	    src/blackbox.c test/test_blackbox.c -o $(BUILD)/test_file
	@$(BUILD)/test_file

# ESP_FLASH backend against a RAM-backed esp_partition mock (test/mock). Unity build so the backend
# and the test share the mock's single static partition buffer. BLACKBOX_PERSIST=2 mirrors ESP_FLASH.
test-flash:
	@mkdir -p $(BUILD)
	@echo "[test] PERSIST_ESP_FLASH (mock partition)"
	@$(CC) $(CFLAGS) -Itest/mock -DBLACKBOX_PERSIST=2 -DBLACKBOX_CLOCK=test_clock -DBLACKBOX_IMPLEMENTATION \
	    test/test_blackbox_flash.c -o $(BUILD)/test_flash
	@$(BUILD)/test_flash

csv2json-test:
	@node js/csv2json.js --definitions test/records.example.h < test/records.example.csv

clean:
	@rm -rf $(BUILD)
