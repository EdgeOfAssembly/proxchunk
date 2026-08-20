/**
 * @file ia_size.hpp
 * @brief Parse archive.org /download/ URLs and metadata JSON for file size.
 */

#ifndef PROXCHUNK_IA_SIZE_HPP
#define PROXCHUNK_IA_SIZE_HPP

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace proxchunk {

/**
 * @brief Percent-decode @p in (`%20` → space). Invalid `%` sequences are copied.
 */
[[nodiscard]] inline std::string
url_decode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size()
            && std::isxdigit(static_cast<unsigned char>(in[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(in[i + 2])))
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f')
                {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F')
                {
                    return c - 'A' + 10;
                }
                return 0;
            };
            out.push_back(static_cast<char>((hex(in[i + 1]) << 4) | hex(in[i + 2])));
            i += 2;
        }
        else if (in[i] == '+')
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(in[i]);
        }
    }
    return out;
}

/**
 * @brief Extract item identifier and decoded filename from an archive.org download URL.
 *
 * @return nullopt if @p url is not `…archive.org/download/<id>/<file>`.
 */
[[nodiscard]] inline std::optional<std::pair<std::string, std::string>>
ia_parse_download_url(std::string_view url)
{
    const std::string_view marker = "archive.org/download/";
    const auto p = url.find(marker);
    if (p == std::string_view::npos)
    {
        return std::nullopt;
    }
    auto rest = url.substr(p + marker.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0)
    {
        return std::nullopt;
    }
    std::string ident(rest.substr(0, slash));
    auto file = rest.substr(slash + 1);
    const auto q = file.find_first_of("?#");
    if (q != std::string_view::npos)
    {
        file = file.substr(0, q);
    }
    if (file.empty() || ident.empty())
    {
        return std::nullopt;
    }
    return std::make_pair(std::move(ident), url_decode(file));
}

/**
 * @brief Read `"size"` for @p filename from archive.org metadata JSON.
 */
[[nodiscard]] inline std::optional<std::int64_t>
ia_size_from_metadata_json(std::string_view json, std::string_view filename)
{
    if (json.empty() || filename.empty())
    {
        return std::nullopt;
    }
    const std::string needle = std::string("\"name\":\"") + std::string(filename) + '"';
    auto pos = json.find(needle);
    if (pos == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto window = json.substr(pos, 1024);
    const auto sp = window.find("\"size\":");
    if (sp == std::string_view::npos)
    {
        return std::nullopt;
    }
    auto d = sp + 7;
    while (d < window.size() && (window[d] == ' ' || window[d] == '"'))
    {
        ++d;
    }
    if (d >= window.size() || !std::isdigit(static_cast<unsigned char>(window[d])))
    {
        return std::nullopt;
    }
    std::int64_t n = 0;
    while (d < window.size() && std::isdigit(static_cast<unsigned char>(window[d])))
    {
        n = n * 10 + (window[d] - '0');
        ++d;
    }
    if (n <= 0)
    {
        return std::nullopt;
    }
    return n;
}

} // namespace proxchunk

#endif /* PROXCHUNK_IA_SIZE_HPP */
