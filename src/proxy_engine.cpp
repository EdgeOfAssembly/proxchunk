/*
 * Proxy fetch / score / cache / lease engine for proxchunkd.
 */

#include "proxchunk/proxy_engine.hpp"
#include "proxchunk/plan.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace proxchunk {
namespace {

constexpr int k_max_inflight = 96;
constexpr curl_off_t k_min_test_bytes = 1024;

} // namespace

ProxyEngine::ProxyEngine(ProxyEngineConfig cfg)
    : cfg_(std::move(cfg))
{
    if (cfg_.test_url.empty())
    {
        cfg_.test_url = k_default_test_url;
    }
}

ProxyEngine::~ProxyEngine()
{
    stop();
}

void
ProxyEngine::log(std::string_view msg) const
{
    std::cerr << cfg_.log_prefix << ' ' << msg << '\n';
}

void
ProxyEngine::start(ProxyProgressFn initial_progress)
{
    if (running_.exchange(true))
    {
        return;
    }
    stop_flag_.store(false);
    warming_.store(true);
    refresh_thread_ = std::jthread([this, progress = std::move(initial_progress)](std::stop_token st) {
        (void)st;
        if (!stop_flag_.load() && !try_reuse_cache(progress) && !stop_flag_.load())
        {
            refresh(true, progress);
        }
        warming_.store(false);
        if (cfg_.refresh_sec > 0 && !stop_flag_.load())
        {
            updater_ = std::jthread([this](std::stop_token ust) {
                while (!ust.stop_requested() && !stop_flag_.load())
                {
                    const int total_ms = cfg_.refresh_sec * 1000;
                    int slept = 0;
                    while (slept < total_ms && !ust.stop_requested() && !stop_flag_.load())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        slept += 100;
                    }
                    if (!ust.stop_requested() && !stop_flag_.load())
                    {
                        refresh(false, {});
                    }
                }
            });
        }
    });
}

void
ProxyEngine::stop()
{
    stop_flag_.store(true);
    if (!running_.exchange(false))
    {
        return;
    }
    if (refresh_thread_.joinable())
    {
        refresh_thread_.request_stop();
        refresh_thread_.join();
    }
    if (updater_.joinable())
    {
        updater_.request_stop();
        updater_.join();
    }
    save_cache();
}

std::optional<Proxy>
ProxyEngine::acquire(const std::vector<std::string>& skip)
{
    std::unique_lock lock(mutex_);
    auto pick = [&](bool honor_skip) -> std::optional<Proxy> {
        for (auto& p : pool_)
        {
            if (!p.alive || p.busy || p.fails >= 4)
            {
                continue;
            }
            if (target_filter_ && !p.target_ok)
            {
                continue;
            }
            if (honor_skip)
            {
                bool skipped = false;
                for (const auto& s : skip)
                {
                    if (s == p.address)
                    {
                        skipped = true;
                        break;
                    }
                }
                if (skipped)
                {
                    continue;
                }
            }
            p.busy = true;
            if (cfg_.debug)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "ACQUIRE %s mbps=%.4f fails=%d skip=%zu",
                              p.address.c_str(), p.speed_mbps, p.fails, skip.size());
                log(buf);
            }
            return p;
        }
        return std::nullopt;
    };
    if (auto p = pick(true))
    {
        return p;
    }
    return pick(false);
}

void
ProxyEngine::release(std::string_view url, bool success, double mbps)
{
    std::unique_lock lock(mutex_);
    for (auto it = pool_.begin(); it != pool_.end(); ++it)
    {
        if (it->address != url)
        {
            continue;
        }
        it->busy = false;
        if (success)
        {
            it->fails = 0;
            if (mbps > 0.0)
            {
                it->speed_mbps = (it->speed_mbps * 0.7) + (mbps * 0.3);
            }
            it->alive = true;
        }
        else
        {
            it->fails++;
            it->speed_mbps *= 0.25; /* drop down the speed-sorted list */
            if (it->fails >= 4)
            {
                it->alive = false;
            }
            if (cfg_.debug)
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "RELEASE fail %s fails=%d mbps=%.4f %s",
                              it->address.c_str(), it->fails, it->speed_mbps,
                              it->alive ? "demoted" : "dead");
                log(buf);
            }
        }
        if (!it->alive && !it->busy)
        {
            pool_.erase(it);
        }
        break;
    }
    std::sort(pool_.begin(), pool_.end(), std::greater<>{});
}

std::size_t
ProxyEngine::live() const
{
    std::shared_lock lock(mutex_);
    std::size_t n = 0;
    for (const auto& p : pool_)
    {
        if (p.alive && (!target_filter_ || p.target_ok))
        {
            ++n;
        }
    }
    return n;
}

