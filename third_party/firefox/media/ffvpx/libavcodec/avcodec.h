/*
 * copyright (c) 2001 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_AVCODEC_H
#define AVCODEC_AVCODEC_H


#include "libavutil/samplefmt.h"
#include "libavutil/attributes.h"
#include "libavutil/avutil.h"
#include "libavutil/buffer.h"
#include "libavutil/channel_layout.h"
#include "libavutil/dict.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"

#include "codec.h"
#include "codec_id.h"
#include "defs.h"
#include "packet.h"
#include "version_major.h"
#ifndef HAVE_AV_CONFIG_H
#include "version.h"

#include "codec_desc.h"
#include "codec_par.h"
#endif

struct AVCodecParameters;




typedef struct RcOverride{
    int start_frame;
    int end_frame;
    int qscale; 
    float quality_factor;
} RcOverride;


#define AV_CODEC_FLAG_UNALIGNED       (1 <<  0)
#define AV_CODEC_FLAG_QSCALE          (1 <<  1)
#define AV_CODEC_FLAG_4MV             (1 <<  2)
#define AV_CODEC_FLAG_OUTPUT_CORRUPT  (1 <<  3)
#define AV_CODEC_FLAG_QPEL            (1 <<  4)
#define AV_CODEC_FLAG_RECON_FRAME     (1 <<  6)
/**
 * @par decoding
 * Request the decoder to propagate each packet's AVPacket.opaque and
 * AVPacket.opaque_ref to its corresponding output AVFrame.
 *
 * @par encoding:
 * Request the encoder to propagate each frame's AVFrame.opaque and
 * AVFrame.opaque_ref values to its corresponding output AVPacket.
 *
 * @par
 * May only be set on encoders that have the
 * @ref AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE capability flag.
 *
 * @note
 * While in typical cases one input frame produces exactly one output packet
 * (perhaps after a delay), in general the mapping of frames to packets is
 * M-to-N, so
 * - Any number of input frames may be associated with any given output packet.
 *   This includes zero - e.g. some encoders may output packets that carry only
 *   metadata about the whole stream.
 * - A given input frame may be associated with any number of output packets.
 *   Again this includes zero - e.g. some encoders may drop frames under certain
 *   conditions.
 * .
 * This implies that when using this flag, the caller must NOT assume that
 * - a given input frame's opaques will necessarily appear on some output packet;
 * - every output packet will have some non-NULL opaque value.
 * .
 * When an output packet contains multiple frames, the opaque values will be
 * taken from the first of those.
 *
 * @note
 * The converse holds for decoders, with frames and packets switched.
 */
#define AV_CODEC_FLAG_COPY_OPAQUE     (1 <<  7)
#define AV_CODEC_FLAG_FRAME_DURATION  (1 <<  8)
#define AV_CODEC_FLAG_PASS1           (1 <<  9)
#define AV_CODEC_FLAG_PASS2           (1 << 10)
#define AV_CODEC_FLAG_LOOP_FILTER     (1 << 11)
#define AV_CODEC_FLAG_GRAY            (1 << 13)
#define AV_CODEC_FLAG_PSNR            (1 << 15)
#define AV_CODEC_FLAG_INTERLACED_DCT  (1 << 18)
#define AV_CODEC_FLAG_LOW_DELAY       (1 << 19)
#define AV_CODEC_FLAG_GLOBAL_HEADER   (1 << 22)
#define AV_CODEC_FLAG_BITEXACT        (1 << 23)
#define AV_CODEC_FLAG_AC_PRED         (1 << 24)
#define AV_CODEC_FLAG_INTERLACED_ME   (1 << 29)
#define AV_CODEC_FLAG_CLOSED_GOP      (1U << 31)

#define AV_CODEC_FLAG2_FAST           (1 <<  0)
#define AV_CODEC_FLAG2_NO_OUTPUT      (1 <<  2)
#define AV_CODEC_FLAG2_LOCAL_HEADER   (1 <<  3)

#define AV_CODEC_FLAG2_CHUNKS         (1 << 15)
#define AV_CODEC_FLAG2_IGNORE_CROP    (1 << 16)

#define AV_CODEC_FLAG2_SHOW_ALL       (1 << 22)
#define AV_CODEC_FLAG2_EXPORT_MVS     (1 << 28)
#define AV_CODEC_FLAG2_SKIP_MANUAL    (1 << 29)
#define AV_CODEC_FLAG2_RO_FLUSH_NOOP  (1 << 30)
#define AV_CODEC_FLAG2_ICC_PROFILES   (1U << 31)

#define AV_CODEC_EXPORT_DATA_MVS         (1 << 0)
#define AV_CODEC_EXPORT_DATA_PRFT        (1 << 1)
#define AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS (1 << 2)
#define AV_CODEC_EXPORT_DATA_FILM_GRAIN (1 << 3)

#define AV_CODEC_EXPORT_DATA_ENHANCEMENTS (1 << 4)

#define AV_GET_BUFFER_FLAG_REF (1 << 0)

#define AV_GET_ENCODE_BUFFER_FLAG_REF (1 << 0)

#define AV_CODEC_RECEIVE_FRAME_FLAG_SYNCHRONOUS (1 << 0)

