/*
 * proxchunk - Fully automatic multi-proxy Range chunked downloader
 * C++23, Linux CLI
 *
 * Fetches free HTTP proxies, scores them, splits the target into Range
 * chunks, and downloads each chunk through a different IP.
 */

#include "proxchunk/curl_util.hpp"
#include "proxchunk/ia_size.hpp"
#include "proxchunk/plan.hpp"
#include "proxchunk/proxy_ipc.hpp"
#include "proxchunk/repl.hpp"

#include <libsf/tui/progress_bar.h>
#include <libsf/tui/detail/terminal.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#ifndef PROXCHUNK_VERSION
#define PROXCHUNK_VERSION "1.1"
#endif

namespace fs = std::filesystem;

using proxchunk::apply_curl_proxy;
using proxchunk::apply_fast_tcp;
using proxchunk::is_socks_proxy;
using proxchunk::k_user_agent;
using proxchunk::write_null;
using proxchunk::write_to_file;
using proxchunk::write_to_string;

/**
 * Pin a block of progress-bar rows. Raw mode, CUP only — never emit '\\n'
 * (tui::progress_bar() prints a newline at 100% and would scroll VTE).
 */
struct BarLayout
{
    int origin_row = 1;
    int n_lines = 0;
    bool active = false;
    termios cooked{};
    bool have_cooked = false;

    ~BarLayout()
    {
        finish();
    }

    void begin(int lines)
    {
        tui::detail::set_stdout_unbuffered(true);
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &cooked) == 0)
        {
            have_cooked = true;
            termios raw = cooked;
            cfmakeraw(&raw);
            raw.c_lflag |= ISIG; /* keep Ctrl+C */
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        tui::detail::hide_cursor();
        std::cout << tui::line_wrap_off;

        int term_rows = 24;
        if (auto sz = tui::detail::query_terminal_size())
        {
            if (sz->rows > 0)
            {
                term_rows = sz->rows;
            }
        }

        n_lines = lines;
        /* Pin to the bottom so a second download overwrites the same slot
         * instead of stacking under leftover bars. */
        int row = term_rows - lines + 1;
        if (row < 1)
        {
            row = 1;
        }
        origin_row = row;
        for (int i = 0; i < n_lines; ++i)
        {
            std::cout << "\033[" << (origin_row + i) << ";1H\033[K";
        }
        std::cout.flush();
        active = true;
    }

    void go_line(int i) const
    {
        std::cout << "\033[" << (origin_row + i) << ";1H\033[K";
    }

    void finish()
    {
        if (!active)
        {
            return;
        }
        /* Stay on the last bar row; cooked + newline in the caller for logs. */
        std::cout << "\033[" << (origin_row + n_lines - 1) << ";1H" << tui::line_wrap;
        tui::detail::show_cursor();
        std::cout.flush();
        if (have_cooked)
        {
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &cooked);
            have_cooked = false;
        }
        active = false;
    }
};

// ---------------------------------------------------------------------------
// Range probe + download
// ---------------------------------------------------------------------------

struct FileInfo
{
    std::int64_t size = -1;
    bool         accepts_ranges = false;
};

static void
probe_curl_common(CURL* c, const std::string& url, const std::string& proxy)
{
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, k_user_agent);
    apply_curl_proxy(c, proxy, url.starts_with("https://"));
    apply_fast_tcp(c);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
}

