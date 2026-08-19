/*
 * proxchunk - Fully automatic multi-proxy Range chunked downloader
 * C++23, Linux CLI
 *
 * Fetches free HTTP proxies, scores them, splits the target into Range
 * chunks, and downloads each chunk through a different IP.
 */

#include "proxchunk/plan.hpp"

#include <libsf/tui/progress_bar.h>

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
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef PROXCHUNK_VERSION
#define PROXCHUNK_VERSION "1.2"
#endif

namespace fs = std::filesystem;

static constexpr const char* kUserAgent = "proxchunk/" PROXCHUNK_VERSION;

// ---------------------------------------------------------------------------
// libcurl callbacks
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
    return fwrite(ptr, 1, size * nmemb, f);
}

// ---------------------------------------------------------------------------
// Proxy pool
// ---------------------------------------------------------------------------

struct Proxy
{
    std::string address;
    double      speed_mbps = 0.0;
    int         latency_ms = 99999;
    int         fails      = 0;
    bool        alive      = true;
    bool        busy       = false;

    bool operator>(const Proxy& o) const noexcept
    {
        return speed_mbps > o.speed_mbps;
    }
};

class ProxyPool
{
public:
    explicit ProxyPool(std::size_t max_keep, int refresh_sec, std::string test_url)
        : max_keep_(max_keep)
        , refresh_sec_(refresh_sec)
        , test_url_(std::move(test_url))
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
        {
            return;
        }
        refresh();
        updater_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested())
            {
                for (int i = 0; i < refresh_sec_ && !st.stop_requested(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (!st.stop_requested())
                {
                    refresh();
                }
            }
        });
    }

    void stop()
    {
        if (!running_.exchange(false))
        {
            return;
        }
        if (updater_.joinable())
        {
            updater_.request_stop();
        }
    }

    [[nodiscard]] std::optional<Proxy>
    acquire()
    {
        std::unique_lock lock(mutex_);
        for (auto& p : pool_)
        {
            if (p.alive && !p.busy && p.fails < 3)
            {
                p.busy = true;
                return p;
            }
        }
        return std::nullopt;
    }

    void release(const Proxy& used, bool success, double measured)
    {
        std::unique_lock lock(mutex_);
        for (auto& p : pool_)
        {
            if (p.address != used.address)
            {
                continue;
            }
            p.busy = false;
            if (success)
            {
                p.fails = 0;
                if (measured > 0.0)
                {
                    p.speed_mbps = (p.speed_mbps * 0.7) + (measured * 0.3);
                }
                p.alive = true;
            }
            else
            {
                p.fails++;
                if (p.fails >= 4)
                {
                    p.alive = false;
                }
            }
            break;
        }
        std::sort(pool_.begin(), pool_.end(), std::greater<>{});
    }

    [[nodiscard]] std::size_t
    size() const
    {
        std::shared_lock lock(mutex_);
        std::size_t n = 0;
        for (const auto& p : pool_)
        {
            if (p.alive)
            {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] double
    top_speed_mbps() const
    {
        std::shared_lock lock(mutex_);
        return pool_.empty() ? 0.0 : pool_.front().speed_mbps;
    }

private:
    struct TestJob
    {
        std::string address;
        CURL*       easy = nullptr;
        std::chrono::steady_clock::time_point t0{};
    };

    void refresh()
    {
        std::vector<std::string> candidates = fetch_all_lists();
        if (candidates.empty())
        {
            std::cerr << "[proxchunk] No proxies fetched from sources\n";
            return;
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        {
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(candidates.begin(), candidates.end(), rng);
        }

        std::vector<Proxy> tested = test_proxies_multi(candidates);

        std::sort(tested.begin(), tested.end(), std::greater<>{});
        if (tested.size() > max_keep_)
        {
            tested.resize(max_keep_);
        }

        {
            std::unique_lock lock(mutex_);
            pool_ = std::move(tested);
        }

        std::cerr << "[proxchunk] Proxy pool refreshed: " << size()
                  << " good proxies (top " << top_speed_mbps() << " MB/s)\n";
    }

    [[nodiscard]] static std::vector<std::string>
    parse_proxy_body(const std::string& body)
    {
        std::vector<std::string> out;
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line))
        {
            line = proxchunk::trim(line);
            if (line.empty() || line[0] == '#' || line[0] == '/')
            {
                continue;
            }
            if (line.find("://") == std::string::npos)
            {
                if (line.find(':') == std::string::npos)
                {
                    continue;
                }
                line = "http://" + line;
            }
            out.push_back(std::move(line));
        }
        return out;
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
            "https://raw.githubusercontent.com/proxifly/free-proxy-list/main/proxies/protocols/http/data.txt",
        };

        std::vector<std::string> bodies(sources.size());
        std::vector<std::jthread> threads;
        threads.reserve(sources.size());
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            threads.emplace_back([&, i]() {
                CURL* c = curl_easy_init();
                if (!c)
                {
                    return;
                }
                curl_easy_setopt(c, CURLOPT_URL, sources[i].c_str());
                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_string);
                curl_easy_setopt(c, CURLOPT_WRITEDATA, &bodies[i]);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
                (void)curl_easy_perform(c);
                curl_easy_cleanup(c);
            });
        }
        threads.clear();

        std::vector<std::string> all;
        for (const auto& body : bodies)
        {
            auto part = parse_proxy_body(body);
            all.insert(all.end(), part.begin(), part.end());
        }
        return all;
    }

    CURL*
    make_test_easy(TestJob* job)
    {
        CURL* c = curl_easy_init();
        if (!c)
        {
            return nullptr;
        }
        const bool https = test_url_.starts_with("https://");
        curl_easy_setopt(c, CURLOPT_URL, test_url_.c_str());
        curl_easy_setopt(c, CURLOPT_PROXY, job->address.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(c, CURLOPT_PRIVATE, job);
        if (https)
        {
            curl_easy_setopt(c, CURLOPT_HTTPPROXYTUNNEL, 1L);
        }
        job->easy = c;
        job->t0 = std::chrono::steady_clock::now();
        return c;
    }

    [[nodiscard]] std::vector<Proxy>
    test_proxies_multi(const std::vector<std::string>& candidates)
    {
        constexpr int kMaxInflight = 96;
        const std::size_t total = candidates.size();
        const std::size_t want_live = max_keep_ + 8;
        const curl_off_t min_bytes = 1024;

        CURLM* multi = curl_multi_init();
        if (!multi)
        {
            return {};
        }
        curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)kMaxInflight);

        std::vector<Proxy> tested;
        std::size_t curl_ok = 0;
        std::vector<std::unique_ptr<TestJob>> jobs;
        jobs.reserve(std::min(total, static_cast<std::size_t>(kMaxInflight) * 2));

        std::size_t next = 0;
        std::size_t completed = 0;
        int inflight = 0;
        bool stop_adding = false;

        auto add_one = [&]() -> bool {
            if (stop_adding || next >= total || inflight >= kMaxInflight)
            {
                return false;
            }
            auto job = std::make_unique<TestJob>();
            job->address = candidates[next++];
            CURL* easy = make_test_easy(job.get());
            if (!easy)
            {
                return true;
            }
            curl_multi_add_handle(multi, easy);
            jobs.push_back(std::move(job));
            ++inflight;
            return true;
        };

        while (inflight < kMaxInflight && add_one())
        {
        }

        const bool use_bar = isatty(STDOUT_FILENO);
        tui::progress_bar_style style = tui::progress_bar_styles::blocks_smooth();
        style.use_gradient = true;
        style.gradient_start = tui::make_rgb(220, 160, 40);
        style.gradient_end = tui::make_rgb(40, 200, 120);
        style.percent_inside = true;
        if (use_bar)
        {
            std::cout << tui::cursor_hide;
            setvbuf(stdout, nullptr, _IONBF, 0);
        }

        auto draw = [&]() {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "proxies  live %zu  curl-ok %zu", tested.size(),
                          curl_ok);
            if (use_bar)
            {
                tui::progress_bar(msg, static_cast<long long>(completed),
                                  static_cast<long long>(total), 42, style);
                std::cout.flush();
            }
            else
            {
                std::fprintf(stderr, "\r[proxchunk] %s  %zu/%zu   ", msg, completed, total);
                std::fflush(stderr);
            }
        };
        draw();

        int still = 0;
        curl_multi_perform(multi, &still);
        while (still > 0)
        {
            int numfds = 0;
            curl_multi_poll(multi, nullptr, 0, 150, &numfds);
            curl_multi_perform(multi, &still);

            int queued = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi, &queued))
            {
                if (msg->msg != CURLMSG_DONE)
                {
                    continue;
                }
                CURL* easy = msg->easy_handle;
                char* priv = nullptr;
                curl_easy_getinfo(easy, CURLINFO_PRIVATE, &priv);
                auto* job = reinterpret_cast<TestJob*>(priv);

                curl_off_t nbytes = 0;
                long http_code = 0;
                curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &nbytes);
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_code);

                if (msg->data.result == CURLE_OK)
                {
                    ++curl_ok;
                }
                const bool ok = (msg->data.result == CURLE_OK && nbytes >= min_bytes
                                 && (http_code == 0 || http_code == 200 || http_code == 206));
                if (ok && job != nullptr)
                {
                    double secs = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - job->t0)
                                      .count();
                    if (secs < 0.01)
                    {
                        secs = 0.01;
                    }
                    Proxy p;
                    p.address = job->address;
                    p.speed_mbps =
                        (static_cast<double>(nbytes) / (1024.0 * 1024.0)) / secs;
                    p.latency_ms = static_cast<int>(secs * 1000.0);
                    p.alive = true;
                    if (p.speed_mbps > 0.02)
                    {
                        tested.push_back(std::move(p));
                    }
                }

                curl_multi_remove_handle(multi, easy);
                curl_easy_cleanup(easy);
                if (job != nullptr)
                {
                    job->easy = nullptr;
                }
                --inflight;
                ++completed;

                if (tested.size() >= want_live)
                {
                    stop_adding = true;
                }
                if (!stop_adding)
                {
                    add_one();
                }
            }

            if (stop_adding && inflight > 0)
            {
                // Drop remaining in-flight tests; we have enough live proxies.
                for (auto& job : jobs)
                {
                    if (job && job->easy != nullptr)
                    {
                        curl_multi_remove_handle(multi, job->easy);
                        curl_easy_cleanup(job->easy);
                        job->easy = nullptr;
                    }
                }
                inflight = 0;
                still = 0;
                completed = total;
                break;
            }
            draw();
        }

        if (use_bar)
        {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "proxies  live %zu  curl-ok %zu", tested.size(),
                          curl_ok);
            tui::progress_bar(msg, static_cast<long long>(total),
                              static_cast<long long>(total), 42, style);
            std::cout << '\n' << tui::cursor_show;
            std::cout.flush();
        }
        else
        {
            std::fprintf(stderr, "\n");
        }

        curl_multi_cleanup(multi);
        return tested;
    }

    std::size_t               max_keep_;
    int                       refresh_sec_;
    std::string               test_url_;
    std::vector<Proxy>        pool_;
    mutable std::shared_mutex mutex_;
    std::jthread              updater_;
    std::atomic<bool>         running_{false};
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
    {
        return std::unexpected("curl_easy_init failed");
    }

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_HEADER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);

    CURLcode res = curl_easy_perform(c);
    FileInfo info;

    if (res == CURLE_OK)
    {
        curl_off_t cl = -1;
        curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0)
        {
            info.size = static_cast<std::int64_t>(cl);
        }
    }
    curl_easy_cleanup(c);

    c = curl_easy_init();
    if (!c)
    {
        return std::unexpected("curl_easy_init failed");
    }

    std::string headers;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_to_string);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);

    res = curl_easy_perform(c);
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