typedef struct AVCodecContext {
    const AVClass *av_class;
    int log_level_offset;

    enum AVMediaType codec_type; 
    const struct AVCodec  *codec;
    enum AVCodecID     codec_id; 

    /**
     * fourcc (LSB first, so "ABCD" -> ('D'<<24) + ('C'<<16) + ('B'<<8) + 'A').
     * This is used to work around some encoder bugs.
     * A demuxer should set this to what is stored in the field used to identify the codec.
     * If there are multiple such fields in a container then the demuxer should choose the one
     * which maximizes the information about the used codec.
     * If the codec tag field in a container is larger than 32 bits then the demuxer should
     * remap the longer ID to 32 bits with a table or other structure. Alternatively a new
     * extra_codec_tag + size could be added but for this a clear advantage must be demonstrated
     * first.
     * - encoding: Set by user, if not then the default based on codec_id will be used.
     * - decoding: Set by user, will be converted to uppercase by libavcodec during init.
     */
    unsigned int codec_tag;

    void *priv_data;

    struct AVCodecInternal *internal;

    /**
     * Private data of the user, can be used to carry app specific stuff.
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    void *opaque;

    /**
     * the average bitrate
     * - encoding: Set by user; unused for constant quantizer encoding.
     * - decoding: Set by user, may be overwritten by libavcodec
     *             if this info is available in the stream
     */
    int64_t bit_rate;

    /**
     * AV_CODEC_FLAG_*.
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int flags;

    /**
     * AV_CODEC_FLAG2_*
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int flags2;

    /**
     * Out-of-band global headers that may be used by some codecs.
     *
     * - decoding: Should be set by the caller when available (typically from a
     *   demuxer) before opening the decoder; some decoders require this to be
     *   set and will fail to initialize otherwise.
     *
     *   The array must be allocated with the av_malloc() family of functions;
     *   allocated size must be at least AV_INPUT_BUFFER_PADDING_SIZE bytes
     *   larger than extradata_size.
     *
     * - encoding: May be set by the encoder in avcodec_open2() (possibly
     *   depending on whether the AV_CODEC_FLAG_GLOBAL_HEADER flag is set).
     *
     * After being set, the array is owned by the codec and freed in
     * avcodec_free_context().
     */
    uint8_t *extradata;
    int extradata_size;

    /**
     * This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identically 1.
     * This often, but not always is the inverse of the frame rate or field rate
     * for video. 1/time_base is not the average frame rate if the frame rate is not
     * constant.
     *
     * Like containers, elementary streams also can store timestamps, 1/time_base
     * is the unit in which these timestamps are specified.
     * As example of such codec time base see ISO/IEC 14496-2:2001(E)
     * vop_time_increment_resolution and fixed_vop_rate
     * (fixed_vop_rate == 0 implies that it is different from the framerate)
     *
     * - encoding: MUST be set by user.
     * - decoding: unused.
     */
    AVRational time_base;

    /**
     * Timebase in which pkt_dts/pts and AVPacket.dts/pts are expressed.
     * - encoding: unused.
     * - decoding: set by user.
     */
    AVRational pkt_timebase;

    /**
     * - decoding: For codecs that store a framerate value in the compressed
     *             bitstream, the decoder may export it here. { 0, 1} when
     *             unknown.
     * - encoding: May be used to signal the framerate of CFR content to an
     *             encoder.
     */
    AVRational framerate;

    /**
     * Codec delay.
     *
     * Encoding: Number of frames delay there will be from the encoder input to
     *           the decoder output. (we assume the decoder matches the spec)
     * Decoding: Number of frames delay in addition to what a standard decoder
     *           as specified in the spec would produce.
     *
     * Video:
     *   Number of frames the decoded output will be delayed relative to the
     *   encoded input.
     *
     * Audio:
     *   For encoding, this field is unused (see initial_padding).
     *
     *   For decoding, this is the number of samples the decoder needs to
     *   output before the decoder's output is valid. When seeking, you should
     *   start decoding this many samples prior to your desired seek point.
     *
     * - encoding: Set by libavcodec.
     * - decoding: Set by libavcodec.
     */
    int delay;


    /**
     * picture width / height.
     *
     * @note Those fields may not match the values of the last
     * AVFrame output by avcodec_receive_frame() due frame
     * reordering.
     *
     * - encoding: MUST be set by user.
     * - decoding: May be set by the user before opening the decoder if known e.g.
     *             from the container. Some decoders will require the dimensions
     *             to be set by the caller. During decoding, the decoder may
     *             overwrite those values as required while parsing the data.
     */
    int width, height;

    /**
     * Bitstream width / height, may be different from width/height e.g. when
     * the decoded frame is cropped before being output or lowres is enabled.
     *
     * @note Those field may not match the value of the last
     * AVFrame output by avcodec_receive_frame() due frame
     * reordering.
     *
     * - encoding: unused
     * - decoding: May be set by the user before opening the decoder if known
     *             e.g. from the container. During decoding, the decoder may
     *             overwrite those values as required while parsing the data.
     */
    int coded_width, coded_height;

    /**
     * sample aspect ratio (0 if unknown)
     * That is the width of a pixel divided by the height of the pixel.
     * Numerator and denominator must be relatively prime and smaller than 256 for some video standards.
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     */
    AVRational sample_aspect_ratio;

    /**
     * Pixel format, see AV_PIX_FMT_xxx.
     * May be set by the demuxer if known from headers.
     * May be overridden by the decoder if it knows better.
     *
     * @note This field may not match the value of the last
     * AVFrame output by avcodec_receive_frame() due frame
     * reordering.
     *
     * - encoding: Set by user.
     * - decoding: Set by user if known, overridden by libavcodec while
     *             parsing the data.
     */
    enum AVPixelFormat pix_fmt;

    /**
     * Nominal unaccelerated pixel format, see AV_PIX_FMT_xxx.
     * - encoding: unused.
     * - decoding: Set by libavcodec before calling get_format()
     */
    enum AVPixelFormat sw_pix_fmt;

    /**
     * Chromaticity coordinates of the source primaries.
     * - encoding: Set by user
     * - decoding: Set by libavcodec
     */
    enum AVColorPrimaries color_primaries;

    /**
     * Color Transfer Characteristic.
     * - encoding: Set by user
     * - decoding: Set by libavcodec
     */
    enum AVColorTransferCharacteristic color_trc;

    /**
     * YUV colorspace type.
     * - encoding: Set by user
     * - decoding: Set by libavcodec
     */
    enum AVColorSpace colorspace;

    /**
     * MPEG vs JPEG YUV range.
     * - encoding: Set by user to override the default output color range value,
     *   If not specified, libavcodec sets the color range depending on the
     *   output format.
     * - decoding: Set by libavcodec, can be set by the user to propagate the
     *   color range to components reading from the decoder context.
     */
    enum AVColorRange color_range;

    /**
     * This defines the location of chroma samples.
     * - encoding: Set by user
     * - decoding: Set by libavcodec
     */
    enum AVChromaLocation chroma_sample_location;

    /** Field order
     * - encoding: set by libavcodec
     * - decoding: Set by user.
     */
    enum AVFieldOrder field_order;

    /**
     * number of reference frames
     * - encoding: Set by user.
     * - decoding: Set by lavc.
     */
    int refs;

    /**
     * Size of the frame reordering buffer in the decoder.
     * For MPEG-2 it is 1 IPB or 0 low delay IP.
     * - encoding: Set by libavcodec.
     * - decoding: Set by libavcodec.
     */
    int has_b_frames;

    /**
     * slice flags
     * - encoding: unused
     * - decoding: Set by user.
     */
    int slice_flags;
#define SLICE_FLAG_CODED_ORDER    0x0001 ///< draw_horiz_band() is called in coded order instead of display
#define SLICE_FLAG_ALLOW_FIELD    0x0002 ///< allow draw_horiz_band() with field slices (MPEG-2 field pics)
#define SLICE_FLAG_ALLOW_PLANE    0x0004 ///< allow draw_horiz_band() with 1 component at a time (SVQ1)

    /**
     * If non NULL, 'draw_horiz_band' is called by the libavcodec
     * decoder to draw a horizontal band. It improves cache usage. Not
     * all codecs can do that. You must check the codec capabilities
     * beforehand.
     * When multithreading is used, it may be called from multiple threads
     * at the same time; threads might draw different parts of the same AVFrame,
     * or multiple AVFrames, and there is no guarantee that slices will be drawn
     * in order.
     * The function is also used by hardware acceleration APIs.
     * It is called at least once during frame decoding to pass
     * the data needed for hardware render.
     * In that mode instead of pixel data, AVFrame points to
     * a structure specific to the acceleration API. The application
     * reads the structure and can change some fields to indicate progress
     * or mark state.
     * - encoding: unused
     * - decoding: Set by user.
     * @param height the height of the slice
     * @param y the y position of the slice
     * @param type 1->top field, 2->bottom field, 3->frame
     * @param offset offset into the AVFrame.data from which the slice should be read
     */
    void (*draw_horiz_band)(struct AVCodecContext *s,
                            const AVFrame *src, int offset[AV_NUM_DATA_POINTERS],
                            int y, int type, int height);

    enum AVPixelFormat (*get_format)(struct AVCodecContext *s, const enum AVPixelFormat * fmt);

    /**
     * maximum number of B-frames between non-B-frames
     * Note: The output will be delayed by max_b_frames+1 relative to the input.
     * - encoding: Set by user.
     * - decoding: unused
     */
    int max_b_frames;

    /**
     * qscale factor between IP and B-frames
     * If > 0 then the last P-frame quantizer will be used (q= lastp_q*factor+offset).
     * If < 0 then normal ratecontrol will be done (q= -normal_q*factor+offset).
     * - encoding: Set by user.
     * - decoding: unused
     */
    float b_quant_factor;

    /**
     * qscale offset between IP and B-frames
     * - encoding: Set by user.
     * - decoding: unused
     */
    float b_quant_offset;

    /**
     * qscale factor between P- and I-frames
     * If > 0 then the last P-frame quantizer will be used (q = lastp_q * factor + offset).
     * If < 0 then normal ratecontrol will be done (q= -normal_q*factor+offset).
     * - encoding: Set by user.
     * - decoding: unused
     */
    float i_quant_factor;

    /**
     * qscale offset between P and I-frames
     * - encoding: Set by user.
     * - decoding: unused
     */
    float i_quant_offset;

    /**
     * luminance masking (0-> disabled)
     * - encoding: Set by user.
     * - decoding: unused
     */
    float lumi_masking;

    /**
     * temporary complexity masking (0-> disabled)
     * - encoding: Set by user.
     * - decoding: unused
     */
    float temporal_cplx_masking;

    /**
     * spatial complexity masking (0-> disabled)
     * - encoding: Set by user.
     * - decoding: unused
     */
    float spatial_cplx_masking;

    /**
     * p block masking (0-> disabled)
     * - encoding: Set by user.
     * - decoding: unused
     */
    float p_masking;

    /**
     * darkness masking (0-> disabled)
     * - encoding: Set by user.
     * - decoding: unused
     */
    float dark_masking;

    /**
     * noise vs. sse weight for the nsse comparison function
     * - encoding: Set by user.
     * - decoding: unused
     */
     int nsse_weight;

    /**
     * motion estimation comparison function
     * - encoding: Set by user.
     * - decoding: unused
     */
    int me_cmp;
    /**
     * subpixel motion estimation comparison function
     * - encoding: Set by user.
     * - decoding: unused
     */
    int me_sub_cmp;
    /**
     * macroblock comparison function (not supported yet)
     * - encoding: Set by user.
     * - decoding: unused
     */
    int mb_cmp;
    /**
     * interlaced DCT comparison function
     * - encoding: Set by user.
     * - decoding: unused
     */
    int ildct_cmp;
#define FF_CMP_SAD          0
#define FF_CMP_SSE          1
#define FF_CMP_SATD         2
#define FF_CMP_DCT          3
#define FF_CMP_PSNR         4
#define FF_CMP_BIT          5
#define FF_CMP_RD           6
#define FF_CMP_ZERO         7
#define FF_CMP_VSAD         8
#define FF_CMP_VSSE         9
#define FF_CMP_NSSE         10
#define FF_CMP_W53          11
#define FF_CMP_W97          12
#define FF_CMP_DCTMAX       13
#define FF_CMP_DCT264       14
#define FF_CMP_MEDIAN_SAD   15
#define FF_CMP_CHROMA       256

    /**
     * ME diamond size & shape
     * - encoding: Set by user.
     * - decoding: unused
     */
    int dia_size;

    /**
     * amount of previous MV predictors (2a+1 x 2a+1 square)
     * - encoding: Set by user.
     * - decoding: unused
     */
    int last_predictor_count;

    /**
     * motion estimation prepass comparison function
     * - encoding: Set by user.
     * - decoding: unused
     */
    int me_pre_cmp;

    /**
     * ME prepass diamond size & shape
     * - encoding: Set by user.
     * - decoding: unused
     */
    int pre_dia_size;

    /**
     * subpel ME quality
     * - encoding: Set by user.
     * - decoding: unused
     */
    int me_subpel_quality;

    /**
     * maximum motion estimation search range in subpel units
     * If 0 then no limit.
     *
     * - encoding: Set by user.
     * - decoding: unused
     */
    int me_range;

    /**
     * macroblock decision mode
     * - encoding: Set by user.
     * - decoding: unused
     */
    int mb_decision;
#define FF_MB_DECISION_SIMPLE 0        ///< uses mb_cmp
#define FF_MB_DECISION_BITS   1        ///< chooses the one which needs the fewest bits
#define FF_MB_DECISION_RD     2        ///< rate distortion

    /**
     * custom intra quantization matrix
     * Must be allocated with the av_malloc() family of functions, and will be freed in
     * avcodec_free_context().
     * - encoding: Set/allocated by user, freed by libavcodec. Can be NULL.
     * - decoding: Set/allocated/freed by libavcodec.
     */
    uint16_t *intra_matrix;

    /**
     * custom inter quantization matrix
     * Must be allocated with the av_malloc() family of functions, and will be freed in
     * avcodec_free_context().
     * - encoding: Set/allocated by user, freed by libavcodec. Can be NULL.
     * - decoding: Set/allocated/freed by libavcodec.
     */
    uint16_t *inter_matrix;

    /**
     * custom intra quantization matrix
     * - encoding: Set by user, can be NULL.
     * - decoding: unused.
     */
    uint16_t *chroma_intra_matrix;

#if FF_API_INTRA_DC_PRECISION
    /**
     * precision of the intra DC coefficient - 8
     * - encoding: Set by user.
     * - decoding: Set by libavcodec
     * @deprecated Use the MPEG-2 encoder's private option "intra_dc_precision" instead.
     */
    attribute_deprecated
    int intra_dc_precision;
#endif

    /**
     * minimum MB Lagrange multiplier
     * - encoding: Set by user.
     * - decoding: unused
     */
    int mb_lmin;

    /**
     * maximum MB Lagrange multiplier
     * - encoding: Set by user.
     * - decoding: unused
     */
    int mb_lmax;

    /**
     * - encoding: Set by user.
     * - decoding: unused
     */
    int bidir_refine;

    /**
     * minimum GOP size
     * - encoding: Set by user.
     * - decoding: unused
     */
    int keyint_min;

    /**
     * the number of pictures in a group of pictures, or 0 for intra_only
     * - encoding: Set by user.
     * - decoding: unused
     */
    int gop_size;

    /**
     * Note: Value depends upon the compare function used for fullpel ME.
     * - encoding: Set by user.
     * - decoding: unused
     */
    int mv0_threshold;

    /**
     * Number of slices.
     * Indicates number of picture subdivisions. Used for parallelized
     * decoding.
     * - encoding: Set by user
     * - decoding: unused
     */
    int slices;

    int sample_rate; 

    /**
     * audio sample format
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     */
    enum AVSampleFormat sample_fmt;  

    /**
     * Audio channel layout.
     * - encoding: must be set by the caller, to one of AVCodec.ch_layouts.
     * - decoding: may be set by the caller if known e.g. from the container.
     *             The decoder can then override during decoding as needed.
     */
    AVChannelLayout ch_layout;

    /**
     * Number of samples per channel in an audio frame.
     *
     * - encoding: set by libavcodec in avcodec_open2(). Each submitted frame
     *   except the last must contain exactly frame_size samples per channel.
     *   May be 0 when the codec has AV_CODEC_CAP_VARIABLE_FRAME_SIZE set, then the
     *   frame size is not restricted.
     * - decoding: may be set by some decoders to indicate constant frame size
     */
    int frame_size;

    int block_align;

    /**
     * Audio cutoff bandwidth (0 means "automatic")
     * - encoding: Set by user.
     * - decoding: unused
     */
    int cutoff;

    /**
     * Type of service that the audio stream conveys.
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     */
    enum AVAudioServiceType audio_service_type;

    /**
     * desired sample format
     * - encoding: Not used.
     * - decoding: Set by user.
     * Decoder will decode to this format if it can.
     */
    enum AVSampleFormat request_sample_fmt;

    /**
     * Audio only. The number of "priming" samples (padding) inserted by the
     * encoder at the beginning of the audio. I.e. this number of leading
     * decoded samples must be discarded by the caller to get the original audio
     * without leading padding.
     *
     * - decoding: unused
     * - encoding: Set by libavcodec. The timestamps on the output packets are
     *             adjusted by the encoder so that they always refer to the
     *             first sample of the data actually contained in the packet,
     *             including any added padding.  E.g. if the timebase is
     *             1/samplerate and the timestamp of the first input sample is
     *             0, the timestamp of the first output packet will be
     *             -initial_padding.
     */
    int initial_padding;

    /**
     * Audio only. The amount of padding (in samples) appended by the encoder to
     * the end of the audio. I.e. this number of decoded samples must be
     * discarded by the caller from the end of the stream to get the original
     * audio without any trailing padding.
     *
     * - decoding: unused
     * - encoding: unused
     */
    int trailing_padding;

    /**
     * Number of samples to skip after a discontinuity
     * - decoding: unused
     * - encoding: set by libavcodec
     */
    int seek_preroll;

    /**
     * This callback is called at the beginning of each frame to get data
     * buffer(s) for it. There may be one contiguous buffer for all the data or
     * there may be a buffer per each data plane or anything in between. What
     * this means is, you may set however many entries in buf[] you feel necessary.
     * Each buffer must be reference-counted using the AVBuffer API (see description
     * of buf[] below).
     *
     * The following fields will be set in the frame before this callback is
     * called:
     * - format
     * - width, height (video only)
     * - sample_rate, channel_layout, nb_samples (audio only)
     * Their values may differ from the corresponding values in
     * AVCodecContext. This callback must use the frame values, not the codec
     * context values, to calculate the required buffer size.
     *
     * This callback must fill the following fields in the frame:
     * - data[]
     * - linesize[]
     * - extended_data:
     *   * if the data is planar audio with more than 8 channels, then this
     *     callback must allocate and fill extended_data to contain all pointers
     *     to all data planes. data[] must hold as many pointers as it can.
     *     extended_data must be allocated with av_malloc() and will be freed in
     *     av_frame_unref().
     *   * otherwise extended_data must point to data
     * - buf[] must contain one or more pointers to AVBufferRef structures. Each of
     *   the frame's data and extended_data pointers must be contained in these. That
     *   is, one AVBufferRef for each allocated chunk of memory, not necessarily one
     *   AVBufferRef per data[] entry. See: av_buffer_create(), av_buffer_alloc(),
     *   and av_buffer_ref().
     * - extended_buf and nb_extended_buf must be allocated with av_malloc() by
     *   this callback and filled with the extra buffers if there are more
     *   buffers than buf[] can hold. extended_buf will be freed in
     *   av_frame_unref().
     *   Decoders will generally initialize the whole buffer before it is output
     *   but it can in rare error conditions happen that uninitialized data is passed
     *   through. \important The buffers returned by get_buffer* should thus not contain sensitive
     *   data.
     *
     * If AV_CODEC_CAP_DR1 is not set then get_buffer2() must call
     * avcodec_default_get_buffer2() instead of providing buffers allocated by
     * some other means.
     *
     * Each data plane must be aligned to the maximum required by the target
     * CPU.
     *
     * @see avcodec_default_get_buffer2()
     *
     * Video:
     *
     * If AV_GET_BUFFER_FLAG_REF is set in flags then the frame may be reused
     * (read and/or written to if it is writable) later by libavcodec.
     *
     * avcodec_align_dimensions2() should be used to find the required width and
     * height, as they normally need to be rounded up to the next multiple of 16.
     *
     * Some decoders do not support linesizes changing between frames.
     *
     * If frame multithreading is used, this callback may be called from a
     * different thread, but not from more than one at once. Does not need to be
     * reentrant.
     *
     * @see avcodec_align_dimensions2()
     *
     * Audio:
     *
     * Decoders request a buffer of a particular size by setting
     * AVFrame.nb_samples prior to calling get_buffer2(). The decoder may,
     * however, utilize only part of the buffer by setting AVFrame.nb_samples
     * to a smaller value in the output frame.
     *
     * As a convenience, av_samples_get_buffer_size() and
     * av_samples_fill_arrays() in libavutil may be used by custom get_buffer2()
     * functions to find the required data size and to fill data pointers and
     * linesize. In AVFrame.linesize, only linesize[0] may be set for audio
     * since all planes must be the same size.
     *
     * @see av_samples_get_buffer_size(), av_samples_fill_arrays()
     *
     * - encoding: unused
     * - decoding: Set by libavcodec, user can override.
     */
    int (*get_buffer2)(struct AVCodecContext *s, AVFrame *frame, int flags);

    /**
     * number of bits the bitstream is allowed to diverge from the reference.
     *           the reference can be CBR (for CBR pass1) or VBR (for pass2)
     * - encoding: Set by user; unused for constant quantizer encoding.
     * - decoding: unused
     */
    int bit_rate_tolerance;

    /**
     * Global quality for codecs which cannot change it per frame.
     * This should be proportional to MPEG-1/2/4 qscale.
     * - encoding: Set by user.
     * - decoding: unused
     */
    int global_quality;

    /**
     * - encoding: Set by user.
     * - decoding: unused
     */
    int compression_level;
#define FF_COMPRESSION_DEFAULT -1

    float qcompress;  
    float qblur;      

    /**
     * minimum quantizer
     * - encoding: Set by user.
     * - decoding: unused
     */
    int qmin;

    /**
     * maximum quantizer
     * - encoding: Set by user.
     * - decoding: unused
     */
    int qmax;

    /**
     * maximum quantizer difference between frames
     * - encoding: Set by user.
     * - decoding: unused
     */
    int max_qdiff;

    /**
     * decoder bitstream buffer size
     * - encoding: Set by user.
     * - decoding: May be set by libavcodec.
     */
    int rc_buffer_size;

    /**
     * ratecontrol override, see RcOverride
     * - encoding: Allocated/set/freed by user.
     * - decoding: unused
     */
    int rc_override_count;
    RcOverride *rc_override;

    /**
     * maximum bitrate
     * - encoding: Set by user.
     * - decoding: Set by user, may be overwritten by libavcodec.
     */
    int64_t rc_max_rate;

    /**
     * minimum bitrate
     * - encoding: Set by user.
     * - decoding: unused
     */
    int64_t rc_min_rate;

    /**
     * Ratecontrol attempt to use, at maximum, <value> of what can be used without an underflow.
     * - encoding: Set by user.
     * - decoding: unused.
     */
    float rc_max_available_vbv_use;

    /**
     * Ratecontrol attempt to use, at least, <value> times the amount needed to prevent a vbv overflow.
     * - encoding: Set by user.
     * - decoding: unused.
     */
    float rc_min_vbv_overflow_use;

    /**
     * Number of bits which should be loaded into the rc buffer before decoding starts.
     * - encoding: Set by user.
     * - decoding: unused
     */
    int rc_initial_buffer_occupancy;

    /**
     * trellis RD quantization
     * - encoding: Set by user.
     * - decoding: unused
     */
    int trellis;

    /**
     * pass1 encoding statistics output buffer
     * - encoding: Set by libavcodec.
     * - decoding: unused
     */
    char *stats_out;

    /**
     * pass2 encoding statistics input buffer
     * Concatenated stuff from stats_out of pass1 should be placed here.
     * - encoding: Allocated/set/freed by user.
     * - decoding: unused
     */
    char *stats_in;

    /**
     * Work around bugs in encoders which sometimes cannot be detected automatically.
     * - encoding: Set by user
     * - decoding: Set by user
     */
    int workaround_bugs;
#define FF_BUG_AUTODETECT       1  ///< autodetection
#define FF_BUG_XVID_ILACE       4
#define FF_BUG_UMP4             8
#define FF_BUG_NO_PADDING       16
#define FF_BUG_AMV              32
#define FF_BUG_QPEL_CHROMA      64
#define FF_BUG_STD_QPEL         128
#define FF_BUG_QPEL_CHROMA2     256
#define FF_BUG_DIRECT_BLOCKSIZE 512
#define FF_BUG_EDGE             1024
#define FF_BUG_HPEL_CHROMA      2048
#define FF_BUG_DC_CLIP          4096
#define FF_BUG_MS               8192 ///< Work around various bugs in Microsoft's broken decoders.
#define FF_BUG_TRUNCATED       16384
#define FF_BUG_IEDGE           32768

    /**
     * strictly follow the standard (MPEG-4, ...).
     * - encoding: Set by user.
     * - decoding: Set by user.
     * Setting this to STRICT or higher means the encoder and decoder will
     * generally do stupid things, whereas setting it to unofficial or lower
     * will mean the encoder might produce output that is not supported by all
     * spec-compliant decoders. Decoders don't differentiate between normal,
     * unofficial and experimental (that is, they always try to decode things
     * when they can) unless they are explicitly asked to behave stupidly
     * (=strictly conform to the specs)
     * This may only be set to one of the FF_COMPLIANCE_* values in defs.h.
     */
    int strict_std_compliance;

    /**
     * error concealment flags
     * - encoding: unused
     * - decoding: Set by user.
     */
    int error_concealment;
#define FF_EC_GUESS_MVS   1
#define FF_EC_DEBLOCK     2
#define FF_EC_FAVOR_INTER 256

    /**
     * debug
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int debug;
#define FF_DEBUG_PICT_INFO   1
#define FF_DEBUG_RC          2
#define FF_DEBUG_BITSTREAM   4
#define FF_DEBUG_MB_TYPE     8
#define FF_DEBUG_QP          16
#define FF_DEBUG_DCT_COEFF   0x00000040
#define FF_DEBUG_SKIP        0x00000080
#define FF_DEBUG_STARTCODE   0x00000100
#define FF_DEBUG_ER          0x00000400
#define FF_DEBUG_MMCO        0x00000800
#define FF_DEBUG_BUGS        0x00001000
#define FF_DEBUG_BUFFERS     0x00008000
#define FF_DEBUG_THREADS     0x00010000
#define FF_DEBUG_GREEN_MD    0x00800000
#define FF_DEBUG_NOMC        0x01000000

    /**
     * Error recognition; may misdetect some more or less valid parts as errors.
     * This is a bitfield of the AV_EF_* values defined in defs.h.
     *
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int err_recognition;

    /**
     * Hardware accelerator in use
     * - encoding: unused.
     * - decoding: Set by libavcodec
     */
    const struct AVHWAccel *hwaccel;

    /**
     * Legacy hardware accelerator context.
     *
     * For some hardware acceleration methods, the caller may use this field to
     * signal hwaccel-specific data to the codec. The struct pointed to by this
     * pointer is hwaccel-dependent and defined in the respective header. Please
     * refer to the FFmpeg HW accelerator documentation to know how to fill
     * this.
     *
     * In most cases this field is optional - the necessary information may also
     * be provided to libavcodec through @ref hw_frames_ctx or @ref
     * hw_device_ctx (see avcodec_get_hw_config()). However, in some cases it
     * may be the only method of signalling some (optional) information.
     *
     * The struct and its contents are owned by the caller.
     *
     * - encoding: May be set by the caller before avcodec_open2(). Must remain
     *             valid until avcodec_free_context().
     * - decoding: May be set by the caller in the get_format() callback.
     *             Must remain valid until the next get_format() call,
     *             or avcodec_free_context() (whichever comes first).
     */
    void *hwaccel_context;

    /**
     * A reference to the AVHWFramesContext describing the input (for encoding)
     * or output (decoding) frames. The reference is set by the caller and
     * afterwards owned (and freed) by libavcodec - it should never be read by
     * the caller after being set.
     *
     * - decoding: This field should be set by the caller from the get_format()
     *             callback. The previous reference (if any) will always be
     *             unreffed by libavcodec before the get_format() call.
     *
     *             If the default get_buffer2() is used with a hwaccel pixel
     *             format, then this AVHWFramesContext will be used for
     *             allocating the frame buffers.
     *
     * - encoding: For hardware encoders configured to use a hwaccel pixel
     *             format, this field should be set by the caller to a reference
     *             to the AVHWFramesContext describing input frames.
     *             AVHWFramesContext.format must be equal to
     *             AVCodecContext.pix_fmt.
     *
     *             This field should be set before avcodec_open2() is called.
     */
    AVBufferRef *hw_frames_ctx;

    AVBufferRef *hw_device_ctx;

    /**
     * Bit set of AV_HWACCEL_FLAG_* flags, which affect hardware accelerated
     * decoding (if active).
     * - encoding: unused
     * - decoding: Set by user (either before avcodec_open2(), or in the
     *             AVCodecContext.get_format callback)
     */
    int hwaccel_flags;

    int extra_hw_frames;

    /**
     * error
     * - encoding: Set by libavcodec if flags & AV_CODEC_FLAG_PSNR.
     * - decoding: unused
     */
    uint64_t error[AV_NUM_DATA_POINTERS];

    /**
     * DCT algorithm, see FF_DCT_* below
     * - encoding: Set by user.
     * - decoding: unused
     */
    int dct_algo;
#define FF_DCT_AUTO    0
#define FF_DCT_FASTINT 1
#define FF_DCT_INT     2
#define FF_DCT_MMX     3
#define FF_DCT_ALTIVEC 5
#define FF_DCT_FAAN    6
#define FF_DCT_NEON    7

    /**
     * IDCT algorithm, see FF_IDCT_* below.
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int idct_algo;
#define FF_IDCT_AUTO          0
#define FF_IDCT_INT           1
#define FF_IDCT_SIMPLE        2
#define FF_IDCT_SIMPLEMMX     3
#define FF_IDCT_ARM           7
#define FF_IDCT_ALTIVEC       8
#define FF_IDCT_SIMPLEARM     10
#define FF_IDCT_XVID          14
#define FF_IDCT_SIMPLEARMV5TE 16
#define FF_IDCT_SIMPLEARMV6   17
#define FF_IDCT_FAAN          20
#define FF_IDCT_SIMPLENEON    22
#define FF_IDCT_SIMPLEAUTO    128

    /**
     * bits per sample/pixel from the demuxer (needed for huffyuv).
     * - encoding: Set by libavcodec.
     * - decoding: Set by user.
     */
     int bits_per_coded_sample;

    /**
     * Bits per sample/pixel of internal libavcodec pixel/sample format.
     * - encoding: set by user.
     * - decoding: set by libavcodec.
     */
    int bits_per_raw_sample;

    /**
     * thread count
     * is used to decide how many independent tasks should be passed to execute()
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    int thread_count;

    /**
     * Which multithreading methods to use.
     * Use of FF_THREAD_FRAME will increase decoding delay by one frame per thread,
     * so clients which cannot provide future frames should not use it.
     *
     * - encoding: Set by user, otherwise the default is used.
     * - decoding: Set by user, otherwise the default is used.
     */
    int thread_type;
#define FF_THREAD_FRAME   1 ///< Decode more than one frame at once
#define FF_THREAD_SLICE   2 ///< Decode more than one part of a single frame at once

    /**
     * Which multithreading methods are in use by the codec.
     * - encoding: Set by libavcodec.
     * - decoding: Set by libavcodec.
     */
    int active_thread_type;

    /**
     * The codec may call this to execute several independent things.
     * It will return only after finishing all tasks.
     * The user may replace this with some multithreaded implementation,
     * the default implementation will execute the parts serially.
     * @param count the number of things to execute
     * - encoding: Set by libavcodec, user can override.
     * - decoding: Set by libavcodec, user can override.
     */
    int (*execute)(struct AVCodecContext *c, int (*func)(struct AVCodecContext *c2, void *arg), void *arg2, int *ret, int count, int size);

    /**
     * The codec may call this to execute several independent things.
     * It will return only after finishing all tasks.
     * The user may replace this with some multithreaded implementation,
     * the default implementation will execute the parts serially.
     * @param c context passed also to func
     * @param count the number of things to execute
     * @param arg2 argument passed unchanged to func
     * @param ret return values of executed functions, must have space for "count" values. May be NULL.
     * @param func function that will be called count times, with jobnr from 0 to count-1.
     *             threadnr will be in the range 0 to c->thread_count-1 < MAX_THREADS and so that no
     *             two instances of func executing at the same time will have the same threadnr.
     * @return always 0 currently, but code should handle a future improvement where when any call to func
     *         returns < 0 no further calls to func may be done and < 0 is returned.
     * - encoding: Set by libavcodec, user can override.
     * - decoding: Set by libavcodec, user can override.
     */
    int (*execute2)(struct AVCodecContext *c, int (*func)(struct AVCodecContext *c2, void *arg, int jobnr, int threadnr), void *arg2, int *ret, int count);

    /**
     * profile
     * - encoding: Set by user.
     * - decoding: Set by libavcodec.
     * See the AV_PROFILE_* defines in defs.h.
     */
     int profile;

    /**
     * Encoding level descriptor.
     * - encoding: Set by user, corresponds to a specific level defined by the
     *   codec, usually corresponding to the profile level, if not specified it
     *   is set to AV_LEVEL_UNKNOWN.
     * - decoding: Set by libavcodec.
     * See AV_LEVEL_* in defs.h.
     */
     int level;

#if FF_API_CODEC_PROPS
    /**
     * Properties of the stream that gets decoded
     * - encoding: unused
     * - decoding: set by libavcodec
     */
    attribute_deprecated
    unsigned properties;
#define FF_CODEC_PROPERTY_LOSSLESS        0x00000001
#define FF_CODEC_PROPERTY_CLOSED_CAPTIONS 0x00000002
#define FF_CODEC_PROPERTY_FILM_GRAIN      0x00000004
#endif

    /**
     * Skip loop filtering for selected frames.
     * - encoding: unused
     * - decoding: Set by user.
     */
    enum AVDiscard skip_loop_filter;

    /**
     * Skip IDCT/dequantization for selected frames.
     * - encoding: unused
     * - decoding: Set by user.
     */
    enum AVDiscard skip_idct;

    /**
     * Skip decoding for selected frames.
     * - encoding: unused
     * - decoding: Set by user.
     */
    enum AVDiscard skip_frame;

    /**
     * Skip processing alpha if supported by codec.
     * Note that if the format uses pre-multiplied alpha (common with VP6,
     * and recommended due to better video quality/compression)
     * the image will look as if alpha-blended onto a black background.
     * However for formats that do not use pre-multiplied alpha
     * there might be serious artefacts (though e.g. libswscale currently
     * assumes pre-multiplied alpha anyway).
     *
     * - decoding: set by user
     * - encoding: unused
     */
    int skip_alpha;

    /**
     * Number of macroblock rows at the top which are skipped.
     * - encoding: unused
     * - decoding: Set by user.
     */
    int skip_top;

    /**
     * Number of macroblock rows at the bottom which are skipped.
     * - encoding: unused
     * - decoding: Set by user.
     */
    int skip_bottom;

    /**
     * low resolution decoding, 1-> 1/2 size, 2->1/4 size
     * - encoding: unused
     * - decoding: Set by user.
     */
     int lowres;

    /**
     * AVCodecDescriptor
     * - encoding: unused.
     * - decoding: set by libavcodec.
     */
    const struct AVCodecDescriptor *codec_descriptor;

    /**
     * Character encoding of the input subtitles file.
     * - decoding: set by user
     * - encoding: unused
     */
    char *sub_charenc;

    /**
     * Subtitles character encoding mode. Formats or codecs might be adjusting
     * this setting (if they are doing the conversion themselves for instance).
     * - decoding: set by libavcodec
     * - encoding: unused
     */
    int sub_charenc_mode;
#define FF_SUB_CHARENC_MODE_DO_NOTHING  -1  ///< do nothing (demuxer outputs a stream supposed to be already in UTF-8, or the codec is bitmap for instance)
#define FF_SUB_CHARENC_MODE_AUTOMATIC    0  ///< libavcodec will select the mode itself
#define FF_SUB_CHARENC_MODE_PRE_DECODER  1  ///< the AVPacket data needs to be recoded to UTF-8 before being fed to the decoder, requires iconv
#define FF_SUB_CHARENC_MODE_IGNORE       2  ///< neither convert the subtitles, nor check them for valid UTF-8

    /**
     * Header containing style information for text subtitles.
     * For SUBTITLE_ASS subtitle type, it should contain the whole ASS
     * [Script Info] and [V4+ Styles] section, plus the [Events] line and
     * the Format line following. It shouldn't include any Dialogue line.
     *
     * - encoding: May be set by the caller before avcodec_open2() to an array
     *   allocated with the av_malloc() family of functions.
     * - decoding: May be set by libavcodec in avcodec_open2().
     *
     * After being set, the array is owned by the codec and freed in
     * avcodec_free_context().
     */
    int subtitle_header_size;
    uint8_t *subtitle_header;

    /**
     * dump format separator.
     * can be ", " or "\n      " or anything else
     * - encoding: Set by user.
     * - decoding: Set by user.
     */
    uint8_t *dump_separator;

    /**
     * ',' separated list of allowed decoders.
     * If NULL then all are allowed
     * - encoding: unused
     * - decoding: set by user
     */
    char *codec_whitelist;

    /**
     * Additional data associated with the entire coded stream.
     *
     * - decoding: may be set by user before calling avcodec_open2().
     * - encoding: may be set by libavcodec after avcodec_open2().
     */
    AVPacketSideData *coded_side_data;
    int            nb_coded_side_data;

    /**
     * Bit set of AV_CODEC_EXPORT_DATA_* flags, which affects the kind of
     * metadata exported in frame, packet, or coded stream side data by
     * decoders and encoders.
     *
     * - decoding: set by user
     * - encoding: set by user
     */
    int export_side_data;

    /**
     * The number of pixels per image to maximally accept.
     *
     * - decoding: set by user
     * - encoding: set by user
     */
    int64_t max_pixels;

    int apply_cropping;

    /**
     * The percentage of damaged samples to discard a frame.
     *
     * - decoding: set by user
     * - encoding: unused
     */
    int discard_damaged_percentage;

    /**
     * The number of samples per frame to maximally accept.
     *
     * - decoding: set by user
     * - encoding: set by user
     */
    int64_t max_samples;

    /**
     * This callback is called at the beginning of each packet to get a data
     * buffer for it.
     *
     * The following field will be set in the packet before this callback is
     * called:
     * - size
     * This callback must use the above value to calculate the required buffer size,
     * which must padded by at least AV_INPUT_BUFFER_PADDING_SIZE bytes.
     *
     * In some specific cases, the encoder may not use the entire buffer allocated by this
     * callback. This will be reflected in the size value in the packet once returned by
     * avcodec_receive_packet().
     *
     * This callback must fill the following fields in the packet:
     * - data: alignment requirements for AVPacket apply, if any. Some architectures and
     *   encoders may benefit from having aligned data.
     * - buf: must contain a pointer to an AVBufferRef structure. The packet's
     *   data pointer must be contained in it. See: av_buffer_create(), av_buffer_alloc(),
     *   and av_buffer_ref().
     *
     * If AV_CODEC_CAP_DR1 is not set then get_encode_buffer() must call
     * avcodec_default_get_encode_buffer() instead of providing a buffer allocated by
     * some other means.
     *
     * The flags field may contain a combination of AV_GET_ENCODE_BUFFER_FLAG_ flags.
     * They may be used for example to hint what use the buffer may get after being
     * created.
     * Implementations of this callback may ignore flags they don't understand.
     * If AV_GET_ENCODE_BUFFER_FLAG_REF is set in flags then the packet may be reused
     * (read and/or written to if it is writable) later by libavcodec.
     *
     * This callback must be thread-safe, as when frame threading is used, it may
     * be called from multiple threads simultaneously.
     *
     * @see avcodec_default_get_encode_buffer()
     *
     * - encoding: Set by libavcodec, user can override.
     * - decoding: unused
     */
    int (*get_encode_buffer)(struct AVCodecContext *s, AVPacket *pkt, int flags);

    /**
     * Frame counter, set by libavcodec.
     *
     * - decoding: total number of frames returned from the decoder so far.
     * - encoding: total number of frames passed to the encoder so far.
     *
     *   @note the counter is not incremented if encoding/decoding resulted in
     *   an error.
     */
    int64_t frame_num;

    int        *side_data_prefer_packet;
    unsigned nb_side_data_prefer_packet;

    /**
     * Array containing static side data, such as HDR10 CLL / MDCV structures.
     * Side data entries should be allocated by usage of helpers defined in
     * libavutil/frame.h.
     *
     * - encoding: may be set by user before calling avcodec_open2() for
     *             encoder configuration. Afterwards owned and freed by the
     *             encoder.
     * - decoding: may be set by libavcodec in avcodec_open2().
     */
    AVFrameSideData  **decoded_side_data;
    int             nb_decoded_side_data;

    /**
     * Indicates how the alpha channel of the video is represented.
     * - encoding: Set by user
     * - decoding: Set by libavcodec
     */
    enum AVAlphaMode alpha_mode;

    int moz_extradata_offset;

    void* moz_ndk_crypto;
} AVCodecContext;