std::size_t
ProxyEngine::busy() const
{
    std::shared_lock lock(mutex_);
    std::size_t n = 0;
    for (const auto& p : pool_)
    {
        if (p.busy)
        {
            ++n;
        }
    }
    return n;
}

double
ProxyEngine::top_mbps() const
{
    std::shared_lock lock(mutex_);
    return pool_.empty() ? 0.0 : pool_.front().speed_mbps;
}

bool
ProxyEngine::warming() const
{
    return warming_.load();
}

void
ProxyEngine::install_tested(std::vector<Proxy> tested)
{
    std::unique_lock lock(mutex_);
    std::unordered_map<std::string, Proxy> old_busy;
    old_busy.reserve(pool_.size());
    for (const auto& p : pool_)
    {
        if (p.busy)
        {
            old_busy.emplace(p.address, p);
        }
    }
    for (auto& t : tested)
    {
        auto it = old_busy.find(t.address);
        if (it != old_busy.end())
        {
            t.busy = true;
            t.fails = it->second.fails;
            old_busy.erase(it);
        }
    }
    for (auto& [addr, p] : old_busy)
    {
        tested.push_back(std::move(p));
    }
    std::sort(tested.begin(), tested.end(), std::greater<>{});
    std::vector<char> taken(tested.size(), 0);
    std::vector<Proxy> kept;
    kept.reserve(std::max(tested.size(), cfg_.max_keep));
    for (std::size_t i = 0; i < tested.size(); ++i)
    {
        if (tested[i].busy)
        {
            kept.push_back(tested[i]);
            taken[i] = 1;
        }
    }
    for (std::size_t i = 0; i < tested.size() && kept.size() < cfg_.max_keep; ++i)
    {
        if (taken[i] == 0)
        {
            kept.push_back(tested[i]);
            taken[i] = 1;
        }
    }
    std::sort(kept.begin(), kept.end(), std::greater<>{});
    pool_ = std::move(kept);
}

void
ProxyEngine::refresh(bool initial, const ProxyProgressFn& progress)
{
    (void)initial;
    std::vector<std::string> rest;
    if (cfg_.fetch_public)
    {
        rest = fetch_all_lists();
        std::sort(rest.begin(), rest.end());
        rest.erase(std::unique(rest.begin(), rest.end()), rest.end());
        {
            std::mt19937 rng{std::random_device{}()};
            std::shuffle(rest.begin(), rest.end(), rng);
        }
    }
    auto locals = local_proxy_urls();
    std::vector<std::string> candidates = locals;
    candidates.insert(candidates.end(), rest.begin(), rest.end());
    if (candidates.empty())
    {
        log("No proxies fetched from sources");
        return;
    }

    std::vector<Proxy> tested = test_proxies_multi(candidates, locals.size(), progress);
    if (stop_flag_.load())
    {
        return;
    }
    std::sort(tested.begin(), tested.end(), std::greater<>{});
    install_tested(std::move(tested));

    char buf[160];
    std::snprintf(buf, sizeof(buf), "Proxy pool refreshed: %zu good proxies (top %.4f MB/s)",
                  live(), top_mbps());
    log(buf);
    save_cache();
}

bool
ProxyEngine::try_reuse_cache(const ProxyProgressFn& progress)
{
    if (!cfg_.use_cache)
    {
        return false;
    }
    auto cached = load_cache();
    if (cached.empty())
    {
        return false;
    }
    {
        std::ostringstream os;
        os << "Loaded " << cached.size() << " proxies from " << cfg_.cache_path.string();
        log(os.str());
    }
    auto locals = local_proxy_urls();
    std::vector<std::string> candidates = locals;
    candidates.insert(candidates.end(), cached.begin(), cached.end());
    if (stop_flag_.load())
    {
        return true;
    }
    std::vector<Proxy> tested = test_proxies_multi(candidates, locals.size(), progress);
    if (stop_flag_.load())
    {
        return true;
    }
    std::sort(tested.begin(), tested.end(), std::greater<>{});
    if (tested.size() > cfg_.max_keep)
    {
        tested.resize(cfg_.max_keep);
    }
    if (tested.size() < 3)
    {
        std::ostringstream os;
        os << "Cache too stale (" << tested.size() << " live). Full refresh.";
        log(os.str());
        return false;
    }
    install_tested(std::move(tested));
    {
        std::ostringstream os;
        os << "Reusing " << live() << " cached proxies (top " << top_mbps() << " MB/s)";
        log(os.str());
    }
    save_cache();
    return true;
}

