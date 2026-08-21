/**
 * @file proxy_ipc.hpp
 * @brief Line protocol, UNIX-socket helpers, and @ref ProxyClient.
 */

#ifndef PROXCHUNK_PROXY_IPC_HPP
#define PROXCHUNK_PROXY_IPC_HPP

#include "proxchunk/unique_fd.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace proxchunk {

/** Maximum command/response line including the terminating newline. */
inline constexpr std::size_t k_ipc_max_line = 4096;

/** Protocol version spoken by HELLO. */
inline constexpr int k_ipc_proto_version = 1;

/** Cap for one @ref ipc_write_line (short writes + POLLOUT). */
inline constexpr int k_ipc_write_timeout_ms = 2000;

/** Client-side ACQUIRE retry budget. */
inline constexpr int k_acquire_wait_ms = 30000;

/** Auto-start wait for PING after spawning proxchunkd. */
inline constexpr int k_autostart_wait_ms = 45000;

/** Application PING interval on the persistent UNIX socket. */
inline constexpr int k_keepalive_ms = 5000;

/** RPC reply wait (must cover a busy daemon; do not reconnect on a slow PING). */
inline constexpr int k_ipc_rpc_timeout_ms = 30000;

/**
 * @brief Result of @ref ipc_read_line.
 */
enum class ipc_read_result
{
    line,     ///< Complete line stored in @p line (newline stripped).
    again,    ///< No complete line yet (EAGAIN / poll timeout).
    eof,      ///< Peer closed.
    error,    ///< Hard I/O error.
    overflow, ///< More than @ref k_ipc_max_line without a newline.
};

/**
 * @brief Write @p line plus a trailing newline with a short-write loop.
 *
 * Uses @c send(..., MSG_NOSIGNAL). @c EAGAIN waits on POLLOUT up to
 * @p timeout_ms total.
 *
 * @param[in] fd         Connected SOCK_STREAM.
 * @param[in] line       Payload without a required newline.
 * @param[in] timeout_ms Deadline for the whole write (default 2 s).
 * @retval true  Entire line including @c \\n was sent.
 * @retval false Timeout, hangup, or error — treat as disconnect.
 */
[[nodiscard]] bool ipc_write_line(int fd, std::string_view line,
                                  int timeout_ms = k_ipc_write_timeout_ms);

/**
 * @brief Read one newline-terminated line into @p line.
 *
 * Accumulates in @p leftover across calls. Strips a trailing @c \\r.
 * @p timeout_ms == 0 is non-blocking; &lt; 0 blocks; &gt; 0 is a cap.
 *
 * @param[in]     fd         Socket.
 * @param[in,out] leftover   Per-connection read buffer.
 * @param[out]    line       Filled on @ref ipc_read_result::line.
 * @param[in]     timeout_ms See above.
 */
[[nodiscard]] ipc_read_result ipc_read_line(int fd, std::string& leftover, std::string& line,
                                            int timeout_ms);

/**
 * @brief Split @p line on ASCII spaces (no quoting).
 */
[[nodiscard]] inline std::vector<std::string>
ipc_split(std::string_view line)
{
    std::vector<std::string> tok;
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && line[i] == ' ')
        {
            ++i;
        }
        if (i >= line.size())
        {
            break;
        }
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ')
        {
            ++j;
        }
        tok.emplace_back(line.substr(i, j - i));
        i = j;
    }
    return tok;
}

/**
 * @brief Runtime dir: `$XDG_RUNTIME_DIR/proxchunk`, else `/run/user/UID/proxchunk`,
 *        else `/tmp/proxchunk-UID`.
 */
[[nodiscard]] std::filesystem::path default_runtime_dir();

/** @return `$runtime_dir/proxchunkd.sock`. */
[[nodiscard]] std::filesystem::path default_socket_path();

/** @return `$runtime_dir/proxchunkd.pid`. */
[[nodiscard]] std::filesystem::path default_pid_path();

/** @return `$XDG_CACHE_HOME/proxchunk/proxchunkd.log` (or `~/.cache/...`). */
[[nodiscard]] std::filesystem::path default_log_path();

