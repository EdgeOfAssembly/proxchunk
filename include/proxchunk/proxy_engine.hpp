/**
 * @file proxy_engine.hpp
 * @brief Fetch, score, cache, and lease HTTP/SOCKS proxies for proxchunkd.
 */

#ifndef PROXCHUNK_PROXY_ENGINE_HPP
#define PROXCHUNK_PROXY_ENGINE_HPP

#include "proxchunk/curl_util.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace proxchunk {

/**
 * @brief One scored proxy endpoint.
 */
struct Proxy
{
    std::string address;           ///< scheme://host:port (no spaces).
    double      speed_mbps = 0.0;  ///< Last measured or EMA throughput.
    int         latency_ms = 99999;
    int         fails      = 0;    ///< Consecutive failures; dead at >= 4.
    bool        alive      = true;
    bool        busy       = false; ///< Held by an ACQUIRE lease.
    bool        target_ok  = true;  ///< Reaches last TARGET URL (Range). True if no TARGET yet.

    [[nodiscard]] bool operator>(const Proxy& o) const noexcept
    {
        return speed_mbps > o.speed_mbps;
    }
};

/**
 * @brief Progress of one scoring pass (initial foreground bar).
 *
 * @param completed Tests finished.
 * @param total     Candidate count.
 * @param live      Proxies that passed the speed floor.
 * @param curl_ok   libcurl CURLE_OK count.
 * @param done      True on the final callback of this pass.
 */
using ProxyProgressFn = std::function<void(std::size_t completed, std::size_t total,
                                           std::size_t live, std::size_t curl_ok, bool done)>;

/**
 * @brief Construction knobs for @ref ProxyEngine.
 */
struct ProxyEngineConfig
{
    std::size_t max_keep = 40;     ///< Live rows to retain (busy may exceed).
    int refresh_sec = 180;         ///< 0 disables the updater thread.
    std::string test_url;          ///< Empty → @ref k_default_test_url.
    std::filesystem::path cache_path;
    bool use_cache = true;
    bool use_tor = true;
    bool fetch_public = true;      ///< False = @c --no-fetch.
    std::vector<std::string> extra_proxies;
    std::string log_prefix = "[proxchunkd]";
    bool debug = false; ///< Log acquire/release/demote to stderr.
};

/**
 * @brief Default scored-cache path (`$XDG_CACHE_HOME/proxchunk/proxies.txt`).
 */
[[nodiscard]] inline std::filesystem::path
default_proxy_cache_path()
{
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0')
    {
        return std::filesystem::path(xdg) / "proxchunk" / "proxies.txt";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path(home) / ".cache" / "proxchunk" / "proxies.txt";
    }
    return std::filesystem::path("proxchunk.proxies");
}

/**
 * @brief Default user list (`$XDG_CONFIG_HOME/proxchunk/proxies.txt`).
 */
[[nodiscard]] inline std::filesystem::path
default_user_proxy_list_path()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0')
    {
        return std::filesystem::path(xdg) / "proxchunk" / "proxies.txt";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path(home) / ".config" / "proxchunk" / "proxies.txt";
    }
    return std::filesystem::path("proxies.txt");
}

/**
 * @brief In-process proxy pool used by proxchunkd.
 *
 * Does **not** call @c curl_global_init / @c curl_global_cleanup; the
 * daemon (or test) must init curl in the surviving process after fork.
 *
 * @thread_safety Public methods are safe to call from the poll thread
 * while scoring runs on a worker jthread.
 */
class ProxyEngine
{
public:
    explicit ProxyEngine(ProxyEngineConfig cfg);
    ~ProxyEngine();

    ProxyEngine(const ProxyEngine&) = delete;
    ProxyEngine& operator=(const ProxyEngine&) = delete;
    ProxyEngine(ProxyEngine&&) = delete;
    ProxyEngine& operator=(ProxyEngine&&) = delete;

    /**
     * @brief Kick the first refresh on a jthread and return immediately.
     *
     * @param[in] initial_progress Optional; used only for the first
     *            scoring pass (cache re-test and/or full refresh). Later
     *            updater passes are quiet.
     */
    void start(ProxyProgressFn initial_progress = {});

    /**
     * @brief Join scoring threads, then save the cache.
     *
     * Idempotent. Never calls @c curl_global_cleanup.
     */
    void stop();

    /**
     * @brief Non-blocking lease: fastest @c alive && !busy && fails < 4.
     *
     * @param[in] skip Addresses to avoid if any other row is free (next
     *            in the speed-sorted list). If every live row is skipped
     *            or busy, a skipped row may still be leased.
     */
    [[nodiscard]] std::optional<Proxy> acquire(
        const std::vector<std::string>& skip = {});

    /**
     * @brief Drop a lease. Unknown URLs are a no-op.
     *
     * @param[in] url     Address from a previous @ref acquire.
     * @param[in] success True on a good download/probe.
     * @param[in] mbps    Measured throughput; 0 leaves EMA unchanged.
     */
    void release(std::string_view url, bool success, double mbps);

    /** @return Count of @c alive rows. */
    [[nodiscard]] std::size_t live() const;

    /** @return Count of @c busy rows. */
    [[nodiscard]] std::size_t busy() const;

    /** @return Speed of the current top row, or 0. */
    [[nodiscard]] double top_mbps() const;

    /** @return True while the first refresh has not finished. */
    [[nodiscard]] bool warming() const;

    /**
     * @brief Range-probe every live row against @p url (the real file).
     *
     * GET @c Range: 0-8191 through each proxy. Only rows that return
     * HTTP 200/206 stay leasable. Updates speed from this test.
     *
     * @return How many proxies reached the target.
     */
    std::size_t verify_target(const std::string& url);

private:
    struct TestJob
    {
        std::string address;
        CURL*       easy = nullptr;
        std::chrono::steady_clock::time_point t0{};
    };

    void log(std::string_view msg) const;
    void refresh(bool initial, const ProxyProgressFn& progress);
    [[nodiscard]] bool try_reuse_cache(const ProxyProgressFn& progress);
    [[nodiscard]] std::vector<std::string> load_cache() const;
    void save_cache() const;
    void install_tested(std::vector<Proxy> tested);
    [[nodiscard]] static std::vector<std::string> parse_proxy_body(const std::string& body,
                                                                   std::string_view bare_scheme);
    [[nodiscard]] std::vector<std::string> local_proxy_urls() const;
    [[nodiscard]] std::vector<std::string> fetch_all_lists();
    CURL* make_test_easy(TestJob* job);
    [[nodiscard]] std::vector<Proxy> test_proxies_multi(const std::vector<std::string>& candidates,
                                                        std::size_t must_test_first,
                                                        const ProxyProgressFn& progress);

    ProxyEngineConfig         cfg_;
    std::vector<Proxy>        pool_;
    mutable std::shared_mutex mutex_;
    std::jthread              refresh_thread_;
    std::jthread              updater_;
    std::atomic<bool>         running_{false};
    std::atomic<bool>         stop_flag_{false};
    std::atomic<bool>         warming_{true};
    std::string               target_url_;
    bool                      target_filter_{false};
};

} // namespace proxchunk

#endif /* PROXCHUNK_PROXY_ENGINE_HPP */
