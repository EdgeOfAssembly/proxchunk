/*
 * proxchunk - Fully automatic multi-proxy Range chunked downloader
 * C++23, Linux CLI
 *
 * Features:
 *  - Fetches free HTTP proxy lists from multiple public sources
 *  - Concurrently tests and scores them by download speed
 *  - Drops dead / too-slow proxies
 *  - Keeps a sorted live pool (background refresher)
 *  - Probes target for Content-Length + Accept-Ranges
 *  - Splits file into chunks, downloads each chunk via a different proxy (own IP)
 *  - Writes parts then concatenates in correct order
 *  - Progress reporting
 *
 * Build:
 *   g++ -std=c++23 -O2 -Wall -Wextra -o proxchunk proxchunk.cpp -lcurl -pthread
 *
 * Usage:
 *   ./proxchunk <URL> [-o outfile] [-c concurrent] [-s chunk_mb] [-p max_proxies]
 *
 * License: MIT
 */

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static size_t
write_null(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/)
{
    return size * nmemb;
}

static size_t
write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t
write_to_file(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* f = static_cast<FILE*>(userdata);
    return fwrite(ptr, size, nmemb, f);
}

[[nodiscard]] static std::string
trim(std::string_view sv)
{
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n'))
        sv.remove_suffix(1);
    return std::string(sv);
}

// ---------------------------------------------------------------------------
// Proxy
// ---------------------------------------------------------------------------

struct Proxy
{
    std::string address;          // e.g. "http://1.2.3.4:8080"
    double      speed_mbps = 0.0;
    int         latency_ms = 99999;
    int         fails      = 0;
    bool        alive      = true;

    bool operator>(const Proxy& o) const noexcept
    {
        return speed_mbps > o.speed_mbps;
    }
};

// ---------------------------------------------------------------------------
// ProxyPool
// ---------------------------------------------------------------------------

class ProxyPool
{
public:
    explicit ProxyPool(std::size_t max_keep = 40, int refresh_sec = 180)
        : max_keep_(max_keep)
        , refresh_sec_(refresh_sec)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~ProxyPool()
    {
        stop();
        curl_global_cleanup();
    }

    void start()
    {
        if (running_.exchange(true))
            return;
        refresh(); // initial
        updater_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested())
            {
                for (int i = 0; i < refresh_sec_ && !st.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!st.stop_requested())
                    refresh();
            }
        });
    }

    void stop()
    {
        if (!running_.exchange(false))
            return;
        if (updater_.joinable())
            updater_.request_stop();
    }

    // Acquire a good proxy (thread-safe). Returns empty if none available.
    [[nodiscard]] std::optional<Proxy>
    acquire()
    {
        std::unique_lock lock(mutex_);
        for (auto& p : pool_)
        {
            if (p.alive && p.fails < 3)
            {
                Proxy copy = p;
                p.fails++; // temporary mark as in-use-ish
                return copy;
            }
        }
        return std::nullopt;
    }

    void release(const Proxy& used, bool success, double measured = 0.0)
    {
        std::unique_lock lock(mutex_);
        for (auto& p : pool_)
        {
            if (p.address == used.address)
            {
                if (success)
                {
                    p.fails = 0;
                    if (measured > 0.0)
                        p.speed_mbps = (p.speed_mbps * 0.7) + (measured * 0.3); // EMA
                    p.alive = true;
                }
                else
                {
                    p.fails++;
                    if (p.fails >= 4)
                        p.alive = false;
                }
                break;
            }
        }
        // keep sorted
        std::sort(pool_.begin(), pool_.end(), std::greater<>{});
    }

    [[nodiscard]] std::size_t
    size() const
    {
        std::shared_lock lock(mutex_);
        std::size_t n = 0;
        for (const auto& p : pool_)
            if (p.alive)
                ++n;
        return n;
    }

    void force_refresh()
    {
        refresh();
    }

