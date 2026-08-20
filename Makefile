# proxchunk — silent parallel CMake wrapper
NPROC      := $(shell nproc 2>/dev/null || echo 1)
BUILD_DIR  ?= build
BUILD_TYPE ?= Debug

.PHONY: all test tests verify clean profile release release-static dist dist-check

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j$(NPROC) -- -s

$(BUILD_DIR)/proxchunk $(BUILD_DIR)/test_plan $(BUILD_DIR)/test_repl: all

test: all
	$(BUILD_DIR)/test_plan
	$(BUILD_DIR)/test_repl
	@echo "==> CLI help/version/edges"
	bash tests/test_cli.sh $(BUILD_DIR)/proxchunk
	@echo "==> REPL builtins"
	bash tests/test_repl.sh $(BUILD_DIR)/proxchunk
	@echo "==> local Range download"
	bash tests/test_download.sh $(BUILD_DIR)/proxchunk
	@echo "All tests passed."

tests: test

verify: test
	@echo "formal: not run (no CBMC model for libcurl I/O)"

clean:
	rm -rf $(BUILD_DIR) build-profile build-release build-release-static \
	    gmon.out profile.txt profile-before.txt profile-after.txt

profile:
	cmake -S . -B build-profile -DCMAKE_BUILD_TYPE=Profile
	cmake --build build-profile -j$(NPROC) -- -s
	@echo "Profile binary: build-profile/proxchunk"

release:
	cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
	cmake --build build-release -j$(NPROC) -- -s
	@echo "Release binary: build-release/proxchunk"

# Fully static: libcurl.a + local overlay nghttp2/brotli static-libs.
release-static:
	cmake -S . -B build-release-static -DCMAKE_BUILD_TYPE=Release -DSTATIC_LINK=ON
	cmake --build build-release-static -j$(NPROC) -- -s
	strip --strip-all -R .note.gnu.build-id -o build-release-static/proxchunk.stripped \
	    build-release-static/proxchunk
	@echo "Static binary: build-release-static/proxchunk"
	@echo "Stripped: build-release-static/proxchunk.stripped"

dist:
	bash scripts/make-portable.sh
	bash scripts/make-src.sh

dist-check: dist
	bash tests/test_package.sh