[[nodiscard]] static std::expected<FileInfo, std::string>
probe_file(const std::string& url, const std::string& proxy)
{
    CURL* c = curl_easy_init();
    if (!c)
    {
        return std::unexpected("curl_easy_init failed");
    }

    FileInfo info;
    std::string headers;
    probe_curl_common(c, url, proxy);
    /* 0-0 is rejected/hung by some CDNs; a tiny GET still yields Content-Range. */
    curl_easy_setopt(c, CURLOPT_RANGE, "0-8191");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_to_string);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);

    CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK)
    {
        return std::unexpected(std::string("Range probe failed: ") + curl_easy_strerror(res));
    }

    if (code == 206)
    {
        info.accepts_ranges = true;
        auto pos = headers.find("Content-Range:");
        if (pos == std::string::npos)
        {
            pos = headers.find("content-range:");
        }
        if (pos != std::string::npos)
        {
            auto slash = headers.find('/', pos);
            if (slash != std::string::npos)
            {
                std::string total = headers.substr(slash + 1);
                total = total.substr(0, total.find_first_of("\r\n "));
                try
                {
                    info.size = std::stoll(total);
                }
                catch (...)
                {
                }
            }
        }
    }
    else if (code == 200)
    {
        info.accepts_ranges = false;
        if (info.size < 0)
        {
            auto pos = headers.find("Content-Length:");
            if (pos == std::string::npos)
            {
                pos = headers.find("content-length:");
            }
            if (pos != std::string::npos)
            {
                auto start = headers.find_first_of("0123456789", pos);
                if (start != std::string::npos)
                {
                    try
                    {
                        info.size = std::stoll(headers.substr(start));
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
    }
    else
    {
        return std::unexpected("Unexpected HTTP " + std::to_string(code) + " on Range probe");
    }

    if (info.size <= 0)
    {
        return std::unexpected("Could not determine file size");
    }

    return info;
}

/**
 * One frame via the tui API: label | bar with % inside | now/total bytes.
 * progress must be allowed to equal total (inclusive) or the bar never hits 100%.
 * Do not call progress_bar()/progress_bar_done() — those emit a newline.
 */
static void
draw_byte_bar(const char* msg, std::int64_t now, std::int64_t want,
              const tui::progress_bar_style& style)
{
    const long long total = want > 0 ? static_cast<long long>(want) : 1;
    long long progress = now < 0 ? 0 : static_cast<long long>(now);
    if (progress > total)
    {
        progress = total;
    }
    auto st = tui::progress_bar_init(msg, total, 36, style);
    tui::progress_bar_update(st, progress);
}

/** Width of "255.255.255.255" — IPv4 field is always this wide so bars do not jump. */
static constexpr int kIpv4FieldWidth = 15;

/**
 * @brief Write a 15-char IPv4 field (space-padded) into @p out (16 bytes with NUL).
 *
 * Strips scheme and port from a proxy URL (`http://1.2.3.4:8080` → `1.2.3.4`).
 */
static void
format_ipv4_field(char out[kIpv4FieldWidth + 1], std::string_view addr)
{
    std::memset(out, ' ', static_cast<std::size_t>(kIpv4FieldWidth));
    out[kIpv4FieldWidth] = '\0';
    if (addr.empty())
    {
        return;
    }
    auto scheme = addr.find("://");
    if (scheme != std::string_view::npos)
    {
        addr.remove_prefix(scheme + 3);
    }
    auto slash = addr.find('/');
    if (slash != std::string_view::npos)
    {
        addr = addr.substr(0, slash);
    }
    auto colon = addr.find(':');
    if (colon != std::string_view::npos)
    {
        addr = addr.substr(0, colon);
    }
    const std::size_t n = std::min(addr.size(), static_cast<std::size_t>(kIpv4FieldWidth));
    if (n > 0)
    {
        std::memcpy(out, addr.data(), n);
    }
}

struct SlotProgress
{
    std::atomic<int>           chunk_id{-1};
    std::atomic<std::int64_t>  now{0};
    std::atomic<std::int64_t>  want{0};
    std::atomic<bool>          active{false};
    mutable std::mutex         ip_mu;
    char                       ip[kIpv4FieldWidth + 1]{};
    std::atomic<std::int64_t>  last_byte{0};
    std::atomic<std::int64_t>  last_move_ms{0};
    std::atomic<std::int64_t>  resume_base{0};

    SlotProgress()
    {
        format_ipv4_field(ip, "");
    }

    void set_ip(std::string_view addr)
    {
        std::lock_guard g(ip_mu);
        format_ipv4_field(ip, addr);
    }

    void copy_ip(char out[kIpv4FieldWidth + 1]) const
    {
        std::lock_guard g(ip_mu);
        std::memcpy(out, ip, static_cast<std::size_t>(kIpv4FieldWidth) + 1);
    }
};

static int
chunk_xfer(void* clientp, curl_off_t /*dltotal*/, curl_off_t dlnow, curl_off_t /*ultotal*/,
           curl_off_t /*ulnow*/)
{
    auto* s = static_cast<SlotProgress*>(clientp);
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const std::int64_t cur = s->resume_base.load(std::memory_order_relaxed)
                             + static_cast<std::int64_t>(dlnow < 0 ? 0 : dlnow);
    const std::int64_t want = s->want.load(std::memory_order_relaxed);
    s->now.store(want > 0 && cur > want ? want : cur, std::memory_order_relaxed);
    if (want > 0 && cur >= want)
    {
        return 0; /* complete: never stall-abort a finished Range */
    }
    if (dlnow > s->last_byte.load(std::memory_order_relaxed))
    {
        s->last_byte.store(dlnow, std::memory_order_relaxed);
        s->last_move_ms.store(now_ms, std::memory_order_relaxed);
    }
    else
    {
        const auto t0 = s->last_move_ms.load(std::memory_order_relaxed);
        if (t0 > 0 && now_ms - t0 > 8000)
        {
            return 1; /* abort: stalled > 8 s */
        }
    }
    return 0;
}

static void
setup_pipeline(CURL* c, const std::string& url, const std::string& proxy)
{
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    apply_curl_proxy(c, proxy, url.starts_with("https://"));
    apply_fast_tcp(c);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, k_user_agent);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(c, CURLOPT_FORBID_REUSE, 0L);
    curl_easy_setopt(c, CURLOPT_FRESH_CONNECT, 0L);
}

[[nodiscard]] static bool
download_chunk(CURL* c, const std::string& url, const proxchunk::chunk& ch,
               const std::string& proxy, const fs::path& part_path, SlotProgress* slot)
{
    if (c == nullptr)
    {
        return false;
    }

    std::int64_t have = 0;
    {
        std::error_code ec;
        if (fs::exists(part_path, ec) && !ec)
        {
            const auto sz = fs::file_size(part_path, ec);
            if (!ec && sz > 0)
            {
                have = static_cast<std::int64_t>(sz);
            }
        }
    }
    const std::int64_t want = ch.end - ch.start + 1;
    if (have >= want)
    {
        if (slot != nullptr)
        {
            slot->now.store(want, std::memory_order_relaxed);
        }
        return true;
    }

    FILE* f = std::fopen(part_path.c_str(), have > 0 ? "ab" : "wb");
    if (!f)
    {
        return false;
    }

    const std::int64_t from = ch.start + have;
    std::string range = std::to_string(from) + "-" + std::to_string(ch.end);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    const long timeout_s = std::max(45L, static_cast<long>(want / (32 * 1024) + 20));
    if (is_socks_proxy(proxy))
    {
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, std::max(timeout_s, 25L));
    }
    else
    {
        curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    }
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 16L * 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 8L);
    if (slot != nullptr)
    {
        slot->resume_base.store(have, std::memory_order_relaxed);
        slot->now.store(have, std::memory_order_relaxed);
        slot->last_byte.store(0);
        slot->last_move_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, chunk_xfer);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, slot);
    }

    CURLcode res = curl_easy_perform(c);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_off_t downloaded = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);

    fclose(f);

    std::int64_t on_disk = 0;
    {
        std::error_code ec;
        if (fs::exists(part_path, ec) && !ec)
        {
            const auto sz = fs::file_size(part_path, ec);
            if (!ec)
            {
                on_disk = static_cast<std::int64_t>(sz);
            }
        }
    }
    if (on_disk >= want)
    {
        if (slot != nullptr)
        {
            slot->now.store(want, std::memory_order_relaxed);
        }
        return true;
    }
    /* Stall/timeout after a 206: keep grown bytes for the next proxy. */
    if ((code == 206 || code == 200) && on_disk > have)
    {
        return false;
    }
    (void)res;
    (void)downloaded;
    std::error_code ec;
    if (have <= 0)
    {
        fs::remove(part_path, ec);
    }
    else
    {
        fs::resize_file(part_path, static_cast<std::uintmax_t>(have), ec);
    }
    return false;
}