private:
    void refresh()
    {
        std::vector<std::string> candidates = fetch_all_lists();
        if (candidates.empty())
        {
            std::cerr << "[proxychunk] No proxies fetched from sources\n";
            return;
        }

        // Dedup
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        // Concurrent test (limited parallelism)
        constexpr int kTestThreads = 24;
        std::vector<Proxy> tested;
        tested.reserve(candidates.size());
        std::mutex test_mtx;
        std::atomic<std::size_t> idx{0};

        auto worker = [&]() {
            while (true)
            {
                std::size_t i = idx.fetch_add(1);
                if (i >= candidates.size())
                    break;
                Proxy p;
                p.address = candidates[i];
                if (test_proxy(p))
                {
                    std::lock_guard g(test_mtx);
                    tested.push_back(std::move(p));
                }
            }
        };

        std::vector<std::jthread> threads;
        for (int t = 0; t < kTestThreads; ++t)
            threads.emplace_back(worker);
        threads.clear(); // join

        // Keep top N by speed
        std::sort(tested.begin(), tested.end(), std::greater<>{});
        if (tested.size() > max_keep_)
            tested.resize(max_keep_);

        {
            std::unique_lock lock(mutex_);
            pool_ = std::move(tested);
        }

        std::cerr << "[proxychunk] Proxy pool refreshed: " << size() << " good proxies (top speed "
                  << (pool_.empty() ? 0.0 : pool_.front().speed_mbps) << " MB/s)\n";
    }

    [[nodiscard]] std::vector<std::string>
    fetch_all_lists()
    {
        static const std::vector<std::string> sources = {
            "https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/http.txt",
            "https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/http.txt",
            "https://raw.githubusercontent.com/ProxyScraper/ProxyScraper/main/http.txt",
            "https://raw.githubusercontent.com/clarketm/proxy-list/master/proxy-list-raw.txt",
            "https://raw.githubusercontent.com/ShiftyTR/Proxy-List/master/http.txt",
            "https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/txt/proxies-http.txt",
            "https://api.proxyscrape.com/v2/?request=displayproxies&protocol=http&timeout=8000&country=all&ssl=all&anonymity=all",
            "https://raw.githubusercontent.com/proxifly/free-proxy-list/main/proxies/protocols/http/data.txt"
        };

        std::vector<std::string> all;
        for (const auto& url : sources)
        {
            std::string body;
            CURL* c = curl_easy_init();
            if (!c)
                continue;
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_string);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_USERAGENT, "proxchunk/1.0");
            CURLcode res = curl_easy_perform(c);
            curl_easy_cleanup(c);
            if (res != CURLE_OK)
                continue;

            std::istringstream iss(body);
            std::string line;
            while (std::getline(iss, line))
            {
                line = trim(line);
                if (line.empty() || line[0] == '#' || line[0] == '/')
                    continue;
                // Accept ip:port or already has scheme
                if (line.find("://") == std::string::npos)
                {
                    // crude validation
                    if (line.find(':') == std::string::npos)
                        continue;
                    line = "http://" + line;
                }
                all.push_back(std::move(line));
            }
        }
        return all;
    }

    [[nodiscard]] bool
    test_proxy(Proxy& p)
    {
        // Small test download through the proxy to measure real speed
        const char* test_url = "http://speedtest.tele2.net/100KB.zip"; // ~100 KiB, HTTP, reliable

        CURL* c = curl_easy_init();
        if (!c)
            return false;

        curl_easy_setopt(c, CURLOPT_URL, test_url);
        curl_easy_setopt(c, CURLOPT_PROXY, p.address.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "proxchunk/1.0");

        auto t0 = std::chrono::steady_clock::now();
        CURLcode res = curl_easy_perform(c);
        auto t1 = std::chrono::steady_clock::now();

        double total = 0.0;
        curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &total);
        long http_code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(c);

        if (res != CURLE_OK || http_code >= 400 || total < 1024.0)
            return false;

        double secs = std::chrono::duration<double>(t1 - t0).count();
        if (secs < 0.01)
            secs = 0.01;
        p.speed_mbps = (total / (1024.0 * 1024.0)) / secs;
        p.latency_ms = static_cast<int>(secs * 1000.0);
        p.alive = true;
        p.fails = 0;
        return p.speed_mbps > 0.02; // keep only > ~20 KB/s
    }

    std::size_t                 max_keep_;
    int                         refresh_sec_;
    std::vector<Proxy>          pool_;
    mutable std::shared_mutex   mutex_;
    std::jthread                updater_;
    std::atomic<bool>           running_{false};
};

// ---------------------------------------------------------------------------
// Range probe + download
// ---------------------------------------------------------------------------

struct FileInfo
{
    std::int64_t size = -1;
    bool         accepts_ranges = false;
};