typedef struct AVHWAccel {
    const char *name;

    enum AVMediaType type;

    enum AVCodecID id;

    enum AVPixelFormat pix_fmt;

    int capabilities;
} AVHWAccel;

#define AV_HWACCEL_CODEC_CAP_EXPERIMENTAL 0x0200

#define AV_HWACCEL_FLAG_IGNORE_LEVEL (1 << 0)

#define AV_HWACCEL_FLAG_ALLOW_HIGH_DEPTH (1 << 1)

#define AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH (1 << 2)

#define AV_HWACCEL_FLAG_UNSAFE_OUTPUT (1 << 3)


enum AVSubtitleType {
    SUBTITLE_NONE,

    SUBTITLE_BITMAP,                

    SUBTITLE_TEXT,

    SUBTITLE_ASS,
};

#define AV_SUBTITLE_FLAG_FORCED 0x00000001

typedef struct AVSubtitleRect {
    int x;         
    int y;         
    int w;         
    int h;         
    int nb_colors; 

    uint8_t *data[4];
    int linesize[4];

    int flags;
    enum AVSubtitleType type;

    char *text;                     

    char *ass;
} AVSubtitleRect;

typedef struct AVSubtitle {
    uint16_t format; 
    uint32_t start_display_time; 
    uint32_t end_display_time; 
    unsigned num_rects;
    AVSubtitleRect **rects;
    int64_t pts;    
} AVSubtitle;