[[nodiscard]] static bool
download_chunk(const std::string& url, const proxchunk::chunk& ch, const std::string& proxy,
               const fs::path& part_path, std::atomic<std::int64_t>& bytes_done)
{
    CURL* c = curl_easy_init();
    if (!c)
    {
        return false;
    }

    FILE* f = fopen(part_path.c_str(), "wb");
    if (!f)
    {
        curl_easy_cleanup(c);
        return false;
    }

    std::string range = std::to_string(ch.start) + "-" + std::to_string(ch.end);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    if (!proxy.empty())
    {
        curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
        if (url.starts_with("https://"))
        {
            curl_easy_setopt(c, CURLOPT_HTTPPROXYTUNNEL, 1L);
        }
    }
    curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode res = curl_easy_perform(c);

    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_off_t downloaded = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);

    fclose(f);
    curl_easy_cleanup(c);

    const std::int64_t want = ch.end - ch.start + 1;
    if (res != CURLE_OK || (code != 206 && code != 200)
        || downloaded < static_cast<curl_off_t>(want * 95 / 100))
    {
        fs::remove(part_path);
        return false;
    }

    bytes_done.fetch_add(static_cast<std::int64_t>(downloaded), std::memory_order_relaxed);
    return true;
}

struct RunOptions
{
    int  max_concurrent = 16;
    int  chunk_mb       = 8;
    std::int64_t limit_bytes = 0; ///< 0 = full file
    bool direct         = false;
    bool progress       = true;
};