[[nodiscard]] static std::expected<FileInfo, std::string>
probe_file(const std::string& url)
{
    CURL* c = curl_easy_init();
    if (!c)
        return std::unexpected("curl_easy_init failed");

    // Prefer HEAD, fall back to Range 0-0
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_HEADER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "proxchunk/1.0");

    CURLcode res = curl_easy_perform(c);
    FileInfo info;

    if (res == CURLE_OK)
    {
        double cl = -1;
        curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0)
            info.size = static_cast<std::int64_t>(cl);
    }
    curl_easy_cleanup(c);

    // Always verify with a real Range request
    c = curl_easy_init();
    if (!c)
        return std::unexpected("curl_easy_init failed");

    std::string headers;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_to_string);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "proxchunk/1.0");

    res = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK)
        return std::unexpected(std::string("Range probe failed: ") + curl_easy_strerror(res));

    if (code == 206)
    {
        info.accepts_ranges = true;
        // Parse Content-Range: bytes 0-0/TOTAL
        auto pos = headers.find("Content-Range:");
        if (pos == std::string::npos)
            pos = headers.find("content-range:");
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
                pos = headers.find("content-length:");
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
        return std::unexpected("Could not determine file size");

    return info;
}

struct Chunk
{
    std::int64_t start = 0;
    std::int64_t end   = 0; // inclusive
    int          id    = 0;
    bool         done  = false;
};