unsigned avcodec_version(void);

const char *avcodec_configuration(void);

/**
 * Return the libavcodec license.
 */
const char *avcodec_license(void);

AVCodecContext *avcodec_alloc_context3(const AVCodec *codec);

void avcodec_free_context(AVCodecContext **avctx);

const AVClass *avcodec_get_class(void);

const AVClass *avcodec_get_subtitle_rect_class(void);

int avcodec_parameters_from_context(struct AVCodecParameters *par,
                                    const AVCodecContext *codec);

int avcodec_parameters_to_context(AVCodecContext *codec,
                                  const struct AVCodecParameters *par);

int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);

void avsubtitle_free(AVSubtitle *sub);



int avcodec_default_get_buffer2(AVCodecContext *s, AVFrame *frame, int flags);

int avcodec_default_get_encode_buffer(AVCodecContext *s, AVPacket *pkt, int flags);

void avcodec_align_dimensions(AVCodecContext *s, int *width, int *height);

void avcodec_align_dimensions2(AVCodecContext *s, int *width, int *height,
                               int linesize_align[AV_NUM_DATA_POINTERS]);

int avcodec_decode_subtitle2(AVCodecContext *avctx, AVSubtitle *sub,
                             int *got_sub_ptr, const AVPacket *avpkt);

