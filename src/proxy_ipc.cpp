/*
 * UNIX-socket line protocol and ProxyClient.
 */

#include "proxchunk/proxy_ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <system_error>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace proxchunk {
namespace {

[[nodiscard]] bool
deadline_passed(std::chrono::steady_clock::time_point deadline)
{
    return std::chrono::steady_clock::now() >= deadline;
}

[[nodiscard]] int
remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
    {
        return 0;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

[[nodiscard]] bool
extract_line(std::string& leftover, std::string& line)
{
    const auto pos = leftover.find('\n');
    if (pos == std::string::npos)
    {
        return false;
    }
    line = leftover.substr(0, pos);
    leftover.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    return true;
}

void
enable_keepalive(int fd)
{
    int yes = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
}

} // namespace

bool
ipc_write_line(int fd, std::string_view line, int timeout_ms)
{
    if (fd < 0)
    {
        return false;
    }
    if (line.size() > k_ipc_max_line - 1)
    {
        return false;
    }
    std::string buf;
    buf.reserve(line.size() + 1);
    buf.append(line);
    if (buf.empty() || buf.back() != '\n')
    {
        buf.push_back('\n');
    }
    if (buf.size() > k_ipc_max_line)
    {
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    std::size_t off = 0;
    while (off < buf.size())
    {
        const ssize_t n = send(fd, buf.data() + off, buf.size() - off, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (timeout_ms == 0 || deadline_passed(deadline))
                {
                    return false;
                }
                pollfd p{};
                p.fd = fd;
                p.events = POLLOUT;
                const int pr = poll(&p, 1, remaining_ms(deadline));
                if (pr <= 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

ipc_read_result
ipc_read_line(int fd, std::string& leftover, std::string& line, int timeout_ms)
{
    line.clear();
    if (extract_line(leftover, line))
    {
        if (line.size() > k_ipc_max_line - 1)
        {
            return ipc_read_result::overflow;
        }
        return ipc_read_result::line;
    }
    if (leftover.size() >= k_ipc_max_line)
    {
        return ipc_read_result::overflow;
    }
    if (fd < 0)
    {
        return ipc_read_result::error;
    }

    const bool blocking = timeout_ms != 0;
    const auto deadline =
        (timeout_ms > 0)
            ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
            : std::chrono::steady_clock::time_point::max();

    for (;;)
    {
        if (blocking && timeout_ms > 0 && deadline_passed(deadline))
        {
            return ipc_read_result::again;
        }
        if (blocking)
        {
            pollfd p{};
            p.fd = fd;
            p.events = POLLIN;
            const int wait_ms = (timeout_ms < 0) ? -1 : remaining_ms(deadline);
            const int pr = poll(&p, 1, wait_ms);
            if (pr == 0)
            {
                return ipc_read_result::again;
            }
            if (pr < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return ipc_read_result::error;
            }
            if ((p.revents & (POLLERR | POLLNVAL)) != 0)
            {
                return ipc_read_result::error;
            }
            if ((p.revents & POLLIN) == 0 && (p.revents & POLLHUP) != 0)
            {
                return leftover.empty() ? ipc_read_result::eof : ipc_read_result::error;
            }
        }

        char tmp[256];
        const ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return ipc_read_result::again;
            }
            return ipc_read_result::error;
        }
        if (n == 0)
        {
            return leftover.empty() ? ipc_read_result::eof : ipc_read_result::error;
        }
        leftover.append(tmp, static_cast<std::size_t>(n));
        if (extract_line(leftover, line))
        {
            if (line.size() > k_ipc_max_line - 1)
            {
                return ipc_read_result::overflow;
            }
            return ipc_read_result::line;
        }
        if (leftover.size() >= k_ipc_max_line)
        {
            return ipc_read_result::overflow;
        }
        if (!blocking)
        {
            return ipc_read_result::again;
        }
    }
}

std::filesystem::path
default_runtime_dir()
{
    const uid_t uid = getuid();
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg != nullptr && xdg[0] != '\0')
    {
        std::error_code ec;
        if (std::filesystem::is_directory(xdg, ec))
        {
            return std::filesystem::path(xdg) / "proxchunk";
        }
    }
    {
        std::filesystem::path run = std::filesystem::path("/run/user") / std::to_string(uid);
        std::error_code ec;
        if (std::filesystem::is_directory(run, ec))
        {
            return run / "proxchunk";
        }
    }
    return std::filesystem::path("/tmp") / ("proxchunk-" + std::to_string(uid));
}

std::filesystem::path
default_socket_path()
{
    return default_runtime_dir() / "proxchunkd.sock";
}

std::filesystem::path
default_pid_path()
{
    return default_runtime_dir() / "proxchunkd.pid";
}

std::filesystem::path
default_log_path()
{
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0')
    {
        return std::filesystem::path(xdg) / "proxchunk" / "proxchunkd.log";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path(home) / ".cache" / "proxchunk" / "proxchunkd.log";
    }
    return std::filesystem::path("proxchunkd.log");
}

bool
ensure_runtime_dir(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        return false;
    }
    if (::chmod(dir.c_str(), 0700) != 0)
    {
        return false;
    }
    return true;
}

void
ProxyClient::stop_keepalive()
{
    if (keepalive_.joinable())
    {
        keepalive_.request_stop();
        keepalive_.join();
    }
}

void
ProxyClient::start_keepalive()
{
    stop_keepalive();
    keepalive_ = std::jthread([this](std::stop_token st) {
        while (!st.stop_requested())
        {
            const int slice = 100;
            int slept = 0;
            while (slept < k_keepalive_ms && !st.stop_requested())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                slept += slice;
            }
            if (st.stop_requested())
            {
                break;
            }
            std::unique_lock lk(mu_, std::try_to_lock);
            if (!lk || !fd_)
            {
                continue;
            }
            (void)ipc_write_line(fd_.get(), "PING", 2000);
            std::string reply;
            (void)ipc_read_line(fd_.get(), leftover_, reply, 2000);
        }
    });
}

ProxyClient::~ProxyClient()
{
    stop_keepalive();
    std::lock_guard g(mu_);
    if (fd_)
    {
        for (const auto& url : held_)
        {
            std::string cmd = "RELEASE ";
            cmd += url;
            cmd += " fail 0";
            (void)ipc_write_line(fd_.get(), cmd);
            std::string reply;
            leftover_.clear();
            (void)ipc_read_line(fd_.get(), leftover_, reply, 500);
        }
        held_.clear();
    }
    fd_.reset();
}

bool
ProxyClient::connect(const std::filesystem::path& socket_path)
{
    close();
    socket_path_ = socket_path;
    const int raw = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (raw < 0)
    {
        return false;
    }
    unique_fd sock(raw);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string p = socket_path.string();
    if (p.size() >= sizeof(addr.sun_path))
    {
        return false;
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);
    if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        return false;
    }
    leftover_.clear();
    enable_keepalive(sock.get());
    fd_ = std::move(sock);
    return true;
}