struct RunOptions
{
    int  max_concurrent = 0; ///< filled from hardware_concurrency unless -c
    int  chunk_mb       = 0; ///< 0 = N equal pieces; >0 = size-based via -s
    std::int64_t limit_bytes = 0; ///< 0 = full file
    bool direct         = false;
    bool progress       = true;
    bool show_proxies   = false;
};

[[nodiscard]] static std::expected<FileInfo, std::string>
probe_archive_org_metadata(const std::string& url)
{
    const auto parsed = proxchunk::ia_parse_download_url(url);
    if (!parsed)
    {
        return std::unexpected("not an archive.org download URL");
    }
    const std::string meta_url = "https://archive.org/metadata/" + parsed->first;
    CURL* c = curl_easy_init();
    if (!c)
    {
        return std::unexpected("curl_easy_init failed");
    }
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, meta_url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, k_user_agent);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    apply_fast_tcp(c);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    const CURLcode res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (res != CURLE_OK)
    {
        return std::unexpected(std::string("metadata fetch: ") + curl_easy_strerror(res));
    }
    if (code != 200)
    {
        return std::unexpected("metadata HTTP " + std::to_string(code));
    }
    const auto sz = proxchunk::ia_size_from_metadata_json(body, parsed->second);
    if (!sz)
    {
        return std::unexpected("file not listed in archive.org metadata");
    }
    FileInfo info;
    info.size = *sz;
    info.accepts_ranges = true;
    return info;
}