std::vector<std::string>
ProxyEngine::load_cache() const
{
    std::vector<std::string> out;
    std::ifstream in(cfg_.cache_path);
    if (!in)
    {
        return out;
    }
    std::string line;
    while (std::getline(in, line))
    {
        line = trim(line);
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

void
ProxyEngine::save_cache() const
{
    if (!cfg_.use_cache || cfg_.cache_path.empty())
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
    std::filesystem::create_directories(cfg_.cache_path.parent_path(), ec);
    std::ofstream out(cfg_.cache_path, std::ios::trunc);
    if (!out)
    {
        log("Could not write proxy cache " + cfg_.cache_path.string());
        return;
    }
    out << "# proxchunk proxy cache\n";
    std::size_t n = 0;
    for (const auto& p : snap)
    {
        if (!p.alive)
        {
            continue;
        }
        out << p.address << ' ' << p.speed_mbps << '\n';
        ++n;
    }
    log("Saved " + std::to_string(n) + " proxies to " + cfg_.cache_path.string());
}

std::vector<std::string>
ProxyEngine::parse_proxy_body(const std::string& body, std::string_view bare_scheme)
{
    std::vector<std::string> out;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line))
    {
        line = trim(line);
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

std::vector<std::string>
ProxyEngine::local_proxy_urls() const
{
    std::vector<std::string> loc = cfg_.extra_proxies;
    if (cfg_.use_tor)
    {
        loc.insert(loc.begin(), "socks5h://127.0.0.1:9050");
        loc.push_back("socks5h://127.0.0.1:9150");
        log("Including local Tor socks5h://127.0.0.1:9050");
    }
    return loc;
}

std::vector<std::string>
ProxyEngine::fetch_all_lists()
{
    struct ListSource
    {
        const char* url;
        const char* scheme;
    };
    static const ListSource sources[] = {
        {"https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/http.txt", "http://"},
        {"https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/http.txt", "http://"},
        {"https://raw.githubusercontent.com/ProxyScraper/ProxyScraper/main/http.txt", "http://"},
        {"https://raw.githubusercontent.com/clarketm/proxy-list/master/proxy-list-raw.txt", "http://"},
        {"https://raw.githubusercontent.com/ShiftyTR/Proxy-List/master/http.txt", "http://"},
        {"https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/txt/proxies-http.txt",
         "http://"},
        {"https://api.proxyscrape.com/v2/?request=displayproxies&protocol=http&timeout=8000&country=all&ssl=all&anonymity=all",
         "http://"},
        {"https://raw.githubusercontent.com/proxifly/free-proxy-list/main/proxies/protocols/http/data.txt",
         "http://"},
        {"https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/socks5.txt", "socks5h://"},
        {"https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/socks5.txt", "socks5h://"},
    };
    const std::size_t nsrc = sizeof(sources) / sizeof(sources[0]);

    std::vector<std::string> bodies(nsrc);
    std::vector<std::jthread> threads;
    threads.reserve(nsrc);
    for (std::size_t i = 0; i < nsrc; ++i)
    {
        if (stop_flag_.load())
        {
            break;
        }
        threads.emplace_back([this, &bodies, i]() {
            if (stop_flag_.load())
            {
                return;
            }
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
            curl_easy_setopt(c, CURLOPT_USERAGENT, k_user_agent);
            apply_fast_tcp(c);
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION,
                             +[](void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                                 auto* flag = static_cast<std::atomic<bool>*>(clientp);
                                 return (flag != nullptr && flag->load()) ? 1 : 0;
                             });
            curl_easy_setopt(c, CURLOPT_XFERINFODATA, &stop_flag_);
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
ProxyEngine::make_test_easy(TestJob* job)
{
    CURL* c = curl_easy_init();
    if (!c)
    {
        return nullptr;
    }
    const bool https = cfg_.test_url.starts_with("https://");
    curl_easy_setopt(c, CURLOPT_URL, cfg_.test_url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, k_user_agent);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_PRIVATE, job);
    apply_curl_proxy(c, job->address, https);
    apply_fast_tcp(c);
    if (is_socks_proxy(job->address))
    {
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
    }
    job->easy = c;
    job->t0 = std::chrono::steady_clock::now();
    return c;
}

std::vector<Proxy>
ProxyEngine::test_proxies_multi(const std::vector<std::string>& candidates,
                                std::size_t must_test_first, const ProxyProgressFn& progress)
{
    const std::size_t total = candidates.size();
    const std::size_t want_live = cfg_.max_keep + 8;

    CURLM* multi = curl_multi_init();
    if (!multi)
    {
        return {};
    }
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(k_max_inflight));

    std::vector<Proxy> tested;
    std::size_t curl_ok = 0;
    std::vector<std::unique_ptr<TestJob>> jobs;
    jobs.reserve(std::min(total, static_cast<std::size_t>(k_max_inflight) * 2));

    std::size_t next = 0;
    std::size_t completed = 0;
    int inflight = 0;
    bool stop_adding = false;

    auto add_one = [&]() -> bool {
        if (stop_adding || next >= total || inflight >= k_max_inflight || stop_flag_.load())
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

    while (inflight < k_max_inflight && add_one())
    {
    }

    auto emit = [&](bool done) {
        if (progress)
        {
            progress(completed, total, tested.size(), curl_ok, done);
        }
    };
    emit(false);

    int still = 0;
    curl_multi_perform(multi, &still);
    while (still > 0 && !stop_flag_.load())
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
            const bool ok = (msg->data.result == CURLE_OK && nbytes >= k_min_test_bytes
                             && (http_code == 0 || http_code == 200 || http_code == 206));
            if (ok && job != nullptr)
            {
                double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - job->t0)
                                  .count();
                if (secs < 0.01)
                {
                    secs = 0.01;
                }
                Proxy p;
                p.address = job->address;
                p.speed_mbps = (static_cast<double>(nbytes) / (1024.0 * 1024.0)) / secs;
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

        if ((stop_adding && inflight > 0) || stop_flag_.load())
        {
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
        emit(false);
    }

    emit(true);
    curl_multi_cleanup(multi);
    return tested;
}

std::size_t
ProxyEngine::verify_target(const std::string& url)
{
    if (url.empty())
    {
        return 0;
    }
    std::vector<std::string> addrs;
    {
        std::unique_lock lock(mutex_);
        target_url_ = url;
        target_filter_ = true;
        for (auto& p : pool_)
        {
            p.target_ok = false;
            if (p.alive)
            {
                addrs.push_back(p.address);
            }
        }
    }
    log(std::string("TARGET Range-test ") + std::to_string(addrs.size()) + " proxies vs " + url);

    struct Job
    {
        std::string address;
        CURL* easy = nullptr;
        std::chrono::steady_clock::time_point t0{};
    };
    std::vector<std::unique_ptr<Job>> jobs;
    CURLM* multi = curl_multi_init();
    if (!multi)
    {
        return 0;
    }
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 32L);

    auto make = [&](const std::string& addr) -> CURL* {
        auto job = std::make_unique<Job>();
        job->address = addr;
        job->t0 = std::chrono::steady_clock::now();
        CURL* c = curl_easy_init();
        if (!c)
        {
            return nullptr;
        }
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_RANGE, "0-8191");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_null);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, k_user_agent);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
        apply_curl_proxy(c, addr, url.starts_with("https://"));
        apply_fast_tcp(c);
        curl_easy_setopt(c, CURLOPT_PRIVATE, job.get());
        job->easy = c;
        jobs.push_back(std::move(job));
        return c;
    };

    int inflight = 0;
    std::size_t next = 0;
    auto add = [&]() {
        while (inflight < 32 && next < addrs.size() && !stop_flag_.load())
        {
            CURL* c = make(addrs[next++]);
            if (c == nullptr)
            {
                continue;
            }
            curl_multi_add_handle(multi, c);
            ++inflight;
        }
    };
    add();

    std::unordered_map<std::string, double> ok_speed;
    int still = 0;
    curl_multi_perform(multi, &still);
    while (still > 0 && !stop_flag_.load())
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
            auto* job = reinterpret_cast<Job*>(priv);
            long code = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
            const bool ok = (msg->data.result == CURLE_OK && (code == 206 || code == 200));
            if (ok && job != nullptr)
            {
                double secs =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - job->t0)
                        .count();
                if (secs < 0.01)
                {
                    secs = 0.01;
                }
                ok_speed[job->address] = (8192.0 / (1024.0 * 1024.0)) / secs;
                if (cfg_.debug)
                {
                    log(std::string("target-ok ") + job->address);
                }
            }
            else if (cfg_.debug && job != nullptr)
            {
                log(std::string("target-fail ") + job->address);
            }
            curl_multi_remove_handle(multi, easy);
            curl_easy_cleanup(easy);
            --inflight;
            add();
        }
    }
    curl_multi_cleanup(multi);

    std::unique_lock lock(mutex_);
    for (auto& p : pool_)
    {
        auto it = ok_speed.find(p.address);
        if (it != ok_speed.end())
        {
            p.target_ok = true;
            p.speed_mbps = it->second;
            p.fails = 0;
            p.alive = true;
        }
        else
        {
            p.target_ok = false;
        }
    }
    std::sort(pool_.begin(), pool_.end(), std::greater<>{});
    const std::size_t n = ok_speed.size();
    log(std::string("TARGET ok ") + std::to_string(n) + " / " + std::to_string(addrs.size()));
    return n;
}

} // namespace proxchunk