int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt);

int avcodec_receive_frame_flags(AVCodecContext *avctx, AVFrame *frame, unsigned flags);

int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame);

int avcodec_send_frame(AVCodecContext *avctx, const AVFrame *frame);

int avcodec_receive_packet(AVCodecContext *avctx, AVPacket *avpkt);

int avcodec_get_hw_frames_parameters(AVCodecContext *avctx,
                                     AVBufferRef *device_ref,
                                     enum AVPixelFormat hw_pix_fmt,
                                     AVBufferRef **out_frames_ref);

enum AVCodecConfig {
    AV_CODEC_CONFIG_PIX_FORMAT,     
    AV_CODEC_CONFIG_FRAME_RATE,     
    AV_CODEC_CONFIG_SAMPLE_RATE,    
    AV_CODEC_CONFIG_SAMPLE_FORMAT,  
    AV_CODEC_CONFIG_CHANNEL_LAYOUT, 
    AV_CODEC_CONFIG_COLOR_RANGE,    
    AV_CODEC_CONFIG_COLOR_SPACE,    
    AV_CODEC_CONFIG_ALPHA_MODE,     
};

int avcodec_get_supported_config(const AVCodecContext *avctx,
                                 const AVCodec *codec, enum AVCodecConfig config,
                                 unsigned flags, const void **out_configs,
                                 int *out_num_configs);