[[nodiscard]] static std::expected<FileInfo, std::string>
probe_size_direct(const std::string& url)
{
    std::cerr << "[proxchunk] Probing file size (direct, no proxy)\n";
    if (auto ia = probe_archive_org_metadata(url); ia)
    {
        std::cerr << "[proxchunk] Size from archive.org metadata: " << ia->size << " bytes\n";
        return ia;
    }
    return probe_file(url, "");
}

[[nodiscard]] static bool
run_download(const std::string& url, const fs::path& output, const RunOptions& opt,
             proxchunk::ProxyClient* client)
{
    auto probe = probe_size_direct(url);
    if (!probe)
    {
        std::cerr << "[proxchunk] Probe failed: " << probe.error() << "\n";
        return false;
    }
    FileInfo info = *probe;
    std::cerr << "[proxchunk] File size: " << info.size << " bytes, Accept-Ranges: "
              << (info.accepts_ranges ? "yes" : "no") << "\n";

    if (!info.accepts_ranges)
    {
        std::cerr << "[proxchunk] Server does not support Range. Aborting.\n";
        return false;
    }

    std::int64_t download_size = info.size;
    if (opt.limit_bytes > 0 && opt.limit_bytes < download_size)
    {
        download_size = opt.limit_bytes;
        std::cerr << "[proxchunk] Limiting download to first " << download_size << " bytes\n";
    }

    std::vector<proxchunk::chunk> chunks;
    if (opt.chunk_mb > 0)
    {
        const std::int64_t chunk_size =
            static_cast<std::int64_t>(opt.chunk_mb) * 1024 * 1024;
        chunks = proxchunk::plan_chunks(download_size, chunk_size);
        std::cerr << "[proxchunk] Split into " << chunks.size() << " chunks of ~"
                  << opt.chunk_mb << " MB\n";
    }
    else
    {
        chunks = proxchunk::plan_chunks_n(download_size, opt.max_concurrent);
        std::cerr << "[proxchunk] Split into " << chunks.size() << " equal pieces\n";
    }

    fs::path tmpdir = output.parent_path();
    if (tmpdir.empty())
    {
        tmpdir = ".";
    }
    tmpdir /= (".proxchunk_parts_" + std::to_string(std::random_device{}()));
    fs::create_directories(tmpdir);

    std::atomic<std::int64_t> bytes_done{0};
    std::atomic<int>          finished{0};
    std::atomic<int>          failed{0};
    std::mutex work_mtx;
    struct Job
    {
        proxchunk::chunk         ch;
        int                      attempts = 0;
        std::vector<std::string> tried;
    };
    std::deque<Job> jobs;
    for (const auto& c : chunks)
    {
        jobs.push_back(Job{c, 0, {}});
    }
    std::atomic<int> inflight{0};

    int n_workers = std::min(opt.max_concurrent, static_cast<int>(chunks.size()));
    n_workers = std::max(1, n_workers);
    /* One TUI bar per live connection, not one per planned slice. */
    std::vector<SlotProgress> slots(static_cast<std::size_t>(n_workers));
    const bool use_bar = opt.progress && isatty(STDOUT_FILENO);

    auto worker_fn = [&](int worker_id) {
        auto& sp = slots[static_cast<std::size_t>(worker_id)];
        CURL* easy = nullptr;
        std::optional<proxchunk::acquired_proxy> pipe;
        double last_mbps = 0.0;
        auto drop_pipe = [&](bool ok) {
            if (pipe && client != nullptr)
            {
                (void)client->release(pipe->url, ok, last_mbps);
            }
            pipe.reset();
            if (easy != nullptr)
            {
                curl_easy_cleanup(easy);
                easy = nullptr;
            }
        };
        auto open_pipe = [&](const std::vector<std::string>& skip) -> bool {
            if (easy != nullptr && (opt.direct || pipe))
            {
                return true;
            }
            drop_pipe(false);
            if (opt.direct)
            {
                easy = curl_easy_init();
                if (easy == nullptr)
                {
                    return false;
                }
                setup_pipeline(easy, url, "");
                sp.set_ip("direct");
                return true;
            }
            if (client == nullptr)
            {
                return false;
            }
            auto p = client->acquire(skip);
            if (!p)
            {
                return false;
            }
            easy = curl_easy_init();
            if (easy == nullptr)
            {
                (void)client->release(p->url, false, 0.0);
                return false;
            }
            setup_pipeline(easy, url, p->url);
            sp.set_ip(p->url);
            if (!use_bar)
            {
                std::cerr << "[proxchunk] Pipeline via " << p->url << "\n";
            }
            pipe = std::move(p);
            return true;
        };

        while (true)
        {
            Job job;
            {
                std::unique_lock lock(work_mtx);
                if (jobs.empty())
                {
                    if (inflight.load() == 0)
                    {
                        drop_pipe(true);
                        return;
                    }
                    lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                job = jobs.front();
                jobs.pop_front();
                inflight.fetch_add(1);
            }

            const proxchunk::chunk ch = job.ch;
            const std::int64_t want = ch.end - ch.start + 1;
            sp.chunk_id.store(ch.id, std::memory_order_relaxed);
            sp.want.store(want, std::memory_order_relaxed);
            sp.now.store(0, std::memory_order_relaxed);
            sp.active.store(true, std::memory_order_relaxed);

            if (!open_pipe(job.tried))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                {
                    std::lock_guard g(work_mtx);
                    jobs.push_front(job);
                }
                inflight.fetch_sub(1);
                continue;
            }
            if (pipe)
            {
                job.tried.push_back(pipe->url);
            }

            const fs::path part = tmpdir / ("part." + std::to_string(ch.id));
            const auto t0 = std::chrono::steady_clock::now();
            const std::string proxy_addr = pipe ? pipe->url : std::string{};
            const bool ok = download_chunk(easy, url, ch, proxy_addr, part, &sp);
            const auto secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ok && secs > 0.01)
            {
                last_mbps = (static_cast<double>(want) / (1024.0 * 1024.0)) / secs;
            }

            if (ok)
            {
                sp.now.store(want, std::memory_order_relaxed);
                sp.active.store(false, std::memory_order_relaxed);
                bytes_done.fetch_add(want);
                finished.fetch_add(1);
            }
            else
            {
                drop_pipe(false);
                sp.active.store(false, std::memory_order_relaxed);
                job.attempts++;
                constexpr int k_chunk_tries = 3;
                constexpr int k_max_proxies = 8;
                if (job.attempts >= k_chunk_tries)
                {
                    /* Give the chunk to another live pipeline; 3 tries are per pipe. */
                    job.attempts = 0;
                }
                if (static_cast<int>(job.tried.size()) >= k_max_proxies)
                {
                    failed.fetch_add(1);
                    if (!use_bar)
                    {
                        std::cerr << "[proxchunk] Failed chunk " << ch.id << " after "
                                  << job.tried.size() << " proxies\n";
                    }
                }
                else
                {
                    if (!use_bar)
                    {
                        std::cerr << "[proxchunk] Requeue chunk " << ch.id << " for another pipeline\n";
                    }
                    std::lock_guard g(work_mtx);
                    jobs.push_back(job);
                }
            }
            inflight.fetch_sub(1);
        }
    };

    tui::progress_bar_style chunk_style = tui::progress_bar_styles::blocks_smooth();
    chunk_style.use_gradient = true;
    chunk_style.gradient_start = tui::make_rgb(30, 180, 90);
    chunk_style.gradient_end = tui::make_rgb(40, 200, 255);
    chunk_style.percent_inside = true;

    tui::progress_bar_style total_style = chunk_style;
    total_style.gradient_start = tui::make_rgb(220, 160, 40);
    total_style.gradient_end = tui::make_rgb(40, 200, 120);

    const auto t_start = std::chrono::steady_clock::now();
    const int need = static_cast<int>(chunks.size());
    const int n_bars = n_workers;
    BarLayout chunk_bars;
    if (use_bar)
    {
        chunk_bars.begin(n_bars + 1);
    }

    std::vector<std::jthread> workers;
    for (int i = 0; i < n_workers; ++i)
    {
        workers.emplace_back([&, i]() { worker_fn(i); });
    }

    auto live_bytes = [&]() -> std::int64_t {
        std::int64_t n = bytes_done.load(std::memory_order_relaxed);
        for (const auto& s : slots)
        {
            if (s.active.load(std::memory_order_relaxed))
            {
                n += s.now.load(std::memory_order_relaxed);
            }
        }
        return n;
    };

    auto draw_bars = [&]() {
        const std::int64_t live = live_bytes();
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        const double speed = elapsed > 0.1 ? (static_cast<double>(live) / (1024.0 * 1024.0)) / elapsed
                                           : 0.0;
        if (use_bar)
        {
            for (int i = 0; i < n_bars; ++i)
            {
                const auto& sl = slots[static_cast<std::size_t>(i)];
                const std::int64_t now = sl.now.load(std::memory_order_relaxed);
                const std::int64_t want = sl.want.load(std::memory_order_relaxed);
                const int cid = sl.chunk_id.load(std::memory_order_relaxed);
                char msg[64];
                if (opt.show_proxies)
                {
                    char ip[kIpv4FieldWidth + 1];
                    sl.copy_ip(ip);
                    if (cid >= 0)
                    {
                        std::snprintf(msg, sizeof(msg), "%s chunk %d", ip, cid);
                    }
                    else
                    {
                        std::snprintf(msg, sizeof(msg), "%s idle", ip);
                    }
                }
                else if (cid >= 0)
                {
                    std::snprintf(msg, sizeof(msg), "chunk %d", cid);
                }
                else
                {
                    std::snprintf(msg, sizeof(msg), "idle");
                }
                chunk_bars.go_line(i);
                draw_byte_bar(msg, now, want, chunk_style);
            }
            char tmsg[80];
            std::snprintf(tmsg, sizeof(tmsg), "total  %.2f MB/s  %d/%zu", speed, finished.load(),
                          chunks.size());
            chunk_bars.go_line(n_bars);
            draw_byte_bar(tmsg, live, download_size, total_style);
            std::cout.flush();
        }
        else
        {
            const double pct = 100.0 * static_cast<double>(live) / static_cast<double>(download_size);
            std::fprintf(stderr, "\r[proxchunk] %.1f%%  %.2f MB/s  %d/%zu chunks   ", pct, speed,
                         finished.load(), chunks.size());
            std::fflush(stderr);
        }
    };

    while (finished.load() + failed.load() < need)
    {
        draw_bars();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    workers.clear();
    draw_bars();

    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    const double speed = elapsed > 0.1
                             ? (static_cast<double>(bytes_done.load()) / (1024.0 * 1024.0)) / elapsed
                             : 0.0;

    if (use_bar)
    {
        chunk_bars.finish();
        std::cerr << '\n';
    }
    else
    {
        std::fprintf(stderr, "\n");
    }

    std::cerr << "[proxchunk] Aggregate: " << speed << " MB/s over " << elapsed << " s  ("
              << bytes_done.load() << " bytes)\n";

    if (failed.load() > 0)
    {
        std::cerr << "[proxchunk] " << failed.load() << " chunk(s) failed\n";
        fs::remove_all(tmpdir);
        return false;
    }

    std::cerr << "[proxchunk] Assembling final file...\n";
    std::ofstream out(output, std::ios::binary);
    if (!out)
    {
        std::cerr << "Cannot open output file\n";
        fs::remove_all(tmpdir);
        return false;
    }
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        fs::path part = tmpdir / ("part." + std::to_string(static_cast<int>(i)));
        if (!fs::exists(part))
        {
            std::cerr << "Missing part " << i << "\n";
            out.close();
            fs::remove(output);
            fs::remove_all(tmpdir);
            return false;
        }
        std::ifstream in(part, std::ios::binary);
        out << in.rdbuf();
    }
    out.close();
    fs::remove_all(tmpdir);

    if (fs::file_size(output) != static_cast<std::uintmax_t>(download_size))
    {
        std::cerr << "[proxchunk] Size mismatch after assemble!\n";
        return false;
    }

    std::cerr << "[proxchunk] Done. Saved to " << output << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void
