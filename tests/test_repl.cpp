/**
 * @file test_repl.cpp
 * @brief Unit tests for REPL tokenize and directory builtins.
 */

#include "proxchunk/repl.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

int
main()
{
    std::string err;
    auto t0 = proxchunk::tokenize_repl_line("  ", err);
    assert(err.empty() && t0.empty());

    auto t1 = proxchunk::tokenize_repl_line("--show-proxies -o out.bin https://ex.com/a?b=1&c=2", err);
    assert(err.empty());
    assert(t1.size() == 4);
    assert(t1[3] == "https://ex.com/a?b=1&c=2");

    auto tq = proxchunk::tokenize_repl_line("cd \"/tmp/my dir\"", err);
    assert(err.empty() && tq.size() == 2 && tq[1] == "/tmp/my dir");

    auto ts = proxchunk::tokenize_repl_line("echo 'a b'", err);
    assert(err.empty() && ts.size() == 2 && ts[1] == "a b");

    auto bad = proxchunk::tokenize_repl_line("\"open", err);
    assert(!err.empty() && bad.empty());

    auto rhelp = proxchunk::handle_repl_line("help");
    assert(rhelp.kind == proxchunk::repl_kind::info);

    auto rq = proxchunk::handle_repl_line("quit");
    assert(rq.kind == proxchunk::repl_kind::quit);
    auto re = proxchunk::handle_repl_line("exit");
    assert(re.kind == proxchunk::repl_kind::quit);

    auto rrun = proxchunk::handle_repl_line("-h");
    assert(rrun.kind == proxchunk::repl_kind::run && rrun.argv.size() == 1 && rrun.argv[0] == "-h");

    fs::path tmp = fs::temp_directory_path() / ("proxchunk-repl-" + std::to_string(getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    fs::current_path(tmp);

    auto rmk = proxchunk::handle_repl_line("mkdir -p a/b");
    assert(rmk.kind == proxchunk::repl_kind::info);
    assert(fs::is_directory(tmp / "a" / "b"));

    auto rcd = proxchunk::handle_repl_line("cd a");
    assert(rcd.kind == proxchunk::repl_kind::info);
    assert(fs::current_path() == tmp / "a");

    auto rpwd = proxchunk::handle_repl_line("pwd");
    assert(rpwd.kind == proxchunk::repl_kind::info);
    assert(rpwd.message == (tmp / "a").string());

    {
        std::ofstream f("file.txt");
        f << "x\n";
    }
    auto rrmdirf = proxchunk::handle_repl_line("rmdir file.txt");
    assert(rrmdirf.kind == proxchunk::repl_kind::error);

    auto rrm = proxchunk::handle_repl_line("rm file.txt");
    assert(rrm.kind == proxchunk::repl_kind::info);
    assert(!fs::exists("file.txt"));

    auto rrmdir = proxchunk::handle_repl_line("rm b");
    assert(rrmdir.kind == proxchunk::repl_kind::error); // directory

    auto rok = proxchunk::handle_repl_line("rmdir b");
    assert(rok.kind == proxchunk::repl_kind::info);
    assert(!fs::exists("b"));

    fs::current_path(tmp.parent_path());
    fs::remove_all(tmp);
    std::cout << "test_repl ok\n";
    return 0;
}