enum AVPictureStructure {
    AV_PICTURE_STRUCTURE_UNKNOWN,      
    AV_PICTURE_STRUCTURE_TOP_FIELD,    
    AV_PICTURE_STRUCTURE_BOTTOM_FIELD, 
    AV_PICTURE_STRUCTURE_FRAME,        
};

typedef struct AVCodecParserContext {
    void *priv_data;
    const struct AVCodecParser *parser;
    int64_t frame_offset; 
    int64_t cur_offset; 
    int64_t next_frame_offset; 
    int pict_type; 
    int repeat_pict; 
    int64_t pts;     
    int64_t dts;     

    int64_t last_pts;
    int64_t last_dts;
    int fetch_timestamp;

#define AV_PARSER_PTS_NB 4
    int cur_frame_start_index;
    int64_t cur_frame_offset[AV_PARSER_PTS_NB];
    int64_t cur_frame_pts[AV_PARSER_PTS_NB];
    int64_t cur_frame_dts[AV_PARSER_PTS_NB];

    int flags;
#define PARSER_FLAG_COMPLETE_FRAMES           0x0001
#define PARSER_FLAG_ONCE                      0x0002
#define PARSER_FLAG_FETCHED_OFFSET            0x0004
#define PARSER_FLAG_USE_CODEC_TS              0x1000

    int64_t offset;      
    int64_t cur_frame_end[AV_PARSER_PTS_NB];

    int key_frame;

    int dts_sync_point;

    int dts_ref_dts_delta;

    int pts_dts_delta;

    int64_t cur_frame_pos[AV_PARSER_PTS_NB];

    int64_t pos;

    int64_t last_pos;

    int duration;

    enum AVFieldOrder field_order;

    enum AVPictureStructure picture_structure;

    int output_picture_number;

    int width;
    int height;

    int coded_width;
    int coded_height;

    int format;
} AVCodecParserContext;

