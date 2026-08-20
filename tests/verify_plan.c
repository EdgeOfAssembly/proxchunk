/**
 * @file verify_plan.c
 * @brief Concrete checks plus CBMC harness for N-way Range piece bounds.
 */

#include "proxchunk/plan_n.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void
assert_covers(int64_t size, int n_pieces)
{
    const int n = plan_n_piece_count(size, n_pieces);
    assert(n >= 1);
    assert(n <= n_pieces);
    assert((int64_t)n <= size);

    int64_t prev_end = -1;
    int64_t covered = 0;
    for (int i = 0; i < n; ++i)
    {
        int64_t start = 0;
        int64_t end = 0;
        plan_n_bounds(size, n, i, &start, &end);
        assert(start >= 0);
        assert(end >= start);
        assert(end < size);
        assert(start == prev_end + 1);
        covered += end - start + 1;
        prev_end = end;
    }
    assert(covered == size);
    assert(prev_end == size - 1);
}

#ifdef __CPROVER__

int
main(void)
{
    int64_t size = 0;
    int n_pieces = 0;
    __CPROVER_assume(size >= 1 && size <= 256);
    __CPROVER_assume(n_pieces >= 1 && n_pieces <= 16);

    const int n = plan_n_piece_count(size, n_pieces);
    __CPROVER_assert(n >= 1, "at least one piece");
    __CPROVER_assert(n <= n_pieces, "no more pieces than requested");
    __CPROVER_assert((int64_t)n <= size, "no more pieces than bytes");

    int64_t prev_end = -1;
    int64_t covered = 0;
    for (int i = 0; i < n; ++i)
    {
        int64_t start = 0;
        int64_t end = 0;
        plan_n_bounds(size, n, i, &start, &end);
        __CPROVER_assert(start >= 0, "start non-negative");
        __CPROVER_assert(end >= start, "non-empty piece");
        __CPROVER_assert(end < size, "end in range");
        __CPROVER_assert(start == prev_end + 1, "no gap or overlap");
        covered += end - start + 1;
        prev_end = end;
    }
    __CPROVER_assert(covered == size, "covers whole file");
    __CPROVER_assert(prev_end == size - 1, "last byte included");
    return 0;
}

#else

int
main(void)
{
    assert(plan_n_piece_count(0, 8) == 0);
    assert(plan_n_piece_count(10, 0) == 0);
    assert(plan_n_piece_count(3, 8) == 3);
    assert(plan_n_piece_count(100, 8) == 8);

    assert_covers(100, 8);
    assert_covers(8, 8);
    assert_covers(10, 1);
    assert_covers(1, 8);
    assert_covers(7, 3);

    int64_t s = 0;
    int64_t e = 0;
    plan_n_bounds(100, 8, 0, &s, &e);
    assert(s == 0 && e == 11);
    plan_n_bounds(100, 8, 7, &s, &e);
    assert(s == 84 && e == 99);

    puts("verify_plan concrete ok");
    return 0;
}

#endif