usage(const char* prog)
{
    std::cerr
        << "Usage: " << prog << " [options] <URL>\n"
        << "       " << prog << " <URL> [options]\n"
        << "\n"
        << "  -o, --output <file>   Output path (default: basename of URL)\n"
        << "  -c, --concurrent <N>  Equal Range pieces and parallel connections\n"
        << "                        (default: logical CPUs)\n"
        << "  -s, --chunk-mb <MB>   Split by this size in MiB instead of N equal pieces\n"
        << "  -p, --proxies <N>     Max proxies (passed to proxchunkd on auto-start; default: 40)\n"
        << "  -r, --refresh <sec>   Re-test interval for auto-started daemon (default: off)\n"
        << "      --limit-mb <MB>   Download only the first MB (0 = full file)\n"
        << "      --direct          Single-IP download (no daemon, no proxies)\n"
        << "      --socket <path>   proxchunkd UNIX socket (default: runtime dir)\n"
        << "      --no-progress     Do not draw the TUI progress bar\n"
        << "      --no-cache        Do not load/save ~/.cache/proxchunk/proxies.txt\n"
        << "      --show-proxies    Prefix each chunk bar with a 15-char IPv4 field\n"
        << "      --socks <url>     Extra SOCKS/HTTP proxy (repeatable; e.g. socks5h://127.0.0.1:9050)\n"
        << "      --proxy-file <f>  Extra proxy list (ip:port or scheme://host:port per line)\n"
        << "      --no-user-proxies Do not load ~/.config/proxchunk/proxies.txt\n"
        << "      --no-tor          Do not auto-add local Tor on 127.0.0.1:9050\n"
        << "  -h, --help            Show this help\n"
        << "  -v, --version         Print version\n"
        << "      --repl            Interactive prompt (used by proxchunk-gui)\n"
        << "\n"
        << "Downloads N Range pieces through scored proxies from proxchunkd\n"
        << "(auto-started from this binary's directory if needed). --direct skips\n"
        << "the daemon. Failed pieces are retried automatically.\n";
}