typedef struct AVCodecParser {
#if FF_API_PARSER_CODECID
    int codec_ids[7]; 
#else
    enum AVCodecID codec_ids[7]; 
#endif
#if FF_API_PARSER_PRIVATE
    attribute_deprecated
    int priv_data_size;
    attribute_deprecated
    int (*parser_init)(AVCodecParserContext *s);
    attribute_deprecated
    int (*parser_parse)(AVCodecParserContext *s,
                        AVCodecContext *avctx,
                        const uint8_t **poutbuf, int *poutbuf_size,
                        const uint8_t *buf, int buf_size);
    attribute_deprecated
    void (*parser_close)(AVCodecParserContext *s);
    attribute_deprecated
    int (*split)(AVCodecContext *avctx, const uint8_t *buf, int buf_size);
#endif
} AVCodecParser;

const AVCodecParser *av_parser_iterate(void **opaque);

#if FF_API_PARSER_CODECID
AVCodecParserContext *av_parser_init(int codec_id);
#else
AVCodecParserContext *av_parser_init(enum AVCodecID codec_id);
#endif

int av_parser_parse2(AVCodecParserContext *s,
                     AVCodecContext *avctx,
                     uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size,
                     int64_t pts, int64_t dts,
                     int64_t pos);

void av_parser_close(AVCodecParserContext *s);



