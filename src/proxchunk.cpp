/*
 * proxchunk - Fully automatic multi-proxy Range chunked downloader
 * C++23, Linux CLI
 *
 * Fetches free HTTP proxies, scores them, splits the target into Range
 * chunks, and downloads each chunk through a different IP.
 */

#include "proxchunk/plan.hpp"
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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef PROXCHUNK_VERSION
#define PROXCHUNK_VERSION "1.0"
#endif

namespace fs = std::filesystem;

static constexpr const char* kUserAgent = "proxchunk/" PROXCHUNK_VERSION;

[[nodiscard]] static bool
is_socks_proxy(std::string_view proxy)
{
    return proxy.starts_with("socks5://") || proxy.starts_with("socks5h://")
           || proxy.starts_with("socks4://") || proxy.starts_with("socks4a://");
}

/** HTTP proxies need CONNECT for HTTPS; SOCKS5 already tunnels TCP. */
static void
apply_curl_proxy(CURL* c, const std::string& proxy, bool target_https)
{
    if (proxy.empty())
    {
        return;
    }
    curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
    if (target_https && !is_socks_proxy(proxy))
    {
        curl_easy_setopt(c, CURLOPT_HTTPPROXYTUNNEL, 1L);
    }
}

/** Pin a block of progress-bar rows and CUP to each on update (no newlines). */
struct BarLayout
{
    int origin_row = 1;
    int n_lines    = 0;

    void begin(int lines)
    {
        tui::detail::set_stdout_unbuffered(true);
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

        int row = 0;
        if (isatty(STDIN_FILENO))
        {
            tui::detail::set_raw(true);
            auto pos = tui::detail::query_cursor_position(200000);
            tui::detail::set_raw(false);
            if (pos && pos->row > 0)
            {
                row = pos->row;
            }
        }

        n_lines = lines;
        if (row <= 0)
        {
            row = term_rows - lines + 1;
        }
        if (row + lines - 1 > term_rows)
        {
            row = term_rows - lines + 1;
        }
        if (row < 1)
        {
            row = 1;
        }
        origin_row = row;
    }

    void go_line(int i) const
    {
        std::cout << "\033[" << (origin_row + i) << ";1H";
    }

    void finish() const
    {
        std::cout << "\033[" << (origin_row + n_lines) << ";1H" << tui::line_wrap;
        tui::detail::show_cursor();
    }
};

[[nodiscard]] static fs::path
default_proxy_cache_path()
{
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0')
    {
        return fs::path(xdg) / "proxchunk" / "proxies.txt";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return fs::path(home) / ".cache" / "proxchunk" / "proxies.txt";
    }
    return fs::path("proxchunk.proxies");
}

