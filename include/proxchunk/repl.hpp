/**
 * @file repl.hpp
 * @brief Line tokenizer and directory builtins for `proxchunk --repl`.
 */

#ifndef PROXCHUNK_REPL_HPP
#define PROXCHUNK_REPL_HPP

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace proxchunk {

enum class repl_kind
{
    empty,     ///< Blank line — reprint prompt.
    quit,      ///< exit / quit / EOF.
    error,     ///< Builtin failed or bad quotes; print @c message.
    info,      ///< Builtin succeeded; print @c message if non-empty.
    run,       ///< Remaining tokens are proxchunk argv (no program name).
};

struct repl_result
{
    repl_kind kind = repl_kind::empty;
    std::string message;
    std::vector<std::string> argv;
};

/**
 * @brief Split @p line on whitespace with `'…'` / `"…"` quoting.
 *
 * Backslash escapes the next character outside quotes. Unmatched quotes
 * return an empty vector and set @p err.
 *
 * @param[in]  line Input line (no trailing newline required).
 * @param[out] err  Set on unmatched quote; otherwise cleared.
 * @return Tokens; empty on error or blank input.
 */
[[nodiscard]] inline std::vector<std::string>
tokenize_repl_line(std::string_view line, std::string& err)
{
    err.clear();
    std::vector<std::string> out;
    std::string cur;
    bool in_token = false;
    char quote = '\0';

    auto flush = [&]() {
        if (in_token)
        {
            out.push_back(std::move(cur));
            cur.clear();
            in_token = false;
        }
    };

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const char c = line[i];
        if (quote != '\0')
        {
            if (c == quote)
            {
                quote = '\0';
            }
            else
            {
                cur.push_back(c);
            }
            continue;
        }
        if (c == '\\' && i + 1 < line.size())
        {
            in_token = true;
            cur.push_back(line[++i]);
            continue;
        }
        if (c == '\'' || c == '"')
        {
            in_token = true;
            quote = c;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            flush();
            continue;
        }
        in_token = true;
        cur.push_back(c);
    }
    if (quote != '\0')
    {
        err = "unmatched quote";
        return {};
    }
    flush();
    return out;
}

[[nodiscard]] inline std::string
repl_home_dir()
{
    const char* h = std::getenv("HOME");
    if (h != nullptr && h[0] != '\0')
    {
        return std::string(h);
    }
    return "/";
}

/**
 * @brief Run one REPL line: builtins or pass-through to proxchunk.
 *
 * @param[in] line Raw input (prompt not included).
 * @return What the caller should do next.
 */
[[nodiscard]] inline repl_result
handle_repl_line(std::string_view line)
{
    namespace fs = std::filesystem;
    repl_result r;
    std::string err;
    auto tok = tokenize_repl_line(line, err);
    if (!err.empty())
    {
        r.kind = repl_kind::error;
        r.message = err;
        return r;
    }
    if (tok.empty())
    {
        r.kind = repl_kind::empty;
        return r;
    }

    const std::string& cmd = tok[0];
    if (cmd == "exit" || cmd == "quit")
    {
        r.kind = repl_kind::quit;
        return r;
    }
    if (cmd == "help")
    {
        r.kind = repl_kind::info;
        r.message =
            "builtins: cd [dir]  pwd  mkdir [-p] dir  rm file  rmdir dir  exit\n"
            "anything else is passed to proxchunk (try -h)";
        return r;
    }
    if (cmd == "pwd")
    {
        std::error_code ec;
        fs::path p = fs::current_path(ec);
        if (ec)
        {
            r.kind = repl_kind::error;
            r.message = ec.message();
            return r;
        }
        r.kind = repl_kind::info;
        r.message = p.string();
        return r;
    }
    if (cmd == "cd")
    {
        std::string dest = (tok.size() >= 2) ? tok[1] : repl_home_dir();
        std::error_code ec;
        fs::current_path(fs::path(dest), ec);
        if (ec)
        {
            r.kind = repl_kind::error;
            r.message = "cd: " + dest + ": " + ec.message();
            return r;
        }
        r.kind = repl_kind::info;
        r.message = fs::current_path().string();
        return r;
    }
    if (cmd == "mkdir")
    {
        bool parents = false;
        std::vector<std::string> dirs;
        for (std::size_t i = 1; i < tok.size(); ++i)
        {
            if (tok[i] == "-p")
            {
                parents = true;
            }
            else
            {
                dirs.push_back(tok[i]);
            }
        }
        if (dirs.empty())
        {
            r.kind = repl_kind::error;
            r.message = "mkdir: missing operand";
            return r;
        }
        for (const auto& d : dirs)
        {
            std::error_code ec;
            if (parents)
            {
                fs::create_directories(fs::path(d), ec);
            }
            else
            {
                fs::create_directory(fs::path(d), ec);
            }
            if (ec)
            {
                r.kind = repl_kind::error;
                r.message = "mkdir: " + d + ": " + ec.message();
                return r;
            }
        }
        r.kind = repl_kind::info;
        return r;
    }
    if (cmd == "rm")
    {
        if (tok.size() < 2)
        {
            r.kind = repl_kind::error;
            r.message = "rm: missing operand";
            return r;
        }
        for (std::size_t i = 1; i < tok.size(); ++i)
        {
            std::error_code ec;
            fs::path p(tok[i]);
            auto st = fs::symlink_status(p, ec);
            if (ec)
            {
                r.kind = repl_kind::error;
                r.message = "rm: " + tok[i] + ": " + ec.message();
                return r;
            }
            if (fs::is_directory(st))
            {
                r.kind = repl_kind::error;
                r.message = "rm: " + tok[i] + ": is a directory (use rmdir)";
                return r;
            }
            if (!fs::remove(p, ec) || ec)
            {
                r.kind = repl_kind::error;
                r.message = "rm: " + tok[i] + ": " + (ec ? ec.message() : "failed");
                return r;
            }
        }
        r.kind = repl_kind::info;
        return r;
    }
    if (cmd == "rmdir")
    {
        if (tok.size() < 2)
        {
            r.kind = repl_kind::error;
            r.message = "rmdir: missing operand";
            return r;
        }
        for (std::size_t i = 1; i < tok.size(); ++i)
        {
            std::error_code ec;
            fs::path p(tok[i]);
            if (!fs::is_directory(p, ec))
            {
                r.kind = repl_kind::error;
                r.message = "rmdir: " + tok[i] + ": not a directory";
                return r;
            }
            if (!fs::is_empty(p, ec))
            {
                r.kind = repl_kind::error;
                r.message = "rmdir: " + tok[i] + ": directory not empty";
                return r;
            }
            fs::remove(p, ec);
            if (ec)
            {
                r.kind = repl_kind::error;
                r.message = "rmdir: " + tok[i] + ": " + ec.message();
                return r;
            }
        }
        r.kind = repl_kind::info;
        return r;
    }

    r.kind = repl_kind::run;
    r.argv = std::move(tok);
    return r;
}

} // namespace proxchunk

#endif /* PROXCHUNK_REPL_HPP */
