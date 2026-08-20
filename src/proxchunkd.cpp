/*
 * proxchunkd — proxy fetch/score daemon (UNIX socket IPC).
 */

#include "proxchunk/plan.hpp"
#include "proxchunk/proxy_engine.hpp"
#include "proxchunk/proxy_ipc.hpp"
#include "proxchunk/unique_fd.hpp"

#include <libsf/tui/progress_bar.h>
#include <libsf/tui/detail/terminal.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifndef PROXCHUNK_VERSION
#define PROXCHUNK_VERSION "1.1"
#endif

namespace fs = std::filesystem;
using proxchunk::unique_fd;

namespace {

struct Options
{
    bool foreground = false;
    bool do_stop = false;
    bool do_status = false;
    fs::path socket_path;
    fs::path pid_path;
    fs::path log_path;
    int max_proxies = 40;
    int refresh_sec = 180;
    std::string test_url = proxchunk::k_default_test_url;
    bool use_cache = true;
    bool use_tor = true;
    bool fetch_public = true;
    bool use_user_list = true;
    bool debug = false;
    std::vector<std::string> extra_proxies;
    std::vector<std::string> proxy_files;
};

struct Conn
{
    unique_fd fd;
    std::string buf;
    std::vector<std::string> held;
    bool greeted = false;
};

struct BarLayout
{
    int origin_row = 1;
    int n_lines = 0;

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

struct FgBar
{
    bool use_bar = false;
    bool started = false;
    BarLayout layout;
    tui::progress_bar_style style{};

    void draw(std::size_t completed, std::size_t total, std::size_t live, std::size_t curl_ok,
              bool done)
    {
        if (!use_bar)
        {
            return;
        }
        if (!started)
        {
            style = tui::progress_bar_styles::blocks_smooth();
            style.use_gradient = true;
            style.gradient_start = tui::make_rgb(220, 160, 40);
            style.gradient_end = tui::make_rgb(40, 200, 120);
            style.percent_inside = true;
            layout.begin(1);
            started = true;
        }
        char msg[80];
        std::snprintf(msg, sizeof(msg), "proxies  live %zu  curl-ok %zu", live, curl_ok);
        layout.go_line(0);
        const long long tot = total > 0 ? static_cast<long long>(total) : 1;
        const long long now = done ? tot : static_cast<long long>(completed);
        tui::progress_bar(msg, now, tot, 42, style);
        if (done)
        {
            layout.finish();
            started = false;
        }
    }
};

void
usage(const char* prog)
{
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "       " << prog << " --foreground [options]\n"
        << "       " << prog << " --stop\n"
        << "       " << prog << " --status\n"
        << "\n"
        << "Start the proxy engine daemon. With no arguments the process\n"
        << "daemonizes; the parent exits 0 when the UNIX socket is listening\n"
        << "(initial proxy refresh may still be running — ACQUIRE retries).\n"
        << "\n"
        << "      --foreground      Stay in the terminal (Ctrl+C = graceful stop)\n"
        << "      --stop            Graceful stop (socket STOP, then SIGTERM)\n"
        << "      --status          Ping the socket; print live/busy/top\n"
        << "      --socket <path>   UNIX socket (default: $XDG_RUNTIME_DIR/proxchunk/proxchunkd.sock)\n"
        << "      --pid-file <path> PID file (default: sibling proxchunkd.pid)\n"
        << "  -p, --proxies <N>     Max live proxies to keep (default: 40)\n"
        << "  -r, --refresh <sec>   Re-test interval (default: 180; 0 = off)\n"
        << "      --test-url <url>  Scoring URL (default: cloudflare 64KiB)\n"
        << "      --no-cache        Do not load/save ~/.cache/proxchunk/proxies.txt\n"
        << "      --no-tor          Do not auto-add local Tor on 127.0.0.1:9050\n"
        << "      --no-fetch        Do not fetch public proxy lists\n"
        << "      --socks <url>     Extra SOCKS/HTTP proxy (repeatable)\n"
        << "      --proxy-file <f>  Extra proxy list (ip:port or URL per line)\n"
        << "      --no-user-proxies Do not load ~/.config/proxchunk/proxies.txt\n"
        << "      --log-file <path> Log file when daemonized (default: cache dir)\n"
        << "      --debug           Log IPC (ACQUIRE/RELEASE/HELLO) to stderr\n"
        << "  -h, --help            Show this help\n"
        << "  -v, --version         Print version\n";
}

[[nodiscard]] bool
pid_alive(pid_t pid)
{
    if (pid <= 0)
    {
        return false;
    }
    if (::kill(pid, 0) == 0)
    {
        return true;
    }
    return errno != ESRCH;
}

[[nodiscard]] pid_t
read_pidfile(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return 0;
    }
    long p = 0;
    in >> p;
    if (!in || p <= 0)
    {
        return 0;
    }
    return static_cast<pid_t>(p);
}

[[nodiscard]] bool
try_ping(const fs::path& sock)
{
    proxchunk::ProxyClient c;
    if (!c.connect(sock) || !c.hello())
    {
        return false;
    }
    return c.ping();
}

/** 0 = connect succeeded (live listener). Else errno from connect/socket. */
[[nodiscard]] int
unix_connect_errno(const fs::path& sock)
{
    const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (raw < 0)
    {
        return errno;
    }
    unique_fd fd(raw);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = sock.string();
    if (p.size() >= sizeof(addr.sun_path))
    {
        return ENAMETOOLONG;
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
    {
        return 0;
    }
    return errno;
}

[[nodiscard]] bool
pidfile_lock_free(const fs::path& path)
{
    const int raw = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (raw < 0)
    {
        return true;
    }
    unique_fd fd(raw);
    flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fd.get(), F_SETLK, &fl) < 0)
    {
        return false;
    }
    return true;
}

