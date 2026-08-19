# proxchunk — silent parallel CMake wrapper
NPROC      := $(shell nproc 2>/dev/null || echo 1)
BUILD_DIR  ?= build
BUILD_TYPE ?= Debug

.PHONY: all test tests verify clean

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j$(NPROC) -- -s

$(BUILD_DIR)/proxchunk $(BUILD_DIR)/test_plan: all

test: all
	$(BUILD_DIR)/test_plan
	@echo "==> CLI help/version"
	$(BUILD_DIR)/proxchunk -h >/dev/null
	$(BUILD_DIR)/proxchunk --help >/dev/null
	$(BUILD_DIR)/proxchunk -v
	$(BUILD_DIR)/proxchunk --version
	@echo "All tests passed."

tests: test

verify: test
	@echo "formal: not run (no CBMC model for libcurl I/O)"

clean:
	rm -rf $(BUILD_DIR)
