# proxchunk — silent parallel CMake wrapper
NPROC      := $(shell nproc 2>/dev/null || echo 1)
BUILD_DIR  ?= build
BUILD_TYPE ?= Debug

.PHONY: all test tests verify clean profile release

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
	rm -rf $(BUILD_DIR) build-profile build-release gmon.out profile.txt profile-before.txt profile-after.txt

# Release-like -O3 + gprof. Cleans the profile tree only (not debug).
profile:
	cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Profile
	cmake --build build-profile -j$(NPROC) -- -s
	@echo "Profile binary: build-profile/proxchunk"

release:
	cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
	cmake --build build-release -j$(NPROC) -- -s
	@echo "Release binary: build-release/proxchunk"