void
block_stop_signals()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGQUIT);
    (void)pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    (void)::signal(SIGPIPE, SIG_IGN);
}

[[nodiscard]] unique_fd
make_signalfd()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGQUIT);
    const int fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    return unique_fd(fd);
}

[[nodiscard]] bool
lock_and_write_pid(unique_fd& pid_fd, const fs::path& path)
{
    const int raw = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (raw < 0)
    {
        std::cerr << "[proxchunkd] cannot open pid file " << path << ": " << std::strerror(errno)
                  << "\n";
        return false;
    }
    pid_fd.reset(raw);
    flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(pid_fd.get(), F_SETLK, &fl) < 0)
    {
        return false;
    }
    if (ftruncate(pid_fd.get(), 0) < 0)
    {
        return false;
    }
    if (lseek(pid_fd.get(), 0, SEEK_SET) < 0)
    {
        return false;
    }
    const std::string s = std::to_string(::getpid()) + "\n";
    std::size_t off = 0;
    while (off < s.size())
    {
        const ssize_t n = ::write(pid_fd.get(), s.data() + off, s.size() - off);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    (void)::fsync(pid_fd.get());
    return true;
}

[[nodiscard]] unique_fd
bind_listen(const fs::path& sock)
{
    const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (raw < 0)
    {
        std::cerr << "[proxchunkd] socket: " << std::strerror(errno) << "\n";
        return unique_fd{};
    }
    unique_fd fd(raw);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = sock.string();
    if (p.size() >= sizeof(addr.sun_path))
    {
        std::cerr << "[proxchunkd] socket path too long\n";
        return unique_fd{};
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        return unique_fd{};
    }
    (void)::chmod(p.c_str(), 0600);
    if (::listen(fd.get(), SOMAXCONN) != 0)
    {
        std::cerr << "[proxchunkd] listen: " << std::strerror(errno) << "\n";
        (void)::unlink(p.c_str());
        return unique_fd{};
    }
    return fd;
}

void
drop_conn(Conn& c, proxchunk::ProxyEngine& engine)
{
    for (const auto& url : c.held)
    {
        engine.release(url, false, 0.0);
    }
    c.held.clear();
    c.fd.reset();
}

void
erase_one_held(Conn& c, std::string_view url)
{
    for (auto it = c.held.begin(); it != c.held.end(); ++it)
    {
        if (*it == url)
        {
            c.held.erase(it);
            return;
        }
    }
}

enum class CmdResult
{
    ok,
    close,
    stop_daemon,
};

void
ipc_dbg(bool debug, std::string_view dir, std::string_view line)
{
    if (!debug)
    {
        return;
    }
    if (line.starts_with("PING") || line == "OK pong")
    {
        return;
    }
    std::cerr << "[proxchunkd] ipc " << dir << " " << line << "\n";
}

