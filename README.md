# proxchunk

Multi-proxy HTTP Range downloader for Linux.

It splits a file into Range chunks and fetches them through different
proxy IPs so per-IP or per-connection throttling is less effective.

- Scores HTTP and SOCKS5 proxies (public lists, optional user list, optional Tor)
- Downloads chunks in parallel, then assembles the file in order
- TUI progress bars (one per chunk plus a total)

The server must support HTTP Range (`Accept-Ranges: bytes`, 206 responses).

## Build

Requires **g++** (C++23) and **libcurl**.

```bash
make
make test
```

Optimized release:

```bash
make release
```

A portable fully static **musl** binary (no glibc NSS) is built with
`-static -static-libgcc -static-libstdc++ -fno-pie -no-pie -Wl,--build-id=none`
against musl libcurl. `make release-static` is a glibc static link; prefer musl
for shipping to other machines.

## Usage

```text
proxchunk [options] <URL>
```

| Option | Meaning |
|--------|---------|
| `-o`, `--output FILE` | Output path (default: URL basename) |
| `-c`, `--concurrent N` | Max parallel chunks (default: logical CPU count) |
| `-s`, `--chunk-mb MB` | Chunk size in MiB (default: 8) |
| `-p`, `--proxies N` | Live proxies to keep (default: 40) |
| `-r`, `--refresh SEC` | Pool refresh interval (default: 180) |
| `--limit-mb MB` | Download only the first MB (0 = full file) |
| `--direct` | Single IP, no proxies |
| `--no-progress` | No TUI bars |
| `--no-cache` | Do not use `~/.cache/proxchunk/proxies.txt` |
| `--show-proxies` | Show a 15-character IPv4 field on each chunk bar |
| `--socks URL` | Extra HTTP or SOCKS proxy (repeatable) |
| `--proxy-file FILE` | Extra `ip:port` or `scheme://host:port` list |
| `--no-user-proxies` | Skip `~/.config/proxchunk/proxies.txt` |
| `--no-tor` | Do not auto-add `socks5h://127.0.0.1:9050` |
| `-h`, `--help` | Help |
| `-v`, `--version` | Version |

Arguments and options may appear in any order.

### User proxy list

Create `~/.config/proxchunk/proxies.txt` if you have your own endpoints:

```text
# comments allowed
1.2.3.4:8080
socks5h://10.0.0.1:1080
```

Bare `host:port` is HTTP. Those entries are speed-tested first so they are
not skipped when the public list is large.

### Example

```bash
proxchunk --show-proxies -c 8 -s 8 -o file.bin 'https://example.com/file.bin'
```

## Notes

- Free public proxies are often slow or dead. A private list via
  `--proxy-file` is more reliable.
- Stalled chunks are aborted and requeued (up to eight tries). If a chunk
  still fails, the output is not assembled.
- No resume of a partial output file yet.
- For sites that hide the real file behind cookies or a landing page, resolve
  the direct URL first, then pass that URL to proxchunk.

## Files

| Path | Role |
|------|------|
| `~/.config/proxchunk/proxies.txt` | Optional user proxy list |
| `~/.cache/proxchunk/proxies.txt` | Scored cache from previous runs |

## Release tarballs

`make dist` writes:

- `dist/proxchunk-<version>-<arch>.tar.gz` — binary tree (run `./proxchunk`, optional `sudo ./install.sh`). README in that archive is end-user only (no build steps).
- `dist/proxchunk-<version>-src.tar.gz` — this source tree (`make` / `make test`)

```bash
tar -xzf proxchunk-1.0-x86_64.tar.gz
cd proxchunk-1.0-x86_64
./proxchunk --help
sudo ./install.sh
sudo ./uninstall.sh
```

## License

MIT
