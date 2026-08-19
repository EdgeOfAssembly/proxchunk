# proxchunk

**Fully automatic multi-proxy Range chunked downloader for Linux**

Bypasses per-IP download speed limits / throttling on filesharing and other sites by:

1. Automatically fetching free HTTP proxy lists from multiple public sources
2. Concurrently testing them, dropping dead ones, and sorting the live pool by measured download speed
3. Continuously refreshing the pool in the background
4. Probing the target URL for `Content-Length` + `Accept-Ranges` support
5. Splitting the file into chunks and downloading each chunk concurrently through a *different* proxy (each with its own IP)
6. Assembling the parts into the final file in correct order

Written in modern **C++23**, uses **libcurl**.

## Build

```bash
# Requires: g++ with C++23 support (GCC 13+), libcurl development headers
sudo apt install g++ libcurl4-openssl-dev   # Debian/Ubuntu

g++ -std=c++23 -O2 -Wall -Wextra -o proxchunk src/proxchunk.cpp -lcurl -pthread
```

Or with CMake:

```bash
mkdir build && cd build
cmake ..
make -j
```

## Usage

```bash
./proxchunk <URL> [options]

Options:
  -o, --output <file>   Output path (default: basename of URL)
  -c, --concurrent <N>  Max concurrent chunk downloads (default: 16)
  -s, --chunk-mb <MB>   Chunk size in megabytes (default: 8)
  -p, --proxies <N>     Max proxies to keep in pool (default: 40)
  -r, --refresh <sec>   Proxy refresh interval (default: 180)
      --limit-mb <MB>   Download only the first MB (0 = full file)
      --direct          Single-IP download (no proxies)
      --no-progress     Do not draw the TUI progress bar
  -h, --help            Show help
  -v, --version         Print version
```

Progress uses `#include <libsf/tui/progress_bar.h>` (`-I/usr/local/include`).

### Example

```bash
./proxchunk "https://example.com/largefile.zip" -o largefile.zip -c 24 -s 10
```

The tool will:

- Fetch & test proxies (may take 20–60 s the first time)
- Probe the file for size and Range support
- Download in parallel chunks via different proxies
- Show progress
- Assemble the final file

## Notes & Limitations

- The remote server **must** support HTTP Range requests (`Accept-Ranges: bytes` and proper 206 responses). Most direct download links do; some intermediate / captcha pages do not.
- Free public proxies are often slow, unstable, or already overloaded. Expect variable results. For serious use, replace the sources with your own paid residential / datacenter proxy list (edit the sources vector in the code).
- Currently focused on HTTP proxies. SOCKS support can be added.
- For filesharing sites that require cookies, referers, or special headers to get the final direct link, resolve that URL first (e.g. with plowshare, yt-dlp, or a browser), then feed the clean direct URL to proxchunk.
- No resume yet in this version (can be added).
- Tested on Linux. Other platforms may work with minor changes.

## License

MIT

## Credits

Inspired by the need for a true multi-IP segmented downloader. Built by the Grok team for the user.