void
ProxyClient::close()
{
    leftover_.clear();
    fd_.reset();
}

bool
ProxyClient::rpc(std::string_view cmd, std::string& reply)
{
    reply.clear();
    if (!fd_ && !reconnect())
    {
        return false;
    }
    auto once = [&]() -> bool {
        if (!ipc_write_line(fd_.get(), cmd))
        {
            return false;
        }
        const ipc_read_result st =
            ipc_read_line(fd_.get(), leftover_, reply, k_ipc_rpc_timeout_ms);
        return st == ipc_read_result::line;
    };
    if (once())
    {
        return true;
    }
    /* Reconnect would HUP the daemon and RELEASE every held proxy. */
    if (!held_.empty())
    {
        return false;
    }
    if (!reconnect())
    {
        return false;
    }
    return once();
}

bool
ProxyClient::reconnect()
{
    leftover_.clear();
    fd_.reset();
    held_.clear();
    if (socket_path_.empty())
    {
        return false;
    }
    return connect(socket_path_) && hello();
}

bool
ProxyClient::hello()
{
    if (!fd_)
    {
        return false;
    }
    if (!ipc_write_line(fd_.get(), "HELLO 1"))
    {
        return false;
    }
    std::string reply;
    const ipc_read_result st = ipc_read_line(fd_.get(), leftover_, reply, 5000);
    if (st != ipc_read_result::line)
    {
        return false;
    }
    if (!reply.starts_with("OK 1"))
    {
        return false;
    }
    start_keepalive();
    return true;
}

int
ProxyClient::set_target(std::string_view url)
{
    std::lock_guard g(mu_);
    std::string cmd = "TARGET ";
    cmd += url;
    std::string reply;
    if (!rpc(cmd, reply) || !reply.starts_with("OK "))
    {
        return -1;
    }
    try
    {
        return std::stoi(reply.substr(3));
    }
    catch (...)
    {
        return -1;
    }
}

bool
ProxyClient::ping()
{
    std::lock_guard g(mu_);
    std::string reply;
    if (!rpc("PING", reply))
    {
        return false;
    }
    return reply == "OK pong";
}