CmdResult
handle_line(Conn& c, const std::string& line, proxchunk::ProxyEngine& engine,
            std::atomic<bool>& stopping, bool debug)
{
    auto tok = proxchunk::ipc_split(line);
    if (tok.empty())
    {
        return CmdResult::ok;
    }
    if (!c.greeted)
    {
        if (tok.size() == 2 && tok[0] == "HELLO" && tok[1] == "1")
        {
            c.greeted = true;
            const std::string reply = std::string("OK 1 proxchunkd ") + PROXCHUNK_VERSION;
            ipc_dbg(debug, ">>", reply);
            (void)proxchunk::ipc_write_line(c.fd.get(), reply);
            return CmdResult::ok;
        }
        (void)proxchunk::ipc_write_line(c.fd.get(), "ERR protocol");
        return CmdResult::close;
    }
    if (tok[0] == "PING")
    {
        (void)proxchunk::ipc_write_line(c.fd.get(), "OK pong");
        return CmdResult::ok;
    }
    if (tok[0] == "STATS")
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "OK live=%zu busy=%zu top_mbps=%.4f", engine.live(),
                      engine.busy(), engine.top_mbps());
        (void)proxchunk::ipc_write_line(c.fd.get(), buf);
        return CmdResult::ok;
    }
    if (tok[0] == "ACQUIRE")
    {
        if (stopping.load())
        {
            (void)proxchunk::ipc_write_line(c.fd.get(), "ERR shutting-down");
            return CmdResult::ok;
        }
        std::vector<std::string> skip;
        if (tok.size() >= 2 && tok[1] == "skip")
        {
            for (std::size_t i = 2; i < tok.size(); ++i)
            {
                skip.push_back(tok[i]);
            }
        }
        auto p = engine.acquire(skip);
        if (!p)
        {
            ipc_dbg(debug, ">>", "ERR empty");
            (void)proxchunk::ipc_write_line(c.fd.get(), "ERR empty");
            return CmdResult::ok;
        }
        c.held.push_back(p->address);
        char buf[proxchunk::k_ipc_max_line];
        std::snprintf(buf, sizeof(buf), "OK %s %.4f", p->address.c_str(), p->speed_mbps);
        ipc_dbg(debug, ">>", buf);
        if (!proxchunk::ipc_write_line(c.fd.get(), buf))
        {
            return CmdResult::close;
        }
        return CmdResult::ok;
    }
    if (tok[0] == "RELEASE")
    {
        if (tok.size() != 4 || (tok[2] != "ok" && tok[2] != "fail"))
        {
            (void)proxchunk::ipc_write_line(c.fd.get(), "ERR protocol");
            return CmdResult::close;
        }
        double mbps = 0.0;
        try
        {
            mbps = std::stod(tok[3]);
        }
        catch (...)
        {
            (void)proxchunk::ipc_write_line(c.fd.get(), "ERR protocol");
            return CmdResult::close;
        }
        engine.release(tok[1], tok[2] == "ok", mbps);
        erase_one_held(c, tok[1]);
        ipc_dbg(debug, ">>", "OK");
        (void)proxchunk::ipc_write_line(c.fd.get(), "OK");
        return CmdResult::ok;
    }
    if (tok[0] == "STOP")
    {
        (void)proxchunk::ipc_write_line(c.fd.get(), "OK shutting-down");
        return CmdResult::stop_daemon;
    }
    (void)proxchunk::ipc_write_line(c.fd.get(), "ERR unknown");
    return CmdResult::ok;
}

void
load_extra_lists(Options& opt)
{
    auto load_list = [&](const fs::path& p) {
        auto rows = proxchunk::load_proxy_file(p.string());
        if (!rows.empty())
        {
            std::cerr << "[proxchunkd] Loaded " << rows.size() << " user proxies from " << p
                      << "\n";
            opt.extra_proxies.insert(opt.extra_proxies.end(), rows.begin(), rows.end());
        }
    };
    if (opt.use_user_list)
    {
        load_list(proxchunk::default_user_proxy_list_path());
    }
    for (const auto& pf : opt.proxy_files)
    {
        load_list(pf);
        if (!fs::exists(pf))
        {
            std::cerr << "[proxchunkd] --proxy-file not found: " << pf << "\n";
        }
    }
}

