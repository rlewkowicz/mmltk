#include <stdint.h>

#include "encode_df.h"
#include "huff_codes.h"
#include "huffman.h"
#include "igzip_level_buf_structs.h"
#include "igzip_lib.h"
#include "igzip_match.h"
#include "unaligned.h"

static inline void write_deflate_icf(struct deflate_icf* icf, uint32_t lit_len, uint32_t lit_dist,
                                     uint32_t extra_bits) {
    icf->lit_len = lit_len;
    icf->lit_dist = lit_dist;
    icf->dist_extra = extra_bits;
}

static inline void write_match_icf(struct level_buf* level_buf, struct deflate_icf* output, uint16_t match_length,
                                   uint32_t distance) {
    uint32_t length_code, distance_code, extra_bits;
    get_len_icf_code(match_length, &length_code);
    get_dist_icf_code(distance, &distance_code, &extra_bits);
    level_buf->hist.ll_hist[length_code]++;
    level_buf->hist.d_hist[distance_code]++;
    write_deflate_icf(output, length_code, distance_code, extra_bits);
}

static inline void write_literal_icf(struct level_buf* level_buf, struct deflate_icf* output, uint32_t literal) {
    uint32_t code;
    get_lit_icf_code(literal & 0xFF, &code);
    level_buf->hist.ll_hist[code]++;
    write_deflate_icf(output, code, NULL_DIST_SYM, 0);
}

static inline void update_state(struct isal_zstream* stream, uint8_t* start_in, uint8_t* next_in, uint8_t* end_in,
                                struct deflate_icf* start_out, struct deflate_icf* next_out,
                                struct deflate_icf* end_out) {
    struct level_buf* level_buf = (struct level_buf*)stream->level_buf;

    if (next_in - start_in > 0)
        stream->internal_state.has_hist = IGZIP_HIST;

    stream->next_in = next_in;
    stream->total_in += next_in - start_in;
    stream->internal_state.block_end = stream->total_in;
    stream->avail_in = end_in - next_in;

    level_buf->icf_buf_next = next_out;
    level_buf->icf_buf_avail_out = end_out - next_out;
}

void isal_deflate_icf_body_hash_hist_base(struct isal_zstream* stream) {
    uint32_t literal;
    uint8_t *start_in, *next_in, *end_in;
    struct deflate_icf *start_out, *next_out, *end_out;
    uint16_t match_length;
    uint32_t dist;
    struct isal_zstate* state = &stream->internal_state;
    struct level_buf* level_buf = (struct level_buf*)stream->level_buf;
    uint16_t* last_seen = level_buf->hash_hist.hash_table;
    uint8_t* file_start = (uint8_t*)((uintptr_t)stream->next_in - stream->total_in);
    uint32_t hist_size = state->dist_mask;
    uint32_t hash_mask = state->hash_mask;

    if (igzip_flush_if_input_empty(stream, ZSTATE_FLUSH_READ_BUFFER))
        return;

    start_in = stream->next_in;
    end_in = start_in + stream->avail_in;
    next_in = start_in;

    start_out = ((struct level_buf*)stream->level_buf)->icf_buf_next;
    /* icf_buf_avail_out is a byte count; divide it into an element count before pointer scaling. */
    /* NOLINTBEGIN(bugprone-sizeof-expression) */
    end_out = start_out + ((struct level_buf*)stream->level_buf)->icf_buf_avail_out / sizeof(struct deflate_icf);
    /* NOLINTEND(bugprone-sizeof-expression) */
    next_out = start_out;

    while (next_in + ISAL_LOOK_AHEAD < end_in) {
        if (next_out >= end_out) {
            state->state = ZSTATE_CREATE_HDR;
            update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);
            return;
        }

        {
            const struct igzip_match match = igzip_find_match(next_in, file_start, last_seen, hist_size, hash_mask);
            literal = match.literal;
            dist = match.distance;
            match_length = match.length;
        }
        if (match_length >= SHORTEST_MATCH) {
            write_match_icf(level_buf, next_out, match_length, dist);
            next_out++;
            next_in += match_length;

            continue;
        }

        write_literal_icf(level_buf, next_out, literal);
        next_out++;
        next_in++;
    }

    update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);

    assert(stream->avail_in <= ISAL_LOOK_AHEAD);
    if (stream->end_of_stream || stream->flush != NO_FLUSH)
        state->state = ZSTATE_FLUSH_READ_BUFFER;

    return;
}

static void finish_hash_base(struct isal_zstream* stream, uint16_t* last_seen, int use_mad_hash) {
    uint32_t literal = 0;
    uint8_t *start_in, *next_in, *end_in;
    struct deflate_icf *start_out, *next_out, *end_out;
    struct isal_zstate* state = &stream->internal_state;
    struct level_buf* level_buf = (struct level_buf*)stream->level_buf;
    uint8_t* file_start = (uint8_t*)((uintptr_t)stream->next_in - stream->total_in);
    uint32_t hist_size = state->dist_mask;
    uint32_t hash_mask = state->hash_mask;

    start_in = stream->next_in;
    end_in = start_in + stream->avail_in;
    next_in = start_in;

    start_out = ((struct level_buf*)stream->level_buf)->icf_buf_next;
    /* icf_buf_avail_out is a byte count; divide it into an element count before pointer scaling. */
    /* NOLINTBEGIN(bugprone-sizeof-expression) */
    end_out = start_out + ((struct level_buf*)stream->level_buf)->icf_buf_avail_out / sizeof(struct deflate_icf);
    /* NOLINTEND(bugprone-sizeof-expression) */
    next_out = start_out;

    if (igzip_flush_if_input_empty(stream, ZSTATE_CREATE_HDR))
        return;

    while (next_in + 3 < end_in) {
        if (next_out >= end_out) {
            state->state = ZSTATE_CREATE_HDR;
            update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);
            return;
        }

        const struct igzip_match match = igzip_find_match_limited(next_in, file_start, last_seen, hist_size, hash_mask,
                                                                  end_in - next_in, use_mad_hash, 3);
        literal = match.literal;
        if (match.length >= SHORTEST_MATCH) {
            write_match_icf(level_buf, next_out, match.length, match.distance);
            next_out++;
            next_in += match.length;
            continue;
        }

        write_literal_icf(level_buf, next_out, literal);
        next_out++;
        next_in++;
    }

    while (next_in < end_in) {
        if (next_out >= end_out) {
            state->state = ZSTATE_CREATE_HDR;
            update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);
            return;
        }

        literal = *next_in;
        write_literal_icf(level_buf, next_out, literal);
        next_out++;
        next_in++;
    }

    if (next_in == end_in) {
        if (stream->end_of_stream || stream->flush != NO_FLUSH)
            state->state = ZSTATE_CREATE_HDR;
    }

    update_state(stream, start_in, next_in, end_in, start_out, next_out, end_out);

    return;
}

void isal_deflate_icf_finish_hash_hist_base(struct isal_zstream* stream) {
    struct level_buf* level_buf = (struct level_buf*)stream->level_buf;
    finish_hash_base(stream, level_buf->hash_hist.hash_table, 0);
}

void isal_deflate_icf_finish_hash_map_base(struct isal_zstream* stream) {
    struct level_buf* level_buf = (struct level_buf*)stream->level_buf;
    finish_hash_base(stream, level_buf->hash_map.hash_table, 1);
}

ISAL_DEFLATE_HASH_FILL_FUNC(isal_deflate_hash_mad_base, compute_hash_mad)
