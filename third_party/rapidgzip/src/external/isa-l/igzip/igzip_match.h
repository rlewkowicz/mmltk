#ifndef IGZIP_MATCH_H
#define IGZIP_MATCH_H

#include <stdint.h>

struct igzip_match {
    uint32_t literal;
    uint32_t distance;
    uint16_t length;
};

/* Every deflate body implementation bails out the same way when the input window is empty:
 * park the stream in a terminal state if this was the last input, then return to the caller.
 * Only the terminal state differs per implementation, so it is passed in.
 * Returns non-zero when the caller must return immediately. */
static inline int igzip_flush_if_input_empty(struct isal_zstream* stream, enum isal_zstate_state empty_state) {
    if (stream->avail_in != 0)
        return 0;
    if (stream->end_of_stream || stream->flush != NO_FLUSH)
        stream->internal_state.state = empty_state;
    return 1;
}

static inline uint32_t igzip_match_hash(uint32_t literal, uint32_t hash_mask, int use_mad_hash) {
    return (use_mad_hash ? compute_hash_mad(literal) : compute_hash(literal)) & hash_mask;
}

static inline struct igzip_match igzip_find_match_limited(uint8_t* next_in, uint8_t* file_start, uint16_t* last_seen,
                                                          uint32_t hist_size, uint32_t hash_mask,
                                                          uint32_t maximum_length, int use_mad_hash,
                                                          uint32_t unhashed_tail) {
    struct igzip_match result;
    uint32_t hash;
    uint8_t* next_hash;
    uint8_t* end;

    result.literal = load_le_u32(next_in);
    hash = igzip_match_hash(result.literal, hash_mask, use_mad_hash);
    result.distance = (next_in - file_start - last_seen[hash]) & 0xFFFF;
    last_seen[hash] = (uint64_t)(next_in - file_start);
    result.length = 0;

    if (result.distance - 1 >= hist_size)
        return result;

    assert(result.distance != 0);
    result.length = compare258(next_in - result.distance, next_in, maximum_length);
    if (result.length < SHORTEST_MATCH)
        return result;

    next_hash = next_in + 1;
#ifdef ISAL_LIMIT_HASH_UPDATE
    (void)unhashed_tail;
    end = next_in + 3;
#else
    end = next_in + result.length - unhashed_tail;
#endif
    for (; next_hash < end; next_hash++) {
        const uint32_t next_literal = load_le_u32(next_hash);
        hash = igzip_match_hash(next_literal, hash_mask, use_mad_hash);
        last_seen[hash] = (uint64_t)(next_hash - file_start);
    }

    return result;
}

static inline struct igzip_match igzip_find_match(uint8_t* next_in, uint8_t* file_start, uint16_t* last_seen,
                                                  uint32_t hist_size, uint32_t hash_mask) {
    return igzip_find_match_limited(next_in, file_start, last_seen, hist_size, hash_mask, 258, 0, 0);
}

#endif