std::optional<acquired_proxy>
ProxyClient::acquire(const std::vector<std::string>& skip)
{
    /* Mutex is held only for the RPC so RELEASE from other workers is not blocked. */
    std::string cmd = "ACQUIRE";
    if (!skip.empty())
    {
        cmd += " skip";
        for (const auto& u : skip)
        {
            cmd += ' ';
            cmd += u;
            if (cmd.size() > k_ipc_max_line - 64)
            {
                break;
            }
        }
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(k_acquire_wait_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string reply;
        bool io_ok = false;
        {
            std::lock_guard g(mu_);
            io_ok = rpc(cmd, reply);
            if (!io_ok)
            {
                (void)reconnect();
            }
            else
            {
                auto tok = ipc_split(reply);
                if (tok.size() >= 3 && tok[0] == "OK")
                {
                    acquired_proxy a;
                    a.url = tok[1];
                    try
                    {
                        a.mbps = std::stod(tok[2]);
                    }
                    catch (...)
                    {
                        a.mbps = 0.0;
                    }
                    held_.push_back(a.url);
                    return a;
                }
                if (reply.find("shutting-down") != std::string::npos)
                {
                    return std::nullopt;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return std::nullopt;
}

bool
ProxyClient::release(std::string_view url, bool success, double mbps)
{
    std::lock_guard g(mu_);
    std::ostringstream os;
    os << "RELEASE " << url << (success ? " ok " : " fail ") << mbps;
    std::string reply;
    const bool ok = rpc(os.str(), reply) && reply.starts_with("OK");
    auto it = std::find(held_.begin(), held_.end(), url);
    if (it != held_.end())
    {
        held_.erase(it);
    }
    return ok;
}

std::optional<proxy_stats>
ProxyClient::stats()
{
    std::lock_guard g(mu_);
    std::string reply;
    if (!rpc("STATS", reply))
    {
        return std::nullopt;
    }
    auto tok = ipc_split(reply);
    if (tok.empty() || tok[0] != "OK")
    {
        return std::nullopt;
    }
    proxy_stats s;
    for (std::size_t i = 1; i < tok.size(); ++i)
    {
        const auto eq = tok[i].find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        const std::string key = tok[i].substr(0, eq);
        const std::string val = tok[i].substr(eq + 1);
        try
        {
            if (key == "live")
            {
                s.live = static_cast<std::size_t>(std::stoul(val));
            }
            else if (key == "busy")
            {
                s.busy = static_cast<std::size_t>(std::stoul(val));
            }
            else if (key == "top_mbps")
            {
                s.top_mbps = std::stod(val);
            }
        }
        catch (...)
        {
        }
    }
    return s;
}

bool
ProxyClient::stop()
{
    std::lock_guard g(mu_);
    std::string reply;
    if (!rpc("STOP", reply))
    {
        return false;
    }
    return reply.find("shutting-down") != std::string::npos;
}

bool
ProxyClient::spawn_daemon()
{
    std::string bin;
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
    {
        exe[static_cast<std::size_t>(n)] = '\0';
        std::filesystem::path cand = std::filesystem::path(exe).parent_path() / "proxchunkd";
        if (::access(cand.c_str(), X_OK) == 0)
        {
            bin = cand.string();
        }
    }
    const bool use_path = bin.empty();
    if (use_path)
    {
        bin = "proxchunkd";
    }

    std::vector<std::string> args;
    args.push_back(bin);
    args.push_back("--socket");
    args.push_back(socket_path_.string());
    args.insert(args.end(), daemon_extra_.begin(), daemon_extra_.end());

    std::vector<char*> av;
    av.reserve(args.size() + 1);
    for (auto& s : args)
    {
        av.push_back(s.data());
    }
    av.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid == 0)
    {
        if (use_path)
        {
            ::execvp(bin.c_str(), av.data());
        }
        else
        {
            ::execv(bin.c_str(), av.data());
        }
        _exit(127);
    }
    int st = 0;
    if (::waitpid(pid, &st, 0) < 0)
    {
        return false;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool
ProxyClient::connect_or_start(const std::filesystem::path& socket_path,
                              const std::vector<std::string>& daemon_extra, int timeout_ms)
{
    std::lock_guard g(mu_);
    socket_path_ = socket_path;
    daemon_extra_ = daemon_extra;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    int spawns = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (connect(socket_path_) && hello())
        {
            std::string reply;
            leftover_.clear();
            if (ipc_write_line(fd_.get(), "PING")
                && ipc_read_line(fd_.get(), leftover_, reply, 2000) == ipc_read_result::line
                && reply == "OK pong")
            {
                return true;
            }
        }
        close();
        if (spawns < 2)
        {
            (void)spawn_daemon();
            ++spawns;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

} // namespace proxchunk