/**
 * @brief `mkdir -p` @p dir and `chmod 0700` even if it already existed.
 * @retval true Directory is usable.
 */
[[nodiscard]] bool ensure_runtime_dir(const std::filesystem::path& dir);

/**
 * @brief STATS payload.
 */
struct proxy_stats
{
    std::size_t live = 0;
    std::size_t busy = 0;
    double      top_mbps = 0.0;
};

/**
 * @brief One successful ACQUIRE.
 */
struct acquired_proxy
{
    std::string url;
    double      mbps = 0.0;
};

/**
 * @brief Persistent UNIX-socket client (one connection per process).
 *
 * One SOCK_STREAM for the whole download. A keepalive thread sends
 * `PING` every @ref k_keepalive_ms so the daemon never treats the
 * client as dead (disconnect would RELEASE every held proxy).
 *
 * Each RPC takes an internal mutex so worker threads may share one
 * instance. ACQUIRE retries locally for up to @ref k_acquire_wait_ms;
 * the daemon itself does not block.
 *
 * The destructor RELEASE-fails any outstanding leases.
 */
class ProxyClient
{
public:
    ProxyClient() = default;
    ~ProxyClient();

    ProxyClient(const ProxyClient&) = delete;
    ProxyClient& operator=(const ProxyClient&) = delete;

    /**
     * @brief Connect to @p socket_path (no HELLO).
     * @retval false Connect failed; previous fd is closed.
     */
    [[nodiscard]] bool connect(const std::filesystem::path& socket_path);

    /**
     * @brief Send `HELLO 1` and expect `OK 1 …`.
     */
    [[nodiscard]] bool hello();

    /**
     * @brief `PING` → `OK pong`.
     */
    [[nodiscard]] bool ping();

    /**
     * @brief `TARGET <url>` — daemon Range-tests the pool against this file.
     * @return How many proxies reached the target, or -1 on IPC error.
     */
    [[nodiscard]] int set_target(std::string_view url);

    /**
     * @brief Retry ACQUIRE for up to 30 s (short sleeps on `ERR empty`).
     *
     * @param[in] skip Proxy URLs already tried for this chunk; the daemon
     *            leases the next fastest row not in this list when possible.
     */
    /**
     * @param[in] wait_ms 0 = one RPC (no wait). &gt;0 retry on empty.
     */
    [[nodiscard]] std::optional<acquired_proxy> acquire(
        const std::vector<std::string>& skip = {}, int wait_ms = 0);

    /**
     * @brief `RELEASE <url> ok|fail <mbps>`.
     */
    [[nodiscard]] bool release(std::string_view url, bool success, double mbps);

    /**
     * @brief `STATS`.
     */
    [[nodiscard]] std::optional<proxy_stats> stats();

    /**
     * @brief `STOP` (asks the daemon to shut down).
     */
    [[nodiscard]] bool stop();

    /**
     * @brief PING loop; spawn `dirname(/proc/self/exe)/proxchunkd` then PATH.
     *
     * @param[in] socket_path UNIX socket the daemon should bind.
     * @param[in] daemon_extra Extra argv after `--socket PATH` (pool flags).
     * @param[in] timeout_ms  Total wait (default 45 s).
     */
    [[nodiscard]] bool connect_or_start(const std::filesystem::path& socket_path,
                                        const std::vector<std::string>& daemon_extra,
                                        int timeout_ms = k_autostart_wait_ms);

    /** Close the socket without implicit RELEASE (destructor still releases). */
    void close();

    [[nodiscard]] bool connected() const noexcept
    {
        return static_cast<bool>(fd_);
    }

private:
    [[nodiscard]] bool rpc(std::string_view cmd, std::string& reply);
    [[nodiscard]] bool reconnect();
    [[nodiscard]] bool spawn_daemon();
    void start_keepalive();
    void stop_keepalive();

    unique_fd fd_;
    std::string leftover_;
    std::mutex mu_;
    std::filesystem::path socket_path_;
    std::vector<std::string> daemon_extra_;
    std::vector<std::string> held_;
    std::jthread keepalive_;
};

} // namespace proxchunk

#endif /* PROXCHUNK_PROXY_IPC_HPP */