[[nodiscard]] static fs::path
default_user_proxy_list_path()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0')
    {
        return fs::path(xdg) / "proxchunk" / "proxies.txt";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return fs::path(home) / ".config" / "proxchunk" / "proxies.txt";
    }
    return fs::path("proxies.txt");
}

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
    explicit ProxyPool(std::size_t max_keep, int refresh_sec, std::string test_url,
                       fs::path cache_path, bool use_cache, bool use_tor,
                       std::vector<std::string> extra_proxies)
        : max_keep_(max_keep)
        , refresh_sec_(refresh_sec)
        , test_url_(std::move(test_url))
        , cache_path_(std::move(cache_path))
        , use_cache_(use_cache)
        , use_tor_(use_tor)
        , extra_proxies_(std::move(extra_proxies))
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
        if (!try_reuse_cache())
        {
            refresh();
        }
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
        save_cache();
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
        std::vector<std::string> rest = fetch_all_lists();
        std::sort(rest.begin(), rest.end());
        rest.erase(std::unique(rest.begin(), rest.end()), rest.end());
        {
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(rest.begin(), rest.end(), rng);
        }
        auto locals = local_proxy_urls();
        std::vector<std::string> candidates = locals;
        candidates.insert(candidates.end(), rest.begin(), rest.end());
        if (candidates.empty())
        {
            std::cerr << "[proxchunk] No proxies fetched from sources\n";
            return;
        }

        std::vector<Proxy> tested = test_proxies_multi(candidates, locals.size());

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
        save_cache();
    }

    [[nodiscard]] bool
    try_reuse_cache()
    {
        if (!use_cache_)
        {
            return false;
        }
        auto cached = load_cache();
        if (cached.empty())
        {
            return false;
        }
        std::cerr << "[proxchunk] Loaded " << cached.size() << " proxies from " << cache_path_
                  << "\n";
        auto locals = local_proxy_urls();
        std::vector<std::string> candidates = locals;
        candidates.insert(candidates.end(), cached.begin(), cached.end());
        std::vector<Proxy> tested = test_proxies_multi(candidates, locals.size());
        std::sort(tested.begin(), tested.end(), std::greater<>{});
        if (tested.size() > max_keep_)
        {
            tested.resize(max_keep_);
        }
        if (tested.size() < 3)
        {
            std::cerr << "[proxchunk] Cache too stale (" << tested.size()
                      << " live). Full refresh.\n";
            return false;
        }
        {
            std::unique_lock lock(mutex_);
            pool_ = std::move(tested);
        }
        std::cerr << "[proxchunk] Reusing " << size() << " cached proxies (top "
                  << top_speed_mbps() << " MB/s)\n";
        save_cache();
        return true;
    }

    [[nodiscard]] std::vector<std::string>
    load_cache() const
    {
        std::vector<std::string> out;
        std::ifstream in(cache_path_);
        if (!in)
        {
            return out;
        }
        std::string line;
        while (std::getline(in, line))
        {
            line = proxchunk::trim(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            auto sp = line.find(' ');
            std::string addr = (sp == std::string::npos) ? line : line.substr(0, sp);
            if (addr.find("://") == std::string::npos)
            {
                if (addr.find(':') == std::string::npos)
                {
                    continue;
                }
                addr = "http://" + addr;
            }
            out.push_back(std::move(addr));
        }
        return out;
    }

    void save_cache() const
    {
        if (!use_cache_ || cache_path_.empty())
        {
            return;
        }
        std::vector<Proxy> snap;
        {
            std::shared_lock lock(mutex_);
            snap = pool_;
        }
        if (snap.empty())
        {
            return;
        }
        std::error_code ec;
        fs::create_directories(cache_path_.parent_path(), ec);
        std::ofstream out(cache_path_, std::ios::trunc);
        if (!out)
        {
            std::cerr << "[proxchunk] Could not write proxy cache " << cache_path_ << "\n";
            return;
        }
        out << "# proxchunk proxy cache\n";
        for (const auto& p : snap)
        {
            if (!p.alive)
            {
                continue;
            }
            out << p.address << ' ' << p.speed_mbps << '\n';
        }
        std::cerr << "[proxchunk] Saved " << snap.size() << " proxies to " << cache_path_ << "\n";
    }

    [[nodiscard]] static std::vector<std::string>
    parse_proxy_body(const std::string& body, std::string_view bare_scheme)
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
                line = std::string(bare_scheme) + line;
            }
            out.push_back(std::move(line));
        }
        return out;
    }

    [[nodiscard]] std::vector<std::string>
    local_proxy_urls() const
    {
        std::vector<std::string> loc = extra_proxies_;
        if (use_tor_)
        {
            loc.insert(loc.begin(), "socks5h://127.0.0.1:9050");
            loc.push_back("socks5h://127.0.0.1:9150");
            std::cerr << "[proxchunk] Including local Tor socks5h://127.0.0.1:9050\n";
        }
        return loc;
    }

    [[nodiscard]] std::vector<std::string>
    fetch_all_lists()
    {
        struct ListSource
        {
            const char* url;
            const char* scheme; /* prepended when the list is host:port only */
        };
        static const ListSource sources[] = {
            {"https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/http.txt", "http://"},
            {"https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/http.txt", "http://"},
            {"https://raw.githubusercontent.com/ProxyScraper/ProxyScraper/main/http.txt", "http://"},
            {"https://raw.githubusercontent.com/clarketm/proxy-list/master/proxy-list-raw.txt", "http://"},
            {"https://raw.githubusercontent.com/ShiftyTR/Proxy-List/master/http.txt", "http://"},
            {"https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/txt/proxies-http.txt", "http://"},
            {"https://api.proxyscrape.com/v2/?request=displayproxies&protocol=http&timeout=8000&country=all&ssl=all&anonymity=all", "http://"},
            {"https://raw.githubusercontent.com/proxifly/free-proxy-list/main/proxies/protocols/http/data.txt", "http://"},
            {"https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/socks5.txt", "socks5h://"},
            {"https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/socks5.txt", "socks5h://"},
        };
        const std::size_t nsrc = sizeof(sources) / sizeof(sources[0]);

        std::vector<std::string> bodies(nsrc);
        std::vector<std::jthread> threads;
        threads.reserve(nsrc);
        for (std::size_t i = 0; i < nsrc; ++i)
        {
            threads.emplace_back([&, i]() {
                CURL* c = curl_easy_init();
                if (!c)
                {
                    return;
                }
                curl_easy_setopt(c, CURLOPT_URL, sources[i].url);
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
        for (std::size_t i = 0; i < nsrc; ++i)
        {
            auto part = parse_proxy_body(bodies[i], sources[i].scheme);
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
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(c, CURLOPT_PRIVATE, job);
        apply_curl_proxy(c, job->address, https);
        if (is_socks_proxy(job->address))
        {
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
        }
        job->easy = c;
        job->t0 = std::chrono::steady_clock::now();
        return c;
    }

    [[nodiscard]] std::vector<Proxy>
    test_proxies_multi(const std::vector<std::string>& candidates,
                       std::size_t must_test_first = 0)
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
        BarLayout proxy_bars;
        if (use_bar)
        {
            proxy_bars.begin(1);
        }

        auto draw = [&]() {
            char msg[80];
            std::snprintf(msg, sizeof(msg), "proxies  live %zu  curl-ok %zu", tested.size(),
                          curl_ok);
            if (use_bar)
            {
                proxy_bars.go_line(0);
                tui::progress_bar(msg, static_cast<long long>(completed),
                                  static_cast<long long>(total), 42, style);
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

                if (tested.size() >= want_live && next >= must_test_first)
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
            proxy_bars.go_line(0);
            tui::progress_bar(msg, static_cast<long long>(total),
                              static_cast<long long>(total), 42, style);
            proxy_bars.finish();
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
    fs::path                  cache_path_;
    bool                      use_cache_ = true;
    bool                      use_tor_ = true;
    std::vector<std::string>  extra_proxies_;
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
    mutable std::mutex         ip_mu;
    char                       ip[kIpv4FieldWidth + 1]{};
    std::atomic<std::int64_t>  last_byte{0};
    std::atomic<std::int64_t>  last_move_ms{0};

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
    if (dlnow >= 0)
    {
        s->now.store(static_cast<std::int64_t>(dlnow), std::memory_order_relaxed);
    }
    return 0;
}

[[nodiscard]] static bool
download_chunk(const std::string& url, const proxchunk::chunk& ch, const std::string& proxy,
               const fs::path& part_path, SlotProgress* slot)
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
    apply_curl_proxy(c, proxy, url.starts_with("https://"));
    curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    const std::int64_t want = ch.end - ch.start + 1;
    /* Drop a crawl before it occupies a worker for minutes (was 1 KiB/s for 30 s). */
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
        slot->last_byte.store(0);
        slot->last_move_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    if (slot != nullptr)
    {
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
    curl_easy_cleanup(c);

    if (res != CURLE_OK || (code != 206 && code != 200)
        || downloaded < static_cast<curl_off_t>(want * 95 / 100))
    {
        fs::remove(part_path);
        return false;
    }

    if (slot != nullptr)
    {
        slot->now.store(static_cast<std::int64_t>(downloaded), std::memory_order_relaxed);
    }
    return true;
}

struct RunOptions
{
    int  max_concurrent = 0; ///< filled from hardware_concurrency unless -c
    int  chunk_mb       = 8;
    std::int64_t limit_bytes = 0; ///< 0 = full file
    bool direct         = false;
    bool progress       = true;
    bool show_proxies   = false;
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
    std::mutex work_mtx;
    struct Job
    {
        proxchunk::chunk ch;
        int              attempts = 0;
    };
    std::deque<Job> jobs;
    for (const auto& c : chunks)
    {
        jobs.push_back(Job{c, 0});
    }
    std::atomic<int> inflight{0};

    int n_workers = std::min(opt.max_concurrent, static_cast<int>(chunks.size()));
    n_workers = std::max(1, n_workers);
    std::vector<SlotProgress> slots(chunks.size());
    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        slots[i].chunk_id.store(chunks[i].id);
        slots[i].want.store(chunks[i].end - chunks[i].start + 1);
        slots[i].now.store(0);
    }

    auto worker_fn = [&]() {
        while (true)
        {
            Job job;
            {
                std::unique_lock lock(work_mtx);
                if (jobs.empty())
                {
                    if (inflight.load() == 0)
                    {
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
            auto& sp = slots[static_cast<std::size_t>(ch.id)];

            std::string proxy_addr;
            std::optional<Proxy> px;
            if (!opt.direct && pool != nullptr)
            {
                for (int w = 0; w < 40 && !px; ++w)
                {
                    px = pool->acquire();
                    if (!px)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    }
                }
                if (px)
                {
                    proxy_addr = px->address;
                    sp.set_ip(proxy_addr);
                }
            }
            else
            {
                sp.set_ip("direct");
            }

            bool ok = false;
            if (opt.direct || px)
            {
                fs::path part = tmpdir / ("part." + std::to_string(ch.id));
                sp.now.store(0);
                auto t0 = std::chrono::steady_clock::now();
                ok = download_chunk(url, ch, proxy_addr, part, &sp);
                auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                                .count();
                double mbps = 0.0;
                if (ok && secs > 0.01)
                {
                    mbps = (static_cast<double>(want) / (1024.0 * 1024.0)) / secs;
                }
                if (px)
                {
                    pool->release(*px, ok, mbps);
                }
            }

            if (ok)
            {
                sp.now.store(want);
                bytes_done.fetch_add(want);
                finished.fetch_add(1);
            }
            else
            {
                sp.now.store(0);
                job.attempts++;
                if (job.attempts < 8)
                {
                    std::cerr << "[proxchunk] Requeue chunk " << ch.id << " (try "
                              << job.attempts + 1 << "/8)\n";
                    std::lock_guard g(work_mtx);
                    jobs.push_back(job);
                }
                else
                {
                    failed.fetch_add(1);
                    std::cerr << "[proxchunk] Failed chunk " << ch.id << " after retries\n";
                }
            }
            inflight.fetch_sub(1);
        }
    };

    std::vector<std::jthread> workers;
    for (int i = 0; i < n_workers; ++i)
    {
        workers.emplace_back(worker_fn);
    }

    const bool use_bar = opt.progress && isatty(STDOUT_FILENO);

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
    const int n_bars = static_cast<int>(chunks.size());
    BarLayout chunk_bars;
    if (use_bar)
    {
        chunk_bars.begin(n_bars + 1);
    }

    auto live_bytes = [&]() -> std::int64_t {
        std::int64_t n = 0;
        for (const auto& s : slots)
        {
            n += s.now.load();
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
                const std::int64_t now = slots[static_cast<std::size_t>(i)].now.load();
                const std::int64_t want = slots[static_cast<std::size_t>(i)].want.load();
                char msg[64];
                if (opt.show_proxies)
                {
                    char ip[kIpv4FieldWidth + 1];
                    slots[static_cast<std::size_t>(i)].copy_ip(ip);
                    std::snprintf(msg, sizeof(msg), "%s chunk %d", ip, i);
                }
                else
                {
                    std::snprintf(msg, sizeof(msg), "chunk %d", i);
                }
                chunk_bars.go_line(i);
                tui::progress_bar(msg, now, want > 0 ? want : 1, 36, chunk_style);
            }
            char tmsg[80];
            std::snprintf(tmsg, sizeof(tmsg), "total  %.2f MB/s  %d/%zu", speed, finished.load(),
                          chunks.size());
            chunk_bars.go_line(n_bars);
            tui::progress_bar(tmsg, live, download_size, 36, total_style);
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
        << "  -c, --concurrent <N>  Max concurrent chunk downloads (default: logical CPUs)\n"
        << "  -s, --chunk-mb <MB>   Chunk size in megabytes (default: 8)\n"
        << "  -p, --proxies <N>     Max proxies to keep in pool (default: 40)\n"
        << "  -r, --refresh <sec>   Proxy refresh interval (default: 180)\n"
        << "      --limit-mb <MB>   Download only the first MB (0 = full file)\n"
        << "      --direct          Single-IP download (no proxies)\n"
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
        << "Fetches and scores free HTTP proxies, then downloads Range chunks\n"
        << "through different IPs to beat per-IP throttle.\n";
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
    int chunk_mb   = 8;
    int max_proxies = 40;
    int refresh_sec = 180;
    RunOptions opt;
    bool use_cache = true;
    bool use_tor = true;
    bool use_user_list = true;
    std::vector<std::string> extra_proxies;
    std::vector<std::string> proxy_files;

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
    auto load_list = [&](const fs::path& p) {
        auto rows = proxchunk::load_proxy_file(p.string());
        if (!rows.empty())
        {
            std::cerr << "[proxchunk] Loaded " << rows.size() << " user proxies from " << p
                      << "\n";
            extra_proxies.insert(extra_proxies.end(), rows.begin(), rows.end());
        }
        else if (!p.empty() && fs::exists(p))
        {
            std::cerr << "[proxchunk] User proxy file empty or unreadable: " << p << "\n";
        }
    };
    if (use_user_list)
    {
        load_list(default_user_proxy_list_path());
    }
    for (const auto& pf : proxy_files)
    {
        load_list(pf);
        if (!fs::exists(pf))
        {
            std::cerr << "[proxchunk] --proxy-file not found: " << pf << "\n";
        }
    }

    const char* test_url = url.starts_with("https://")
                               ? "https://speed.cloudflare.com/__down?bytes=65536"
                               : "http://speedtest.tele2.net/100KB.zip";
    ProxyPool pool(static_cast<std::size_t>(max_proxies), refresh_sec, test_url,
                   default_proxy_cache_path(), use_cache, use_tor, extra_proxies);
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
