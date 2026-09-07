/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*!\defgroup vp8_decoder WebM VP8/VP9 Decoder
 * \ingroup vp8
 *
 * @{
 */
/*!\file
 * \brief Provides definitions for using VP8 or VP9 within the vpx Decoder
 *        interface.
 */
#if !defined(VPX_VPX_VP8DX_H_)
#define VPX_VPX_VP8DX_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include "./vp8.h"

/*!\name Algorithm interface for VP8
 *
 * This interface provides the capability to decode VP8 streams.
 * @{
 */

/*!\brief A single instance of the VP8 decoder.
 *\deprecated This access mechanism is provided for backwards compatibility;
 * prefer vpx_codec_vp8_dx().
 */
extern vpx_codec_iface_t vpx_codec_vp8_dx_algo;

/*!\brief The interface to the VP8 decoder.
 */
extern vpx_codec_iface_t *vpx_codec_vp8_dx(void);
/*!@} - end algorithm interface member group*/

/*!\name Algorithm interface for VP9
 *
 * This interface provides the capability to decode VP9 streams.
 * @{
 */

/*!\brief A single instance of the VP9 decoder.
 *\deprecated This access mechanism is provided for backwards compatibility;
 * prefer vpx_codec_vp9_dx().
 */
extern vpx_codec_iface_t vpx_codec_vp9_dx_algo;

/*!\brief The interface to the VP9 decoder.
 */
extern vpx_codec_iface_t *vpx_codec_vp9_dx(void);
/*!@} - end algorithm interface member group*/

/*!\enum vp8_dec_control_id
 * \brief VP8 decoder control functions
 *
 * This set of macros define the control functions available for the VP8
 * decoder interface.
 *
 * \sa #vpx_codec_control
 */
enum vp8_dec_control_id {
  VP8D_GET_LAST_REF_UPDATES = VP8_DECODER_CTRL_ID_START,

  VP8D_GET_FRAME_CORRUPTED,

  VP8D_GET_LAST_REF_USED,

  VPXD_SET_DECRYPTOR,
  VP8D_SET_DECRYPTOR = VPXD_SET_DECRYPTOR,

  VP9D_GET_FRAME_SIZE,

  VP9D_GET_DISPLAY_SIZE,

  VP9D_GET_BIT_DEPTH,

  VP9_SET_BYTE_ALIGNMENT,

  VP9_INVERT_TILE_DECODE_ORDER,

  VP9_SET_SKIP_LOOP_FILTER,

  VP9_DECODE_SVC_SPATIAL_LAYER,

  /*!\brief Codec control function to get last decoded frame quantizer.
   *
   * Return value uses internal quantizer scale defined by the codec.
   *
   * Supported in codecs: VP8, VP9
   */
  VPXD_GET_LAST_QUANTIZER,

  /*!\brief Codec control function to set row level multi-threading.
   *
   * 0 : off, 1 : on
   *
   * Supported in codecs: VP9
   */
  VP9D_SET_ROW_MT,

  /*!\brief Codec control function to set loopfilter optimization.
   *
   * 0 : off, Loop filter is done after all tiles have been decoded
   * 1 : on, Loop filter is done immediately after decode without
   *     waiting for all threads to sync.
   *
   * Supported in codecs: VP9
   */
  VP9D_SET_LOOP_FILTER_OPT,

  VP8_DECODER_CTRL_ID_MAX
};

typedef void (*vpx_decrypt_cb)(void *decrypt_state, const unsigned char *input,
                               unsigned char *output, int count);

/*!\brief Structure to hold decryption state
 *
 * Defines a structure to hold the decryption state and access function.
 */
typedef struct vpx_decrypt_init {
  /*! Decrypt callback. */
  vpx_decrypt_cb decrypt_cb;

  /*! Decryption state. */
  void *decrypt_state;
} vpx_decrypt_init;

/*!\cond */
/*!\brief VP8 decoder control function parameter type
 *
 * Defines the data types that VP8D control functions take. Note that
 * additional common controls are defined in vp8.h
 *
 */

VPX_CTRL_USE_TYPE(VP8D_GET_LAST_REF_UPDATES, int *)
#define VPX_CTRL_VP8D_GET_LAST_REF_UPDATES
VPX_CTRL_USE_TYPE(VP8D_GET_FRAME_CORRUPTED, int *)
#define VPX_CTRL_VP8D_GET_FRAME_CORRUPTED
VPX_CTRL_USE_TYPE(VP8D_GET_LAST_REF_USED, int *)
#define VPX_CTRL_VP8D_GET_LAST_REF_USED
VPX_CTRL_USE_TYPE(VPXD_SET_DECRYPTOR, vpx_decrypt_init *)
#define VPX_CTRL_VPXD_SET_DECRYPTOR
VPX_CTRL_USE_TYPE(VP8D_SET_DECRYPTOR, vpx_decrypt_init *)
#define VPX_CTRL_VP8D_SET_DECRYPTOR
VPX_CTRL_USE_TYPE(VP9D_GET_FRAME_SIZE, int *)
#define VPX_CTRL_VP9D_GET_FRAME_SIZE
VPX_CTRL_USE_TYPE(VP9D_GET_DISPLAY_SIZE, int *)
#define VPX_CTRL_VP9D_GET_DISPLAY_SIZE
VPX_CTRL_USE_TYPE(VP9D_GET_BIT_DEPTH, unsigned int *)
#define VPX_CTRL_VP9D_GET_BIT_DEPTH
VPX_CTRL_USE_TYPE(VP9_SET_BYTE_ALIGNMENT, int)
#define VPX_CTRL_VP9_SET_BYTE_ALIGNMENT
VPX_CTRL_USE_TYPE(VP9_INVERT_TILE_DECODE_ORDER, int)
#define VPX_CTRL_VP9_INVERT_TILE_DECODE_ORDER
VPX_CTRL_USE_TYPE(VP9_SET_SKIP_LOOP_FILTER, int)
#define VPX_CTRL_VP9_SET_SKIP_LOOP_FILTER
VPX_CTRL_USE_TYPE(VP9_DECODE_SVC_SPATIAL_LAYER, int)
#define VPX_CTRL_VP9_DECODE_SVC_SPATIAL_LAYER
VPX_CTRL_USE_TYPE(VPXD_GET_LAST_QUANTIZER, int *)
#define VPX_CTRL_VPXD_GET_LAST_QUANTIZER
VPX_CTRL_USE_TYPE(VP9D_SET_ROW_MT, int)
#define VPX_CTRL_VP9_DECODE_SET_ROW_MT
VPX_CTRL_USE_TYPE(VP9D_SET_LOOP_FILTER_OPT, int)
#define VPX_CTRL_VP9_SET_LOOP_FILTER_OPT

/*!\endcond */
/*! @} - end defgroup vp8_decoder */

#if defined(__cplusplus)
}  
#endif

#endif
