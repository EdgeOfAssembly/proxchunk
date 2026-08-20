/**
 * @file plan_n.h
 * @brief C23 N-way Range piece bounds (shared by the planner and CBMC).
 */

#ifndef PROXCHUNK_PLAN_N_H
#define PROXCHUNK_PLAN_N_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How many equal pieces to use for a file of @p size bytes.
 *
 * @param[in] size      File size in bytes.
 * @param[in] n_pieces  Requested piece count (typically logical CPU count).
 *
 * @return 0 if @p size or @p n_pieces is invalid; otherwise
 *         min(@p n_pieces, @p size) so every piece has at least one byte.
 */
static inline int
plan_n_piece_count(int64_t size, int n_pieces)
{
    if (size <= 0 || n_pieces < 1)
    {
        return 0;
    }
    if ((int64_t)n_pieces > size)
    {
        return (int)size;
    }
    return n_pieces;
}

/**
 * @brief Inclusive byte range of piece @p i in an N-way split of @p size.
 *
 * Pieces 0 .. n-2 each have length size/n; the last piece takes the remainder
 * so the union is exactly [0, size).
 *
 * @param[in]  size  File size in bytes (must be > 0).
 * @param[in]  n     Piece count from plan_n_piece_count (must be >= 1).
 * @param[in]  i     Piece index in [0, n).
 * @param[out] start Inclusive start offset.
 * @param[out] end   Inclusive end offset.
 *
 * @warning @p start and @p end must be non-NULL. Invalid inputs write
 *          start=0, end=-1 (empty range).
 */
static inline void
plan_n_bounds(int64_t size, int n, int i, int64_t *start, int64_t *end)
{
    if (start == NULL || end == NULL)
    {
        return;
    }
    if (size <= 0 || n < 1 || i < 0 || i >= n)
    {
        *start = 0;
        *end = -1;
        return;
    }
    const int64_t piece = size / (int64_t)n;
    const int64_t ii = (int64_t)i;
    *start = ii * piece;
    if (i == n - 1)
    {
        *end = size - 1;
    }
    else
    {
        *end = *start + piece - 1;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* PROXCHUNK_PLAN_N_H */