int
run_server(Options opt, int ready_fd)
{
    unique_fd ready(ready_fd);
    const fs::path sock_dir = opt.socket_path.parent_path();
    if (!sock_dir.empty() && !proxchunk::ensure_runtime_dir(sock_dir))
    {
        std::cerr << "[proxchunkd] cannot create runtime dir " << sock_dir << "\n";
        if (ready)
        {
            (void)::write(ready.get(), "E", 1);
        }
        return 1;
    }

    unique_fd pid_fd;
    if (!lock_and_write_pid(pid_fd, opt.pid_path))
    {
        if (try_ping(opt.socket_path))
        {
            std::cerr << "already running\n";
            if (ready)
            {
                (void)::write(ready.get(), "R", 1);
            }
            return 0;
        }
        std::cerr << "[proxchunkd] pid file lock failed (" << opt.pid_path << ")\n";
        if (ready)
        {
            (void)::write(ready.get(), "E", 1);
        }
        return 1;
    }

    unique_fd listen_fd = bind_listen(opt.socket_path);
    if (!listen_fd)
    {
        if (errno == EADDRINUSE && try_ping(opt.socket_path))
        {
            std::cerr << "already running\n";
            if (ready)
            {
                (void)::write(ready.get(), "R", 1);
            }
            return 0;
        }
        std::cerr << "[proxchunkd] bind " << opt.socket_path << ": " << std::strerror(errno)
                  << "\n";
        pid_fd.reset();
        (void)::unlink(opt.pid_path.c_str());
        if (ready)
        {
            (void)::write(ready.get(), "E", 1);
        }
        return 1;
    }

    unique_fd sig_fd = make_signalfd();
    if (!sig_fd)
    {
        std::cerr << "[proxchunkd] signalfd: " << std::strerror(errno) << "\n";
        listen_fd.reset();
        (void)::unlink(opt.socket_path.c_str());
        pid_fd.reset();
        (void)::unlink(opt.pid_path.c_str());
        if (ready)
        {
            (void)::write(ready.get(), "E", 1);
        }
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
    {
        std::cerr << "[proxchunkd] curl_global_init failed\n";
        listen_fd.reset();
        (void)::unlink(opt.socket_path.c_str());
        pid_fd.reset();
        (void)::unlink(opt.pid_path.c_str());
        if (ready)
        {
            (void)::write(ready.get(), "E", 1);
        }
        return 1;
    }
    bool curl_inited = true;

    load_extra_lists(opt);

    proxchunk::ProxyEngineConfig cfg;
    cfg.max_keep = static_cast<std::size_t>(opt.max_proxies);
    cfg.refresh_sec = opt.refresh_sec;
    cfg.test_url = opt.test_url;
    cfg.cache_path = proxchunk::default_proxy_cache_path();
    cfg.use_cache = opt.use_cache;
    cfg.use_tor = opt.use_tor;
    cfg.fetch_public = opt.fetch_public;
    cfg.extra_proxies = std::move(opt.extra_proxies);
    cfg.debug = opt.debug;

    proxchunk::ProxyEngine engine(std::move(cfg));
    FgBar bar;
    bar.use_bar = opt.foreground && isatty(STDOUT_FILENO);
    proxchunk::ProxyProgressFn progress;
    if (bar.use_bar)
    {
        progress = [&bar](std::size_t completed, std::size_t total, std::size_t live,
                          std::size_t curl_ok, bool done) {
            bar.draw(completed, total, live, curl_ok, done);
        };
    }
    engine.start(std::move(progress));

    if (ready)
    {
        const char r = 'R';
        (void)::write(ready.get(), &r, 1);
        ready.reset();
    }

    std::vector<Conn> conns;
    std::atomic<bool> stopping{false};

    auto graceful_stop = [&]() {
        if (stopping.exchange(true))
        {
            return;
        }
        listen_fd.reset();
        (void)::unlink(opt.socket_path.c_str());
        for (auto& c : conns)
        {
            if (c.fd)
            {
                (void)proxchunk::ipc_write_line(c.fd.get(), "ERR shutting-down");
                drop_conn(c, engine);
            }
        }
        conns.clear();
        engine.stop();
        if (curl_inited)
        {
            curl_global_cleanup();
            curl_inited = false;
        }
        sig_fd.reset();
        /* Unlink pid only if this fd still owns that path (no replacement daemon). */
        if (pid_fd)
        {
            struct stat st_fd{};
            struct stat st_path{};
            const bool ours = ::fstat(pid_fd.get(), &st_fd) == 0
                              && ::stat(opt.pid_path.c_str(), &st_path) == 0
                              && st_fd.st_dev == st_path.st_dev
                              && st_fd.st_ino == st_path.st_ino;
            pid_fd.reset();
            if (ours)
            {
                (void)::unlink(opt.pid_path.c_str());
            }
        }
    };

    bool run = true;
    while (run)
    {
        std::vector<pollfd> pfds;
        pfds.reserve(conns.size() + 2);
        const std::size_t listen_ix = pfds.size();
        if (listen_fd)
        {
            pfds.push_back(pollfd{listen_fd.get(), POLLIN, 0});
        }
        const std::size_t sig_ix = pfds.size();
        if (sig_fd)
        {
            pfds.push_back(pollfd{sig_fd.get(), POLLIN, 0});
        }
        std::vector<std::size_t> conn_ix(conns.size(), static_cast<std::size_t>(-1));
        for (std::size_t i = 0; i < conns.size(); ++i)
        {
            if (!conns[i].fd)
            {
                continue;
            }
            conn_ix[i] = pfds.size();
            pfds.push_back(pollfd{conns[i].fd.get(), POLLIN, 0});
        }
        const int pr = poll(pfds.data(), pfds.size(), 500);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        bool want_stop = false;
        if (sig_fd && sig_ix < pfds.size() && (pfds[sig_ix].revents & POLLIN) != 0)
        {
            signalfd_siginfo info{};
            const ssize_t n = ::read(sig_fd.get(), &info, sizeof(info));
            (void)n;
            want_stop = true;
        }

        if (listen_fd && listen_ix < pfds.size() && (pfds[listen_ix].revents & POLLIN) != 0)
        {
            for (;;)
            {
                const int cfd = accept4(listen_fd.get(), nullptr, nullptr,
                                        SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        break;
                    }
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    break;
                }
                Conn nc;
                nc.fd.reset(cfd);
                {
                    int yes = 1;
                    (void)::setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                }
                conns.push_back(std::move(nc));
            }
        }

        for (std::size_t i = 0; i < conn_ix.size(); ++i)
        {
            if (!conns[i].fd || conn_ix[i] == static_cast<std::size_t>(-1))
            {
                continue;
            }
            const short re = pfds[conn_ix[i]].revents;
            if ((re & (POLLERR | POLLNVAL)) != 0)
            {
                drop_conn(conns[i], engine);
                continue;
            }
            if ((re & POLLIN) == 0 && (re & POLLHUP) != 0)
            {
                drop_conn(conns[i], engine);
                continue;
            }
            if ((re & POLLIN) == 0)
            {
                continue;
            }
            for (;;)
            {
                std::string line;
                const auto st =
                    proxchunk::ipc_read_line(conns[i].fd.get(), conns[i].buf, line, 0);
                if (st == proxchunk::ipc_read_result::again)
                {
                    break;
                }
                if (st == proxchunk::ipc_read_result::eof
                    || st == proxchunk::ipc_read_result::error
                    || st == proxchunk::ipc_read_result::overflow)
                {
                    drop_conn(conns[i], engine);
                    break;
                }
                ipc_dbg(opt.debug, "<<", line);
                const CmdResult cr = handle_line(conns[i], line, engine, stopping, opt.debug);
                if (cr == CmdResult::close)
                {
                    drop_conn(conns[i], engine);
                    break;
                }
                if (cr == CmdResult::stop_daemon)
                {
                    want_stop = true;
                    break;
                }
            }
        }

        conns.erase(std::remove_if(conns.begin(), conns.end(),
                                   [](const Conn& c) { return !c.fd; }),
                    conns.end());

        if (want_stop)
        {
            graceful_stop();
            run = false;
        }
    }

    if (!stopping.load())
    {
        graceful_stop();
    }
    return 0;
}