int avcodec_encode_subtitle(AVCodecContext *avctx, uint8_t *buf, int buf_size,
                            const AVSubtitle *sub);





unsigned int avcodec_pix_fmt_to_codec_tag(enum AVPixelFormat pix_fmt);

enum AVPixelFormat avcodec_find_best_pix_fmt_of_list(const enum AVPixelFormat *pix_fmt_list,
                                            enum AVPixelFormat src_pix_fmt,
                                            int has_alpha, int *loss_ptr);

enum AVPixelFormat avcodec_default_get_format(struct AVCodecContext *s, const enum AVPixelFormat * fmt);


void avcodec_string(char *buf, int buf_size, AVCodecContext *enc, int encode);

int avcodec_default_execute(AVCodecContext *c, int (*func)(AVCodecContext *c2, void *arg2),void *arg, int *ret, int count, int size);
int avcodec_default_execute2(AVCodecContext *c, int (*func)(AVCodecContext *c2, void *arg2, int, int),void *arg, int *ret, int count);

int avcodec_fill_audio_frame(AVFrame *frame, int nb_channels,
                             enum AVSampleFormat sample_fmt, const uint8_t *buf,
                             int buf_size, int align);

void avcodec_flush_buffers(AVCodecContext *avctx);

int av_get_audio_frame_duration(AVCodecContext *avctx, int frame_bytes);


void av_fast_padded_malloc(void *ptr, unsigned int *size, size_t min_size);

void av_fast_padded_mallocz(void *ptr, unsigned int *size, size_t min_size);

int avcodec_is_open(AVCodecContext *s);


#endif /* AVCODEC_AVCODEC_H */
