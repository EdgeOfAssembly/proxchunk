/**
 * @file test_plan.cpp
 * @brief Unit tests for chunk planning and output names.
 */

#include "proxchunk/plan.hpp"

#include <cassert>
#include <iostream>

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

    std::cout << "test_plan ok\n";
    return 0;
}