void
redirect_stdio(const fs::path& log_path)
{
    std::error_code ec;
    if (!log_path.parent_path().empty())
    {
        fs::create_directories(log_path.parent_path(), ec);
    }
    const int nullfd = ::open("/dev/null", O_RDWR);
    if (nullfd >= 0)
    {
        (void)::dup2(nullfd, STDIN_FILENO);
        if (nullfd > 2)
        {
            (void)::close(nullfd);
        }
    }
    const int logfd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0)
    {
        (void)::dup2(logfd, STDOUT_FILENO);
        (void)::dup2(logfd, STDERR_FILENO);
        if (logfd > 2)
        {
            (void)::close(logfd);
        }
    }
    (void)::setvbuf(stdout, nullptr, _IOLBF, 0);
    (void)::setvbuf(stderr, nullptr, _IOLBF, 0);
}

int
daemonize_and_run(Options opt)
{
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0)
    {
        std::cerr << "[proxchunkd] pipe2: " << std::strerror(errno) << "\n";
        return 1;
    }
    unique_fd pread(pipefd[0]);
    unique_fd pwrite(pipefd[1]);

    const pid_t child1 = ::fork();
    if (child1 < 0)
    {
        std::cerr << "[proxchunkd] fork: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (child1 == 0)
    {
        pread.reset();
        if (::setsid() < 0)
        {
            _exit(1);
        }
        const pid_t child2 = ::fork();
        if (child2 < 0)
        {
            _exit(1);
        }
        if (child2 > 0)
        {
            _exit(0);
        }
        (void)::chdir("/");
        (void)::umask(022);
        redirect_stdio(opt.log_path);
        const int wr = pwrite.release();
        const int rc = run_server(std::move(opt), wr);
        _exit(rc == 0 ? 0 : 1);
    }

    pwrite.reset();
    int st = 0;
    (void)::waitpid(child1, &st, 0);
    char buf[256];
    const ssize_t n = ::read(pread.get(), buf, sizeof(buf));
    if (n >= 1 && buf[0] == 'R')
    {
        return 0;
    }
    if (n > 1)
    {
        (void)::write(STDERR_FILENO, buf + 1, static_cast<std::size_t>(n - 1));
    }
    std::cerr << "[proxchunkd] daemon failed to start\n";
    return 1;
}

