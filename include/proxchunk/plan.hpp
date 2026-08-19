/**
 * @file plan.hpp
 * @brief Pure helpers for proxchunk (chunk plan, output name).
 */

#ifndef PROXCHUNK_PLAN_HPP
#define PROXCHUNK_PLAN_HPP

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace proxchunk {

struct chunk
{
    std::int64_t start = 0; ///< Inclusive byte offset.
    std::int64_t end   = 0; ///< Inclusive byte offset.
    int          id    = 0;
};

[[nodiscard]] inline std::string
trim(std::string_view sv)
{
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'
                           || sv.front() == '\n'))
    {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'
                           || sv.back() == '\n'))
    {
        sv.remove_suffix(1);
    }
    return std::string(sv);
}

/**
 * @brief One proxy list line → URL. Bare `host:port` gets @p default_scheme.
 * Empty / comment lines return an empty string.
 */
[[nodiscard]] inline std::string
normalize_proxy_line(std::string_view raw, std::string_view default_scheme = "http://")
{
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == '/')
    {
        return {};
    }
    if (line.find("://") == std::string::npos)
    {
        if (line.find(':') == std::string::npos)
        {
            return {};
        }
        line.insert(0, default_scheme);
    }
    return line;
}

/** Read a user proxy list (one URL or host:port per line). */
[[nodiscard]] inline std::vector<std::string>
load_proxy_file(const std::string& path, std::string_view default_scheme = "http://")
{
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in)
    {
        return out;
    }
    std::string raw;
    while (std::getline(in, raw))
    {
        std::string url = normalize_proxy_line(raw, default_scheme);
        if (!url.empty())
        {
            out.push_back(std::move(url));
        }
    }
    return out;
}

/**
 * @brief Default output path from a URL (last path segment, query stripped).
 */
[[nodiscard]] inline std::string
default_output_name(std::string_view url)
{
    auto pos = url.find_last_of('/');
    std::string name = (pos == std::string_view::npos) ? std::string(url)
                                                       : std::string(url.substr(pos + 1));
    auto q = name.find('?');
    if (q != std::string::npos)
    {
        name.resize(q);
    }
    if (name.empty() || name == "." || name == "..")
    {
        return "download.bin";
    }
    return name;
}

/**
 * @brief Split [0, size) into inclusive Range chunks of at most chunk_bytes.
 */
[[nodiscard]] inline std::vector<chunk>
plan_chunks(std::int64_t size, std::int64_t chunk_bytes)
{
    std::vector<chunk> out;
    if (size <= 0 || chunk_bytes <= 0)
    {
        return out;
    }
    int id = 0;
    for (std::int64_t off = 0; off < size; off += chunk_bytes)
    {
        chunk ch;
        ch.start = off;
        ch.end   = std::min(off + chunk_bytes - 1, size - 1);
        ch.id    = id++;
        out.push_back(ch);
    }
    return out;
}

} // namespace proxchunk

#endif /* PROXCHUNK_PLAN_HPP */