static std::string
self_exe()
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
    {
        return {};
    }
    buf[static_cast<std::size_t>(n)] = '\0';
    return std::string(buf);
}

/**
 * @brief Prompt loop: directory builtins, otherwise fork/exec this binary.
 */
static int
run_repl()
{
    std::string self = self_exe();
    if (self.empty())
    {
        std::cerr << "proxchunk: cannot resolve /proc/self/exe\n";
        return 1;
    }
    std::cout << std::unitbuf;
    for (;;)
    {
        std::cout << "> ";
        std::string line;
        if (!std::getline(std::cin, line))
        {
            std::cout << '\n';
            break;
        }
        proxchunk::repl_result r = proxchunk::handle_repl_line(line);
        switch (r.kind)
        {
        case proxchunk::repl_kind::empty:
            break;
        case proxchunk::repl_kind::quit:
            return 0;
        case proxchunk::repl_kind::error:
            std::cerr << r.message << '\n';
            break;
        case proxchunk::repl_kind::info:
            if (!r.message.empty())
            {
                std::cout << r.message << '\n';
            }
            break;
        case proxchunk::repl_kind::run:
        {
            std::vector<char*> av;
            av.push_back(self.data());
            for (auto& s : r.argv)
            {
                av.push_back(s.data());
            }
            av.push_back(nullptr);
            pid_t pid = fork();
            if (pid < 0)
            {
                std::cerr << "fork failed\n";
                break;
            }
            if (pid == 0)
            {
                execv(self.c_str(), av.data());
                _exit(127);
            }
            int st = 0;
            if (waitpid(pid, &st, 0) < 0)
            {
                std::cerr << "waitpid failed\n";
            }
            break;
        }
        }
    }
    return 0;
}