int
cmd_status(const fs::path& sock)
{
    proxchunk::ProxyClient c;
    if (!c.connect(sock) || !c.hello())
    {
        std::cerr << "not running\n";
        return 1;
    }
    auto s = c.stats();
    if (!s)
    {
        std::cerr << "not running\n";
        return 1;
    }
    std::printf("live=%zu busy=%zu top_mbps=%.4f\n", s->live, s->busy, s->top_mbps);
    return 0;
}

int
cmd_stop(const fs::path& sock, const fs::path& pid_path)
{
    bool sent_stop = false;
    {
        proxchunk::ProxyClient c;
        if (c.connect(sock) && c.hello() && c.stop())
        {
            sent_stop = true;
        }
    }

    pid_t pid = read_pidfile(pid_path);
    auto wait_gone = [&](int timeout_ms) {
        const int steps = std::max(timeout_ms / 50, 1);
        for (int i = 0; i < steps; ++i)
        {
            if (pid > 0 && !pid_alive(pid))
            {
                return true;
            }
            if (pid <= 0 && !try_ping(sock))
            {
                return true;
            }
            usleep(50 * 1000);
        }
        return pid > 0 ? !pid_alive(pid) : !try_ping(sock);
    };

    if (sent_stop)
    {
        if (wait_gone(15000))
        {
            (void)::unlink(sock.c_str());
            (void)::unlink(pid_path.c_str());
            return 0;
        }
        std::cerr << "proxchunkd still running; not removing socket/pid\n";
        return 1;
    }

    /* No live protocol: do not SIGTERM a pidfile number (PID reuse). */
    if (pidfile_lock_free(pid_path))
    {
        const int ce = unix_connect_errno(sock);
        if (ce == ECONNREFUSED || ce == ENOENT)
        {
            (void)::unlink(sock.c_str());
        }
        (void)::unlink(pid_path.c_str());
        std::cerr << "not running\n";
        return 0;
    }
    std::cerr << "proxchunkd still running; not removing socket/pid\n";
    return 1;
}

