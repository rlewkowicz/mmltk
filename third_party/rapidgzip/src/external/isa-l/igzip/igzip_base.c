#include <stdint.h>

#include "bitbuf2.h"
#include "huff_codes.h"
#include "huffman.h"
#include "igzip_lib.h"
#include "igzip_match.h"

extern const struct isal_hufftables hufftables_default;

static inline void update_state(struct isal_zstream* stream, uint8_t* start_in, uint8_t* next_in, uint8_t* end_in) {
    struct isal_zstate* state = &stream->internal_state;
    uint32_t bytes_written;

    if (next_in - start_in > 0)
        state->has_hist = IGZIP_HIST;

    stream->next_in = next_in;
    stream->total_in += next_in - start_in;
    stream->avail_in = end_in - next_in;

    bytes_written = buffer_used(&state->bitbuf);
    stream->total_out += bytes_written;
    stream->next_out += bytes_written;
    stream->avail_out -= bytes_written;
}

static inline void write_match_bits(struct isal_zstream* stream, uint16_t match_length, uint32_t distance) {
    uint64_t code, code_len, distance_code, distance_code_len;
    get_len_code(stream->hufftables, match_length, &code, &code_len);
    get_dist_code(stream->hufftables, distance, &distance_code, &distance_code_len);
    code |= distance_code << code_len;
    code_len += distance_code_len;
    write_bits(&stream->internal_state.bitbuf, code, code_len);
}

void isal_deflate_body_base(struct isal_zstream* stream) {
    uint32_t literal;
    uint8_t *start_in, *next_in, *end_in;
    uint16_t match_length;
    uint32_t dist;
    uint64_t code, code_len;
    struct isal_zstate* state = &stream->internal_state;
    uint16_t* last_seen = state->head;
    uint8_t* file_start = (uint8_t*)((uintptr_t)stream->next_in - stream->total_in);
    uint32_t hist_size = state->dist_mask;
    uint32_t hash_mask = state->hash_mask;

    if (igzip_flush_if_input_empty(stream, ZSTATE_FLUSH_READ_BUFFER))
        return;

    set_buf(&state->bitbuf, stream->next_out, stream->avail_out);

    start_in = stream->next_in;
    end_in = start_in + stream->avail_in;
    next_in = start_in;

    while (next_in + ISAL_LOOK_AHEAD < end_in) {
        if (is_full(&state->bitbuf)) {
            update_state(stream, start_in, next_in, end_in);
            return;
        }

        {
            const struct igzip_match match = igzip_find_match(next_in, file_start, last_seen, hist_size, hash_mask);
            literal = match.literal;
            dist = match.distance;
            match_length = match.length;
        }
        if (match_length >= SHORTEST_MATCH) {
            write_match_bits(stream, match_length, dist);
            next_in += match_length;

            continue;
        }

        get_lit_code(stream->hufftables, literal & 0xFF, &code, &code_len);
        write_bits(&state->bitbuf, code, code_len);
        next_in++;
    }

    update_state(stream, start_in, next_in, end_in);

    assert(stream->avail_in <= ISAL_LOOK_AHEAD);
    if (stream->end_of_stream || stream->flush != NO_FLUSH)
        state->state = ZSTATE_FLUSH_READ_BUFFER;

    return;
}

void isal_deflate_finish_base(struct isal_zstream* stream) {
    uint32_t literal = 0;
    uint8_t *start_in, *next_in, *end_in;
    uint64_t code, code_len;
    struct isal_zstate* state = &stream->internal_state;
    uint16_t* last_seen = state->head;
    uint8_t* file_start = (uint8_t*)((uintptr_t)stream->next_in - stream->total_in);
    uint32_t hist_size = state->dist_mask;
    uint32_t hash_mask = state->hash_mask;

    set_buf(&state->bitbuf, stream->next_out, stream->avail_out);

    start_in = stream->next_in;
    end_in = start_in + stream->avail_in;
    next_in = start_in;

    if (stream->avail_in != 0) {
        while (next_in + 3 < end_in) {
            if (is_full(&state->bitbuf)) {
                update_state(stream, start_in, next_in, end_in);
                return;
            }

            const struct igzip_match match =
                igzip_find_match_limited(next_in, file_start, last_seen, hist_size, hash_mask, end_in - next_in, 0, 3);
            literal = match.literal;
            if (match.length >= SHORTEST_MATCH) {
                write_match_bits(stream, match.length, match.distance);
                next_in += match.length;
                continue;
            }

            get_lit_code(stream->hufftables, literal & 0xFF, &code, &code_len);
            write_bits(&state->bitbuf, code, code_len);
            next_in++;
        }

        while (next_in < end_in) {
            if (is_full(&state->bitbuf)) {
                update_state(stream, start_in, next_in, end_in);
                return;
            }

            literal = *next_in;
            get_lit_code(stream->hufftables, literal & 0xFF, &code, &code_len);
            write_bits(&state->bitbuf, code, code_len);
            next_in++;
        }
    }

    if (!is_full(&state->bitbuf)) {
        get_lit_code(stream->hufftables, 256, &code, &code_len);
        write_bits(&state->bitbuf, code, code_len);
        state->has_eob = 1;

        if (stream->end_of_stream == 1)
            state->state = ZSTATE_TRL;
        else
            state->state = ZSTATE_SYNC_FLUSH;
    }

    update_state(stream, start_in, next_in, end_in);

    return;
}

ISAL_DEFLATE_HASH_FILL_FUNC(isal_deflate_hash_base, compute_hash)