[[nodiscard]] static bool
run_download(const std::string& url, const fs::path& output, const RunOptions& opt,
             ProxyPool* pool)
{
    auto probe = probe_file(url);
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

    const std::int64_t chunk_size = static_cast<std::int64_t>(opt.chunk_mb) * 1024 * 1024;
    auto chunks = proxchunk::plan_chunks(download_size, chunk_size);
    std::cerr << "[proxchunk] Split into " << chunks.size() << " chunks of ~" << opt.chunk_mb
              << " MB\n";

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
    std::mutex                work_mtx;
    std::size_t               next_chunk = 0;

    auto worker_fn = [&]() {
        while (true)
        {
            proxchunk::chunk ch;
            {
                std::lock_guard g(work_mtx);
                if (next_chunk >= chunks.size())
                {
                    return;
                }
                ch = chunks[next_chunk++];
            }

            bool ok = false;
            for (int attempt = 0; attempt < 6 && !ok; ++attempt)
            {
                std::string proxy_addr;
                std::optional<Proxy> px;
                if (!opt.direct && pool != nullptr)
                {
                    px = pool->acquire();
                    if (!px)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        continue;
                    }
                    proxy_addr = px->address;
                }
                fs::path part = tmpdir / ("part." + std::to_string(ch.id));
                auto t0 = std::chrono::steady_clock::now();
                ok = download_chunk(url, ch, proxy_addr, part, bytes_done);
                auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                                .count();
                double mbps = 0.0;
                if (ok && secs > 0.01)
                {
                    mbps = (static_cast<double>(ch.end - ch.start + 1) / (1024.0 * 1024.0)) / secs;
                }
                if (px)
                {
                    pool->release(*px, ok, mbps);
                }
                if (!ok)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }

            if (ok)
            {
                finished.fetch_add(1);
            }
            else
            {
                failed.fetch_add(1);
                std::cerr << "[proxchunk] Failed chunk " << ch.id << " after retries\n";
            }
        }
    };

    int n_workers = std::min(opt.max_concurrent, static_cast<int>(chunks.size()));
    n_workers = std::max(1, n_workers);
    std::vector<std::jthread> workers;
    for (int i = 0; i < n_workers; ++i)
    {
        workers.emplace_back(worker_fn);
    }

    const bool use_bar = opt.progress && isatty(STDOUT_FILENO);
    if (use_bar)
    {
        std::cout << tui::cursor_hide;
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

    tui::progress_bar_style style = tui::progress_bar_styles::blocks_smooth();
    style.use_gradient = true;
    style.gradient_start = tui::make_rgb(30, 180, 90);
    style.gradient_end = tui::make_rgb(40, 200, 255);
    style.percent_inside = true;

    const auto t_start = std::chrono::steady_clock::now();
    const int need = static_cast<int>(chunks.size());
    while (finished.load() + failed.load() < need)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const double done = static_cast<double>(bytes_done.load());
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        const double speed = elapsed > 0.1 ? (done / (1024.0 * 1024.0)) / elapsed : 0.0;
        if (use_bar)
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "%.2f MB/s  %d/%zu", speed, finished.load(),
                          chunks.size());
            tui::progress_bar(msg, bytes_done.load(), download_size, 42, style);
            std::cout.flush();
        }
        else
        {
            const double pct = 100.0 * done / static_cast<double>(download_size);
            std::fprintf(stderr, "\r[proxchunk] %.1f%%  %.2f MB/s  %d/%zu chunks   ", pct, speed,
                         finished.load(), chunks.size());
            std::fflush(stderr);
        }
    }
    workers.clear();

    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    const double speed = elapsed > 0.1
                             ? (static_cast<double>(bytes_done.load()) / (1024.0 * 1024.0)) / elapsed
                             : 0.0;

    if (use_bar)
    {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "%.2f MB/s  %d/%zu", speed, finished.load(), chunks.size());
        tui::progress_bar(msg, bytes_done.load(), download_size, 42, style);
        std::cout << '\n' << tui::cursor_show;
        std::cout.flush();
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
        << "  -c, --concurrent <N>  Max concurrent chunk downloads (default: 16)\n"
        << "  -s, --chunk-mb <MB>   Chunk size in megabytes (default: 8)\n"
        << "  -p, --proxies <N>     Max proxies to keep in pool (default: 40)\n"
        << "  -r, --refresh <sec>   Proxy refresh interval (default: 180)\n"
        << "      --limit-mb <MB>   Download only the first MB (0 = full file)\n"
        << "      --direct          Single-IP download (no proxies)\n"
        << "      --no-progress     Do not draw the TUI progress bar\n"
        << "  -h, --help            Show this help\n"
        << "  -v, --version         Print version\n"
        << "\n"
        << "Fetches and scores free HTTP proxies, then downloads Range chunks\n"
        << "through different IPs to beat per-IP throttle.\n";
}

