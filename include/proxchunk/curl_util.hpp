/**
 * @file curl_util.hpp
 * @brief Shared libcurl helpers for proxy URLs and write callbacks.
 */

#ifndef PROXCHUNK_CURL_UTIL_HPP
#define PROXCHUNK_CURL_UTIL_HPP

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <string_view>
#include <sys/socket.h>

#ifndef PROXCHUNK_VERSION
#define PROXCHUNK_VERSION "1.1"
#endif

namespace proxchunk {

inline constexpr const char* k_user_agent = "proxchunk/" PROXCHUNK_VERSION;

inline constexpr const char* k_default_test_url =
    "https://speed.cloudflare.com/__down?bytes=65536";

/**
 * @brief True when @p proxy is a SOCKS URL (TCP tunnel, no HTTP CONNECT).
 * @param[in] proxy Proxy URL.
 * @return Whether the scheme is socks4/5 (including *h / *a variants).
 */
[[nodiscard]] inline bool
is_socks_proxy(std::string_view proxy)
{
    return proxy.starts_with("socks5://") || proxy.starts_with("socks5h://")
           || proxy.starts_with("socks4://") || proxy.starts_with("socks4a://");
}

/**
 * @brief Apply @p proxy to a curl easy handle.
 *
 * HTTP proxies get @c CURLOPT_HTTPPROXYTUNNEL when the target is HTTPS.
 *
 * @param[in,out] c           Easy handle (must not be NULL).
 * @param[in]     proxy       Proxy URL; empty is a no-op.
 * @param[in]     target_https True when the origin URL is https://.
 */
/**
 * @brief Kernel sockopts: TCP_NODELAY, QUICKACK, Fast Open, FAST/BBR/westwood.
 *
 * Failures are ignored (UNIX sockets, missing cc modules). TFO needs
 * @c net.ipv4.tcp_fastopen client bit; congestion @c fast is used when loaded.
 */
inline int
fast_tcp_sockopt(void* /*clientp*/, curl_socket_t fd, curlsocktype purpose)
{
    if (purpose != CURLSOCKTYPE_IPCXN)
    {
        return CURL_SOCKOPT_OK;
    }
    int one = 1;
    (void)::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_QUICKACK
    (void)::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
#ifdef TCP_FASTOPEN_CONNECT
    (void)::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_FASTOPEN_CONNECT, &one,
                       sizeof(one));
#endif
#ifndef TCP_CONGESTION
#define TCP_CONGESTION 13
#endif
    static const char* const k_cc[] = {"fast", "bbr", "westwood"};
    for (const char* cc : k_cc)
    {
        if (::setsockopt(static_cast<int>(fd), IPPROTO_TCP, TCP_CONGESTION, cc,
                         static_cast<socklen_t>(std::strlen(cc)))
            == 0)
        {
            break;
        }
    }
    return CURL_SOCKOPT_OK;
}

/**
 * @brief Enable Fast TCP / TFO / keepalive / Nagle-off on a curl easy handle.
 */
inline void
apply_fast_tcp(CURL* c)
{
    if (c == nullptr)
    {
        return;
    }
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
#ifdef CURLOPT_TCP_KEEPIDLE
    curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 30L);
#endif
#ifdef CURLOPT_TCP_KEEPINTVL
    curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 10L);
#endif
#ifdef CURLOPT_TCP_FASTOPEN
    curl_easy_setopt(c, CURLOPT_TCP_FASTOPEN, 1L);
#endif
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, fast_tcp_sockopt);
}

inline void
apply_curl_proxy(CURL* c, const std::string& proxy, bool target_https)
{
    if (c == nullptr || proxy.empty())
    {
        return;
    }
    curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());
    if (target_https && !is_socks_proxy(proxy))
    {
        curl_easy_setopt(c, CURLOPT_HTTPPROXYTUNNEL, 1L);
    }
}

/**
 * @brief Discard curl body bytes.
 */
inline std::size_t
write_null(char* /*ptr*/, std::size_t size, std::size_t nmemb, void* /*userdata*/)
{
    return size * nmemb;
}

/**
 * @brief Append curl bytes to a @c std::string userdata.
 */
inline std::size_t
write_to_string(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

/**
 * @brief Write curl bytes to a @c FILE* userdata.
 */
inline std::size_t
write_to_file(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* f = static_cast<FILE*>(userdata);
    return std::fwrite(ptr, 1, size * nmemb, f);
}

} // namespace proxchunk

#endif /* PROXCHUNK_CURL_UTIL_HPP */
