/**
 * @file test_plan.cpp
 * @brief Unit tests for chunk planning and output names.
 */

#include "proxchunk/plan.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

int
main()
{
    using proxchunk::default_output_name;
    using proxchunk::plan_chunks;
    using proxchunk::trim;

    assert(trim("  abc \n") == "abc");
    assert(default_output_name("https://speedtest.1fichier.com/default.dat") == "default.dat");
    assert(default_output_name("https://ex.com/a.bin?x=1") == "a.bin");
    assert(default_output_name("https://ex.com/") == "download.bin");

    auto chunks = plan_chunks(100, 40);
    assert(chunks.size() == 3);
    assert(chunks[0].start == 0 && chunks[0].end == 39 && chunks[0].id == 0);
    assert(chunks[1].start == 40 && chunks[1].end == 79);
    assert(chunks[2].start == 80 && chunks[2].end == 99);

    auto one = plan_chunks(10, 100);
    assert(one.size() == 1);
    assert(one[0].start == 0 && one[0].end == 9);

    assert(plan_chunks(0, 8).empty());
    assert(plan_chunks(8, 0).empty());

    using proxchunk::normalize_proxy_line;
    assert(normalize_proxy_line("").empty());
    assert(normalize_proxy_line("  # comment").empty());
    assert(normalize_proxy_line("1.2.3.4:8080") == "http://1.2.3.4:8080");
    assert(normalize_proxy_line("socks5h://127.0.0.1:9050") == "socks5h://127.0.0.1:9050");
    assert(normalize_proxy_line("  10.0.0.1:1080  ", "socks5h://") == "socks5h://10.0.0.1:1080");
    assert(normalize_proxy_line("no-port").empty());

    auto tiny = plan_chunks(1, 1);
    assert(tiny.size() == 1 && tiny[0].start == 0 && tiny[0].end == 0);

    using proxchunk::plan_chunks_n;
    auto n8 = plan_chunks_n(100, 8);
    assert(n8.size() == 8);
    assert(n8[0].start == 0 && n8[0].end == 11 && n8[0].id == 0);
    assert(n8[7].start == 84 && n8[7].end == 99 && n8[7].id == 7);
    for (std::size_t i = 1; i < n8.size(); ++i)
    {
        assert(n8[i].start == n8[i - 1].end + 1);
    }
    auto n_cap = plan_chunks_n(3, 8);
    assert(n_cap.size() == 3);
    assert(n_cap[0].start == 0 && n_cap[2].end == 2);
    auto n_one = plan_chunks_n(10, 1);
    assert(n_one.size() == 1 && n_one[0].start == 0 && n_one[0].end == 9);
    assert(plan_chunks_n(0, 8).empty());
    assert(plan_chunks_n(10, 0).empty());

    const char* tmp = "/tmp/proxchunk-test-proxies.txt";
    {
        std::ofstream o(tmp);
        o << "1.2.3.4:8080\n# comment\n\nsocks5h://10.0.0.1:1080\n";
    }
    auto loaded = proxchunk::load_proxy_file(tmp);
    std::remove(tmp);
    assert(loaded.size() == 2);
    assert(loaded[0] == "http://1.2.3.4:8080");
    assert(loaded[1] == "socks5h://10.0.0.1:1080");
    assert(proxchunk::load_proxy_file("/no/such/proxchunk-proxies.txt").empty());

    std::cout << "test_plan ok\n";
    return 0;
}