int
main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--repl")
        {
            return run_repl();
        }
    }
    if (argc < 2)
    {
        usage(argv[0]);
        return 0;
    }

    std::string url;
    std::string out_path;
    int concurrent = 0;
    bool concurrent_set = false;
    int chunk_mb   = 0;
    bool chunk_mb_set = false;
    int max_proxies = 40;
    int refresh_sec = 0;
    RunOptions opt;
    bool use_cache = true;
    bool use_tor = true;
    bool use_user_list = true;
    bool max_proxies_set = false;
    bool refresh_set = false;
    std::vector<std::string> extra_proxies;
    std::vector<std::string> proxy_files;
    std::string socket_path;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
            {
                std::cerr << name << " requires an argument\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        if (a == "-v" || a == "--version")
        {
            std::cout << "proxchunk " << PROXCHUNK_VERSION << "\n";
            return 0;
        }
        if (a == "-o" || a == "--output")
        {
            out_path = need("-o");
        }
        else if (a == "-c" || a == "--concurrent")
        {
            concurrent = std::atoi(need("-c"));
            concurrent_set = true;
        }
        else if (a == "-s" || a == "--chunk-mb")
        {
            chunk_mb = std::atoi(need("-s"));
            chunk_mb_set = true;
        }
        else if (a == "-p" || a == "--proxies")
        {
            max_proxies = std::atoi(need("-p"));
            max_proxies_set = true;
        }
        else if (a == "-r" || a == "--refresh")
        {
            refresh_sec = std::atoi(need("-r"));
            refresh_set = true;
        }
        else if (a == "--socket")
        {
            socket_path = need("--socket");
        }
        else if (a == "--limit-mb")
        {
            opt.limit_bytes = static_cast<std::int64_t>(std::atoi(need("--limit-mb"))) * 1024 * 1024;
        }
        else if (a == "--direct")
        {
            opt.direct = true;
        }
        else if (a == "--no-progress")
        {
            opt.progress = false;
        }
        else if (a == "--no-cache")
        {
            use_cache = false;
        }
        else if (a == "--show-proxies")
        {
            opt.show_proxies = true;
        }
        else if (a == "--no-tor")
        {
            use_tor = false;
        }
        else if (a == "--socks")
        {
            std::string u = proxchunk::normalize_proxy_line(need("--socks"));
            if (!u.empty())
            {
                extra_proxies.push_back(std::move(u));
            }
        }
        else if (a == "--proxy-file")
        {
            proxy_files.emplace_back(need("--proxy-file"));
        }
        else if (a == "--no-user-proxies")
        {
            use_user_list = false;
        }
        else if (a[0] != '-')
        {
            url = a;
        }
        else
        {
            std::cerr << "Unknown option " << a << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (!concurrent_set)
    {
        unsigned ncpu = std::thread::hardware_concurrency();
        concurrent = (ncpu == 0) ? 4 : static_cast<int>(ncpu);
    }

    opt.max_concurrent = concurrent;
    opt.chunk_mb = chunk_mb_set ? chunk_mb : 0;

    if (url.empty())
    {
        usage(argv[0]);
        return 0;
    }
    if (concurrent < 1 || max_proxies < 1)
    {
        std::cerr << "concurrent and proxies must be >= 1\n";
        return 1;
    }
    if (chunk_mb_set && chunk_mb < 1)
    {
        std::cerr << "chunk-mb must be >= 1\n";
        return 1;
    }
    if (refresh_sec < 0)
    {
        std::cerr << "refresh must be >= 0 (0 = no background refresh)\n";
        return 1;
    }

    if (out_path.empty())
    {
        out_path = proxchunk::default_output_name(url);
    }

    std::cerr << "[proxchunk] Starting. Target: " << url << "\n";
    std::cerr << "[proxchunk] Output: " << out_path << "  concurrent=" << concurrent;
    if (chunk_mb_set)
    {
        std::cerr << "  chunk=" << chunk_mb << "MB";
    }
    else
    {
        std::cerr << "  pieces=" << concurrent;
    }
    std::cerr << (opt.direct ? "  direct" : "") << "\n";

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
    {
        std::cerr << "[proxchunk] curl_global_init failed\n";
        return 1;
    }

    if (opt.direct)
    {
        bool ok = run_download(url, out_path, opt, nullptr);
        curl_global_cleanup();
        return ok ? 0 : 1;
    }

    const fs::path sock = socket_path.empty() ? proxchunk::default_socket_path()
                                              : fs::path(socket_path);
    std::vector<std::string> daemon_extra;
    if (max_proxies_set)
    {
        daemon_extra.push_back("-p");
        daemon_extra.push_back(std::to_string(max_proxies));
    }
    if (refresh_set)
    {
        daemon_extra.push_back("-r");
        daemon_extra.push_back(std::to_string(refresh_sec));
    }
    if (!use_cache)
    {
        daemon_extra.push_back("--no-cache");
    }
    if (!use_tor)
    {
        daemon_extra.push_back("--no-tor");
    }
    if (!use_user_list)
    {
        daemon_extra.push_back("--no-user-proxies");
    }
    for (const auto& u : extra_proxies)
    {
        daemon_extra.push_back("--socks");
        daemon_extra.push_back(u);
    }
    for (const auto& pf : proxy_files)
    {
        daemon_extra.push_back("--proxy-file");
        std::error_code ec;
        fs::path abs = fs::absolute(pf, ec);
        daemon_extra.push_back(ec ? pf : abs.string());
    }

    const bool pool_flags = max_proxies_set || refresh_set || !use_cache || !use_tor
                            || !use_user_list || !extra_proxies.empty() || !proxy_files.empty();

    proxchunk::ProxyClient client;
    bool already = false;
    {
        proxchunk::ProxyClient probe;
        already = probe.connect(sock) && probe.hello() && probe.ping();
    }
    if (already)
    {
        if (pool_flags)
        {
            std::cerr << "[proxchunk] daemon already running; proxy pool flags ignored\n";
        }
        if (!client.connect(sock) || !client.hello())
        {
            std::cerr << "proxchunkd is not running and could not be started (socket " << sock
                      << ")\n";
            curl_global_cleanup();
            return 1;
        }
    }
    else if (!client.connect_or_start(sock, daemon_extra))
    {
        std::cerr << "proxchunkd is not running and could not be started (socket " << sock
                  << ")\n";
        curl_global_cleanup();
        return 1;
    }

    bool ok = run_download(url, out_path, opt, &client);
    curl_global_cleanup();
    return ok ? 0 : 1;
}