[[nodiscard]] static bool
download_chunk(const std::string& url, const Chunk& ch, const Proxy& proxy,
               const fs::path& part_path, std::atomic<std::int64_t>& bytes_done)
{
    CURL* c = curl_easy_init();
    if (!c)
        return false;

    FILE* f = fopen(part_path.c_str(), "wb");
    if (!f)
    {
        curl_easy_cleanup(c);
        return false;
    }

    std::string range = std::to_string(ch.start) + "-" + std::to_string(ch.end);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_PROXY, proxy.address.c_str());
    curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "proxchunk/1.0");

    CURLcode res = curl_easy_perform(c);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    double downloaded = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);

    fclose(f);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || (code != 206 && code != 200) || downloaded < (ch.end - ch.start + 1) * 0.95)
    {
        fs::remove(part_path);
        return false;
    }

    bytes_done.fetch_add(static_cast<std::int64_t>(downloaded), std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// Main download orchestration
// ---------------------------------------------------------------------------

[[nodiscard]] static bool
run_download(const std::string& url, const fs::path& output,
             int max_concurrent, int chunk_mb, ProxyPool& pool)
{
    auto probe = probe_file(url);
    if (!probe)
    {
        std::cerr << "[proxychunk] Probe failed: " << probe.error() << "\n";
        return false;
    }
    const FileInfo& info = *probe;
    std::cerr << "[proxychunk] File size: " << info.size << " bytes, Accept-Ranges: "
              << (info.accepts_ranges ? "yes" : "no") << "\n";

    if (!info.accepts_ranges)
    {
        std::cerr << "[proxychunk] Server does not support Range. Falling back to single connection (still through a proxy).\n";
        std::cerr << "This MVP requires Range support.\n";
        return false;
    }

    const std::int64_t chunk_size = static_cast<std::int64_t>(chunk_mb) * 1024 * 1024;
    std::vector<Chunk> chunks;
    int id = 0;
    for (std::int64_t off = 0; off < info.size; off += chunk_size)
    {
        Chunk ch;
        ch.start = off;
        ch.end   = std::min(off + chunk_size - 1, info.size - 1);
        ch.id    = id++;
        chunks.push_back(ch);
    }

    std::cerr << "[proxychunk] Split into " << chunks.size() << " chunks of ~" << chunk_mb << " MB\n";

    // Create temp dir for parts
    fs::path tmpdir = output.parent_path() / (".proxchunk_parts_" + std::to_string(std::random_device{}()));
    fs::create_directories(tmpdir);

    std::atomic<std::int64_t> bytes_done{0};
    std::atomic<int>          finished{0};
    std::mutex                work_mtx;
    std::size_t               next_chunk = 0;

    auto worker_fn = [&]() {
        while (true)
        {
            Chunk ch;
            {
                std::lock_guard g(work_mtx);
                if (next_chunk >= chunks.size())
                    return;
                ch = chunks[next_chunk++];
            }

            bool ok = false;
            for (int attempt = 0; attempt < 6 && !ok; ++attempt)
            {
                auto px = pool.acquire();
                if (!px)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                fs::path part = tmpdir / ("part." + std::to_string(ch.id));
                ok = download_chunk(url, ch, *px, part, bytes_done);
                pool.release(*px, ok, 0.0);
                if (!ok)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            if (ok)
            {
                finished.fetch_add(1);
            }
            else
            {
                std::cerr << "[proxychunk] Failed chunk " << ch.id << " after retries\n";
            }
        }
    };

    // Launch workers
    int n_workers = std::min(max_concurrent, static_cast<int>(chunks.size()));
    n_workers = std::max(1, n_workers);
    std::vector<std::jthread> workers;
    for (int i = 0; i < n_workers; ++i)
        workers.emplace_back(worker_fn);

    // Progress loop
    const auto t_start = std::chrono::steady_clock::now();
    while (finished.load() < static_cast<int>(chunks.size()))
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        double done = static_cast<double>(bytes_done.load());
        double pct  = 100.0 * done / static_cast<double>(info.size);
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        double speed = elapsed > 0.1 ? (done / (1024.0 * 1024.0)) / elapsed : 0.0;
        std::fprintf(stderr, "\r[proxychunk] %.1f%%  %.2f MB/s  %d/%zu chunks   ",
                     pct, speed, finished.load(), chunks.size());
        std::fflush(stderr);
    }
    workers.clear();
    std::fprintf(stderr, "\n");

    // Assemble
    std::cerr << "[proxychunk] Assembling final file...\n";
    std::ofstream out(output, std::ios::binary);
    if (!out)
    {
        std::cerr << "Cannot open output file\n";
        fs::remove_all(tmpdir);
        return false;
    }
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        fs::path part = tmpdir / ("part." + std::to_string(i));
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

    // Verify size
    if (fs::file_size(output) != static_cast<std::uintmax_t>(info.size))
    {
        std::cerr << "[proxychunk] Size mismatch after assemble!\n";
        return false;
    }

    std::cerr << "[proxychunk] Done. Saved to " << output << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void
usage(const char* prog)
{
    std::cerr << "Usage: " << prog << " <URL> [options]\n"
              << "  -o <file>       Output path (default: basename of URL)\n"
              << "  -c <N>          Max concurrent downloads (default: 16)\n"
              << "  -s <MB>         Chunk size in megabytes (default: 8)\n"
              << "  -p <N>          Max proxies to keep in pool (default: 40)\n"
              << "  -r <sec>        Proxy refresh interval (default: 180)\n"
              << "  -h              Help\n"
              << "\nFully automatic: fetches, tests, sorts and refreshes free HTTP proxies,\n"
              << "then downloads the file in parallel chunks through different IPs.\n";
}

int
main(int argc, char* argv[])
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    std::string url;
    std::string out_path;
    int concurrent = 16;
    int chunk_mb   = 8;
    int max_proxies = 40;
    int refresh_sec = 180;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-h" || a == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        if (a == "-o" && i + 1 < argc)
            out_path = argv[++i];
        else if (a == "-c" && i + 1 < argc)
            concurrent = std::atoi(argv[++i]);
        else if (a == "-s" && i + 1 < argc)
            chunk_mb = std::atoi(argv[++i]);
        else if (a == "-p" && i + 1 < argc)
            max_proxies = std::atoi(argv[++i]);
        else if (a == "-r" && i + 1 < argc)
            refresh_sec = std::atoi(argv[++i]);
        else if (a[0] != '-')
            url = a;
        else
        {
            std::cerr << "Unknown option " << a << "\n";
            return 1;
        }
    }

    if (url.empty())
    {
        usage(argv[0]);
        return 1;
    }

    if (out_path.empty())
    {
        auto pos = url.find_last_of('/');
        out_path = (pos == std::string::npos) ? "download.bin" : url.substr(pos + 1);
        if (out_path.empty() || out_path.find('?') != std::string::npos)
            out_path = "download.bin";
    }

    std::cerr << "[proxychunk] Starting. Target: " << url << "\n";
    std::cerr << "[proxychunk] Output: " << out_path << "  concurrent=" << concurrent
              << "  chunk=" << chunk_mb << "MB\n";

    ProxyPool pool(static_cast<std::size_t>(max_proxies), refresh_sec);
    pool.start();

    // Wait a bit for initial pool
    for (int i = 0; i < 30 && pool.size() < 3; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    if (pool.size() == 0)
    {
        std::cerr << "[proxychunk] No working proxies found. Aborting.\n";
        return 1;
    }

    bool ok = run_download(url, out_path, concurrent, chunk_mb, pool);
    pool.stop();
    return ok ? 0 : 1;
}
