# proxchunk

Multi-proxy HTTP Range downloader for Linux.

Extract this directory and run:

```bash
./proxchunk --help
./proxchunk -o file.bin 'https://example.com/file.bin'
./proxchunk-gui          # desktop window (VTE prompt; glibc Linux)
```

The menu launcher starts **proxchunk-gui**. At the `> ` prompt, type proxchunk
options and a URL, or `cd` / `mkdir` / `rm` / `rmdir` / `pwd` / `exit`.
Ctrl+V pastes (also middle-click). File → Quit (Ctrl+Q). Help → About.

The server must support HTTP Range (`Accept-Ranges: bytes`).

## Install (optional)

```bash
sudo ./install.sh           # /usr/local/bin, man, desktop, icons
sudo ./uninstall.sh         # remove those files
./install.sh --user         # ~/.local instead
```

`uninstall.sh --purge` also deletes `~/.config/proxchunk` and `~/.cache/proxchunk`.

## Options

| Option | Meaning |
|--------|---------|
| `-o`, `--output FILE` | Output path (default: URL basename) |
| `-c`, `--concurrent N` | Max parallel chunks (default: logical CPU count) |
| `-s`, `--chunk-mb MB` | Chunk size in MiB (default: 8) |
| `-p`, `--proxies N` | Live proxies to keep (default: 40) |
| `--limit-mb MB` | Download only the first MB |
| `--direct` | Single IP, no proxies |
| `--show-proxies` | 15-character IPv4 field on each chunk bar |
| `--socks URL` | Extra HTTP or SOCKS proxy (repeatable) |
| `--proxy-file FILE` | Extra `ip:port` or `scheme://host:port` list |
| `--no-tor` | Do not auto-add `socks5h://127.0.0.1:9050` |
| `--no-cache` | Do not use `~/.cache/proxchunk/proxies.txt` |
| `--no-user-proxies` | Skip `~/.config/proxchunk/proxies.txt` |
| `-h`, `--help` | Help |
| `-v`, `--version` | Version |

User proxies (optional): `~/.config/proxchunk/proxies.txt`, one `ip:port` per line.

## License

MIT