int
main(int argc, char* argv[])
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 0;
    }

    std::string url;
    std::string out_path;
    int concurrent = 16;
    int chunk_mb   = 8;
    int max_proxies = 40;
    int refresh_sec = 180;
    RunOptions opt;

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
        }
        else if (a == "-s" || a == "--chunk-mb")
        {
            chunk_mb = std::atoi(need("-s"));
        }
        else if (a == "-p" || a == "--proxies")
        {
            max_proxies = std::atoi(need("-p"));
        }
        else if (a == "-r" || a == "--refresh")
        {
            refresh_sec = std::atoi(need("-r"));
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

    opt.max_concurrent = concurrent;
    opt.chunk_mb = chunk_mb;

    if (url.empty())
    {
        usage(argv[0]);
        return 0;
    }
    if (concurrent < 1 || chunk_mb < 1 || max_proxies < 1)
    {
        std::cerr << "concurrent, chunk-mb, and proxies must be >= 1\n";
        return 1;
    }

    if (out_path.empty())
    {
        out_path = proxchunk::default_output_name(url);
    }

    std::cerr << "[proxchunk] Starting. Target: " << url << "\n";
    std::cerr << "[proxchunk] Output: " << out_path << "  concurrent=" << concurrent
              << "  chunk=" << chunk_mb << "MB"
              << (opt.direct ? "  direct" : "") << "\n";

    if (opt.direct)
    {
        bool ok = run_download(url, out_path, opt, nullptr);
        return ok ? 0 : 1;
    }

    // Probe CONNECT on a small HTTPS object — not the target (avoids
    // hammering filesharing Range endpoints with thousands of tests).
    const char* test_url = url.starts_with("https://")
                               ? "https://speed.cloudflare.com/__down?bytes=65536"
                               : "http://speedtest.tele2.net/100KB.zip";
    ProxyPool pool(static_cast<std::size_t>(max_proxies), refresh_sec, test_url);
    pool.start();

    for (int i = 0; i < 45 && pool.size() < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (i % 5 == 4)
        {
            std::cerr << "[proxchunk] Waiting for proxy pool... " << pool.size() << " live\n";
        }
    }

    if (pool.size() == 0)
    {
        std::cerr << "[proxchunk] No working proxies found. Aborting.\n";
        return 1;
    }

    bool ok = run_download(url, out_path, opt, &pool);
    pool.stop();
    return ok ? 0 : 1;
}