void
recover_stale(const fs::path& sock, const fs::path& pid_path)
{
    const int ce = unix_connect_errno(sock);
    if (ce == 0)
    {
        return;
    }
    if (ce == ECONNREFUSED || ce == ENOENT)
    {
        (void)::unlink(sock.c_str());
    }
    if (pidfile_lock_free(pid_path))
    {
        (void)::unlink(pid_path.c_str());
    }
}

int
parse_args(int argc, char** argv, Options& opt)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "-h" || a == "--help")
        {
            usage(argv[0]);
            std::exit(0);
        }
        if (a == "-v" || a == "--version")
        {
            std::cout << "proxchunkd " << PROXCHUNK_VERSION << "\n";
            std::exit(0);
        }
    }

    auto need = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc)
        {
            std::cerr << name << " requires an argument\n";
            std::exit(1);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--foreground")
        {
            opt.foreground = true;
        }
        else if (a == "--stop")
        {
            opt.do_stop = true;
        }
        else if (a == "--status")
        {
            opt.do_status = true;
        }
        else if (a == "--socket")
        {
            opt.socket_path = need(i, "--socket");
        }
        else if (a == "--pid-file")
        {
            opt.pid_path = need(i, "--pid-file");
        }
        else if (a == "-p" || a == "--proxies")
        {
            opt.max_proxies = std::atoi(need(i, "-p"));
        }
        else if (a == "-r" || a == "--refresh")
        {
            opt.refresh_sec = std::atoi(need(i, "-r"));
        }
        else if (a == "--test-url")
        {
            opt.test_url = need(i, "--test-url");
        }
        else if (a == "--no-cache")
        {
            opt.use_cache = false;
        }
        else if (a == "--no-tor")
        {
            opt.use_tor = false;
        }
        else if (a == "--no-fetch")
        {
            opt.fetch_public = false;
        }
        else if (a == "--socks")
        {
            std::string u = proxchunk::normalize_proxy_line(need(i, "--socks"));
            if (!u.empty())
            {
                opt.extra_proxies.push_back(std::move(u));
            }
        }
        else if (a == "--proxy-file")
        {
            opt.proxy_files.emplace_back(need(i, "--proxy-file"));
        }
        else if (a == "--no-user-proxies")
        {
            opt.use_user_list = false;
        }
        else if (a == "--log-file")
        {
            opt.log_path = need(i, "--log-file");
        }
        else if (a == "--debug")
        {
            opt.debug = true;
        }
        else
        {
            std::cerr << "Unknown option " << a << "\n";
            usage(argv[0]);
            return 1;
        }
    }
    return 0;
}

} // namespace

int
main(int argc, char** argv)
{
    Options opt;
    if (parse_args(argc, argv, opt) != 0)
    {
        return 1;
    }
    if (opt.max_proxies < 1)
    {
        std::cerr << "proxies must be >= 1\n";
        return 1;
    }
    if (opt.refresh_sec < 0)
    {
        std::cerr << "refresh must be >= 0 (0 = no background refresh)\n";
        return 1;
    }

    if (opt.socket_path.empty())
    {
        opt.socket_path = proxchunk::default_socket_path();
    }
    if (opt.pid_path.empty())
    {
        opt.pid_path = opt.socket_path.parent_path() / "proxchunkd.pid";
        if (opt.pid_path.parent_path().empty())
        {
            opt.pid_path = "proxchunkd.pid";
        }
    }
    if (opt.log_path.empty())
    {
        opt.log_path = proxchunk::default_log_path();
    }

    if (opt.do_stop)
    {
        return cmd_stop(opt.socket_path, opt.pid_path);
    }
    if (opt.do_status)
    {
        return cmd_status(opt.socket_path);
    }

    block_stop_signals();

    if (try_ping(opt.socket_path))
    {
        std::cerr << "already running\n";
        return 0;
    }
    recover_stale(opt.socket_path, opt.pid_path);
    if (unix_connect_errno(opt.socket_path) == 0)
    {
        std::cerr << "already running\n";
        return 0;
    }

    for (auto& pf : opt.proxy_files)
    {
        std::error_code ec;
        fs::path abs = fs::absolute(pf, ec);
        if (!ec)
        {
            pf = std::move(abs);
        }
    }

    if (opt.foreground)
    {
        return run_server(std::move(opt), -1);
    }
    return daemonize_and_run(std::move(opt));
}
