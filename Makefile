# proxchunk — silent parallel CMake wrapper
NPROC      := $(shell nproc 2>/dev/null || echo 1)
BUILD_DIR  ?= build
BUILD_TYPE ?= Debug

.PHONY: all test tests verify clean profile release release-static dist dist-check

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) -j$(NPROC) -- -s

$(BUILD_DIR)/proxchunk $(BUILD_DIR)/proxchunkd $(BUILD_DIR)/test_plan $(BUILD_DIR)/test_repl: all

test: all
	$(BUILD_DIR)/test_plan
	$(BUILD_DIR)/test_repl
	@echo "==> CLI help/version/edges"
	bash tests/test_cli.sh $(BUILD_DIR)/proxchunk
	@echo "==> proxchunkd CLI + IPC + SIGINT"
	bash tests/test_proxchunkd.sh $(BUILD_DIR)/proxchunkd $(BUILD_DIR)/proxchunk
	@echo "==> REPL builtins"
	bash tests/test_repl.sh $(BUILD_DIR)/proxchunk
	@echo "==> local Range download"
	bash tests/test_download.sh $(BUILD_DIR)/proxchunk
	@echo "==> N-way plan concrete"
	gcc -std=gnu23 -Wall -Wextra -Wpedantic -I include \
	    -o $(BUILD_DIR)/verify_plan_run tests/verify_plan.c
	$(BUILD_DIR)/verify_plan_run
	@echo "All tests passed."

tests: test

CBMC ?= $(HOME)/.local/bin/cbmc
verify: test
	$(CBMC) --bounds-check --pointer-check --div-by-zero-check \
	    --unwind 17 --unwinding-assertions \
	    -I include tests/verify_plan.c
	@echo "formal: CBMC plan_n ok (UNIX IPC / libcurl not modeled)"

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
	@echo "Release binaries: build-release/proxchunk build-release/proxchunkd"

# Fully static: libcurl.a + local overlay nghttp2/brotli static-libs.
release-static:
	cmake -S . -B build-release-static -DCMAKE_BUILD_TYPE=Release -DSTATIC_LINK=ON
	cmake --build build-release-static -j$(NPROC) -- -s
	strip --strip-all -R .note.gnu.build-id -o build-release-static/proxchunk.stripped \
	    build-release-static/proxchunk
	strip --strip-all -R .note.gnu.build-id -o build-release-static/proxchunkd.stripped \
	    build-release-static/proxchunkd
	@echo "Static binaries: build-release-static/proxchunk build-release-static/proxchunkd"
	@echo "Stripped: build-release-static/proxchunk.stripped build-release-static/proxchunkd.stripped"

dist:
	bash scripts/build-glibc-gui.sh
	bash scripts/make-portable.sh
	bash scripts/make-src.sh

dist-check: dist
	bash tests/test_package.sh
