/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

/*!\file
 * \brief Describes the aom image descriptor and associated operations
 *
 */
#if !defined(AOM_AOM_AOM_IMAGE_H_)
#define AOM_AOM_AOM_IMAGE_H_

#if defined(__cplusplus)
extern "C" {
#endif

#include "aom/aom_integer.h"

/*!\brief Current ABI version number
 *
 * \internal
 * If this file is altered in any way that changes the ABI, this value
 * must be bumped.  Examples include, but are not limited to, changing
 * types, removing or reassigning enums, adding/removing/rearranging
 * fields to structures
 */
#define AOM_IMAGE_ABI_VERSION (9) /**<\hideinitializer*/

#define AOM_IMG_FMT_PLANAR 0x100  /**< Image is a planar format. */
#define AOM_IMG_FMT_UV_FLIP 0x200 /**< V plane precedes U in memory. */
#define AOM_IMG_FMT_HIGHBITDEPTH 0x800 /**< Image uses 16bit framebuffer. */

/*!\brief List of supported image formats */
typedef enum aom_img_fmt {
  AOM_IMG_FMT_NONE,
  AOM_IMG_FMT_YV12 =
      AOM_IMG_FMT_PLANAR | AOM_IMG_FMT_UV_FLIP | 1, 
  AOM_IMG_FMT_I420 = AOM_IMG_FMT_PLANAR | 2,
  AOM_IMG_FMT_AOMYV12 = AOM_IMG_FMT_PLANAR | AOM_IMG_FMT_UV_FLIP | 3,
  AOM_IMG_FMT_AOMI420 = AOM_IMG_FMT_PLANAR | 4,
  AOM_IMG_FMT_I422 = AOM_IMG_FMT_PLANAR | 5,
  AOM_IMG_FMT_I444 = AOM_IMG_FMT_PLANAR | 6,
/*!\brief Allows detection of the presence of AOM_IMG_FMT_NV12 at compile time.
 */
#define AOM_HAVE_IMG_FMT_NV12 1
  AOM_IMG_FMT_NV12 =
      AOM_IMG_FMT_PLANAR | 7, 
  AOM_IMG_FMT_I42016 = AOM_IMG_FMT_I420 | AOM_IMG_FMT_HIGHBITDEPTH,
  AOM_IMG_FMT_YV1216 = AOM_IMG_FMT_YV12 | AOM_IMG_FMT_HIGHBITDEPTH,
  AOM_IMG_FMT_I42216 = AOM_IMG_FMT_I422 | AOM_IMG_FMT_HIGHBITDEPTH,
  AOM_IMG_FMT_I44416 = AOM_IMG_FMT_I444 | AOM_IMG_FMT_HIGHBITDEPTH,
} aom_img_fmt_t; 

/*!\brief List of supported color primaries */
typedef enum aom_color_primaries {
  AOM_CICP_CP_RESERVED_0 = 0,  
  AOM_CICP_CP_BT_709 = 1,      
  AOM_CICP_CP_UNSPECIFIED = 2, 
  AOM_CICP_CP_RESERVED_3 = 3,  
  AOM_CICP_CP_BT_470_M = 4,    
  AOM_CICP_CP_BT_470_B_G = 5,  
  AOM_CICP_CP_BT_601 = 6,      
  AOM_CICP_CP_SMPTE_240 = 7,   
  AOM_CICP_CP_GENERIC_FILM =
      8, 
  AOM_CICP_CP_BT_2020 = 9,      
  AOM_CICP_CP_XYZ = 10,         
  AOM_CICP_CP_SMPTE_431 = 11,   
  AOM_CICP_CP_SMPTE_432 = 12,   
  AOM_CICP_CP_RESERVED_13 = 13, 
  AOM_CICP_CP_EBU_3213 = 22,    
  AOM_CICP_CP_RESERVED_23 = 23  
} aom_color_primaries_t;        

/*!\brief List of supported transfer functions */
typedef enum aom_transfer_characteristics {
  AOM_CICP_TC_RESERVED_0 = 0,  
  AOM_CICP_TC_BT_709 = 1,      
  AOM_CICP_TC_UNSPECIFIED = 2, 
  AOM_CICP_TC_RESERVED_3 = 3,  
  AOM_CICP_TC_BT_470_M = 4,    
  AOM_CICP_TC_BT_470_B_G = 5,  
  AOM_CICP_TC_BT_601 = 6,      
  AOM_CICP_TC_SMPTE_240 = 7,   
  AOM_CICP_TC_LINEAR = 8,      
  AOM_CICP_TC_LOG_100 = 9,     
  AOM_CICP_TC_LOG_100_SQRT10 =
      10,                     
  AOM_CICP_TC_IEC_61966 = 11, 
  AOM_CICP_TC_BT_1361 = 12,   
  AOM_CICP_TC_SRGB = 13,      
  AOM_CICP_TC_BT_2020_10_BIT = 14, 
  AOM_CICP_TC_BT_2020_12_BIT = 15, 
  AOM_CICP_TC_SMPTE_2084 = 16,     
  AOM_CICP_TC_SMPTE_428 = 17,      
  AOM_CICP_TC_HLG = 18,            
  AOM_CICP_TC_RESERVED_19 = 19     
} aom_transfer_characteristics_t;  

/*!\brief List of supported matrix coefficients */
typedef enum aom_matrix_coefficients {
  AOM_CICP_MC_IDENTITY = 0,    
  AOM_CICP_MC_BT_709 = 1,      
  AOM_CICP_MC_UNSPECIFIED = 2, 
  AOM_CICP_MC_RESERVED_3 = 3,  
  AOM_CICP_MC_FCC = 4,         
  AOM_CICP_MC_BT_470_B_G = 5,  
  AOM_CICP_MC_BT_601 = 6,      
  AOM_CICP_MC_SMPTE_240 = 7,   
  AOM_CICP_MC_SMPTE_YCGCO = 8, 
  AOM_CICP_MC_BT_2020_NCL =
      9, 
  AOM_CICP_MC_BT_2020_CL = 10, 
  AOM_CICP_MC_SMPTE_2085 = 11, 
  AOM_CICP_MC_CHROMAT_NCL =
      12, 
  AOM_CICP_MC_CHROMAT_CL = 13,  
  AOM_CICP_MC_ICTCP = 14,       
  AOM_CICP_MC_RESERVED_15 = 15, 
  AOM_CICP_MC_IPT_C2 = 15,      
  AOM_CICP_MC_YCGCO_RE = 16,    
  AOM_CICP_MC_YCGCO_RO = 17,    
} aom_matrix_coefficients_t; 

/*!\brief List of supported color range */
typedef enum aom_color_range {
  AOM_CR_STUDIO_RANGE = 0, 
  AOM_CR_FULL_RANGE = 1    
} aom_color_range_t;       

/*!\brief List of chroma sample positions */
typedef enum aom_chroma_sample_position {
  AOM_CSP_UNKNOWN = 0,          
  AOM_CSP_VERTICAL = 1,         
  AOM_CSP_COLOCATED = 2,        
  AOM_CSP_RESERVED = 3          
} aom_chroma_sample_position_t; 

/*!\brief List of insert flags for Metadata
 *
 * These flags control how the library treats metadata during encode.
 *
 * While encoding, when metadata is added to an aom_image via
 * aom_img_add_metadata(), the flag passed along with the metadata will
 * determine where the metadata OBU will be placed in the encoded OBU stream,
 * and whether it's layer-specific. Metadata will be emitted into the output
 * stream within the next temporal unit if it satisfies the specified insertion
 * flag.
 *
 * If the video contains multiple spatial and/or temporal layers,
 * a layer-specific metadata OBU only applies to the current frame's layer, as
 * determined by the frame's temporal_id and spatial_id. Some metadata types
 * cannot be layer-specific, as listed in Section 6.7.1 of the draft AV1
 * specification as of 2025-03-06.
 *
 * During decoding, when the library encounters a metadata OBU, it is emitted
 * with the next output aom_image. Its insert_flag is set to either
 * AOM_MIF_ANY_FRAME, or AOM_MIF_ANY_FRAME_LAYER_SPECIFIC if the OBU contains an
 * OBU header extension (i.e. the video contains multiple layers AND the
 * metadata was added using *_LAYER_SPECIFIC insert flag if using libaom).
 */
typedef enum aom_metadata_insert_flags {
  AOM_MIF_NON_KEY_FRAME = 0, 
  AOM_MIF_KEY_FRAME = 1,     
  AOM_MIF_ANY_FRAME = 2,     
  AOM_MIF_NON_KEY_FRAME_LAYER_SPECIFIC = 16,
  AOM_MIF_KEY_FRAME_LAYER_SPECIFIC = 17,
  AOM_MIF_ANY_FRAME_LAYER_SPECIFIC = 18,
} aom_metadata_insert_flags_t;

/*!\brief Array of aom_metadata structs for an image. */
typedef struct aom_metadata_array aom_metadata_array_t;

/*!\brief Metadata payload. */
typedef struct aom_metadata {
  uint32_t type;                           
  uint8_t *payload;                        
  size_t sz;                               
  aom_metadata_insert_flags_t insert_flag; 
} aom_metadata_t;

typedef struct aom_image {
  aom_img_fmt_t fmt; 
  /*!\brief CICP Color Primaries
   *
   * \if av1_encoder
   * \attention Only set by the decoder. To control the value used by the
   * encoder, use the \ref AV1E_SET_COLOR_PRIMARIES codec control.
   * \endif
   */
  aom_color_primaries_t cp;
  /*!\brief CICP Transfer Characteristics
   *
   * \if av1_encoder
   * \attention Only set by the decoder. To control the value used by the
   * encoder, use the \ref AV1E_SET_TRANSFER_CHARACTERISTICS codec control.
   * \endif
   */
  aom_transfer_characteristics_t tc;
  /*!\brief CICP Matrix Coefficients
   *
   * \if av1_encoder
   * \attention Only set by the decoder. To control the value used by the
   * encoder, use the \ref AV1E_SET_MATRIX_COEFFICIENTS codec control.
   * \endif
   */
  aom_matrix_coefficients_t mc;
  /*!\brief Whether image is monochrome
   *
   * \if av1_encoder
   * \attention Only set by the decoder. To control the encoder behavior, set
   * aom_codec_enc_cfg_t::monochrome.
   * \endif
   */
  int monochrome;
  /*!\brief Chroma sample position
   *
   * \if av1_encoder
   * \attention Only set by the decoder. To control the value used by the
   * encoder, use the \ref AV1E_SET_CHROMA_SAMPLE_POSITION codec control.
   * \endif
   */
  aom_chroma_sample_position_t csp;
  /*!\brief Color Range
   *
   * \if av1_encoder
   * \attention Only set by the decoder. To control the value used by the
   * encoder, use the \ref AV1E_SET_COLOR_RANGE codec control.
   * \endif
   */
  aom_color_range_t range;

  unsigned int w;         
  unsigned int h;         
  unsigned int bit_depth; 

  unsigned int d_w; 
  unsigned int d_h; 

  unsigned int r_w; 
  unsigned int r_h; 

  unsigned int x_chroma_shift; 
  unsigned int y_chroma_shift; 

#define AOM_PLANE_PACKED 0 /**< To be used for all packed formats */
#define AOM_PLANE_Y 0      /**< Y (Luminance) plane */
#define AOM_PLANE_U 1      /**< U (Chroma) plane */
#define AOM_PLANE_V 2      /**< V (Chroma) plane */
  unsigned char *planes[3]; 
  /*!Stride between rows for each plane
   *
   * \note With planar formats, \c stride[AOM_PLANE_U] must be the same as \c
   * stride[AOM_PLANE_V].
   */
  int stride[3];
  size_t sz; 

  int bps; 

  int temporal_id; 
  int spatial_id;  

  /*!\brief The following member may be set by the application to associate
   * data with this image.
   */
  void *user_priv;

  unsigned char *img_data; 
  int img_data_owner;      
  int self_allocd;         

  aom_metadata_array_t
      *metadata; 

  void *fb_priv; 
} aom_image_t;   

/*!\brief Open a descriptor, allocating storage for the underlying image
 *
 * Returns a descriptor for storing an image of the given format. The
 * storage for the image is allocated on the heap.
 *
 * \param[in]    img       Pointer to storage for descriptor. If this parameter
 *                         is NULL, the storage for the descriptor will be
 *                         allocated on the heap.
 * \param[in]    fmt       Format for the image
 * \param[in]    d_w       Width of the image. Must not exceed 0x08000000
 *                         (2^27).
 * \param[in]    d_h       Height of the image. Must not exceed 0x08000000
 *                         (2^27).
 * \param[in]    align     Alignment, in bytes, of the image buffer and
 *                         each row in the image (stride). Must not exceed
 *                         65536.
 *
 * \return Returns a pointer to the initialized image descriptor. If the img
 *         parameter is non-null, the value of the img parameter will be
 *         returned.
 */
aom_image_t *aom_img_alloc(aom_image_t *img, aom_img_fmt_t fmt,
                           unsigned int d_w, unsigned int d_h,
                           unsigned int align);

/*!\brief Open a descriptor, using existing storage for the underlying image
 *
 * Returns a descriptor for storing an image of the given format. The
 * storage for the image has been allocated elsewhere, and a descriptor is
 * desired to "wrap" that storage.
 *
 * \param[in]    img           Pointer to storage for descriptor. If this
 *                             parameter is NULL, the storage for the descriptor
 *                             will be allocated on the heap.
 * \param[in]    fmt           Format for the image
 * \param[in]    d_w           Width of the image. Must not exceed 0x08000000
 *                             (2^27).
 * \param[in]    d_h           Height of the image. Must not exceed 0x08000000
 *                             (2^27).
 * \param[in]    stride_align  Alignment, in bytes, of each row in the image
 *                             (stride). Must not exceed 65536.
 * \param[in]    img_data      Storage to use for the image. The storage must
 *                             outlive the returned image descriptor; it can be
 *                             disposed of after calling aom_img_free().
 *
 * \return Returns a pointer to the initialized image descriptor. If the img
 *         parameter is non-null, the value of the img parameter will be
 *         returned.
 *
 * \note \a img_data is required to have a minimum allocation size that
 *       satisfies the requirements of the \a fmt, \a d_w, \a d_h and \a
 *       stride_align parameters. This size can be calculated as follows (see
 *       \c img_alloc_helper in the aom_image.c file in the libaom source tree
 *       for more detail):
 * \code
 * align = (1 << x_chroma_shift) - 1;
 * w = (d_w + align) & ~align;
 * align = (1 << y_chroma_shift) - 1;
 * h = (d_h + align) & ~align;
 *
 * s = (fmt & AOM_IMG_FMT_PLANAR) ? w : (uint64_t)bps * w / 8;
 * s = (fmt & AOM_IMG_FMT_HIGHBITDEPTH) ? s * 2 : s;
 * s = (s + stride_align - 1) & ~((uint64_t)stride_align - 1);
 * s = (fmt & AOM_IMG_FMT_HIGHBITDEPTH) ? s / 2 : s;
 * alloc_size = (fmt & AOM_IMG_FMT_PLANAR) ? (uint64_t)h * s * bps / 8
 *                                         : (uint64_t)h * s;
 * \endcode
 * \a x_chroma_shift, \a y_chroma_shift and \a bps can be obtained by calling
 * \ref aom_img_wrap with a non-\c NULL \a img_data parameter. The \c
 * aom_image_t pointer should \em not be used in other API calls until \em after
 * a successful call to \ref aom_img_wrap with a valid image buffer. For
 * example:
 * \code
 * aom_img_wrap(img, fmt, d_w, d_h, stride_align, (unsigned char *)1);
 * ... calculate buffer size and allocate buffer as described earlier
 * aom_img_wrap(img, fmt, d_w, d_h, stride_align, img_data);
 * \endcode
 */
aom_image_t *aom_img_wrap(aom_image_t *img, aom_img_fmt_t fmt, unsigned int d_w,
                          unsigned int d_h, unsigned int stride_align,
                          unsigned char *img_data);

/*!\brief Open a descriptor, allocating storage for the underlying image with a
 * border
 *
 * Returns a descriptor for storing an image of the given format and its
 * borders. The storage for the image is allocated on the heap.
 *
 * \param[in]    img        Pointer to storage for descriptor. If this parameter
 *                          is NULL, the storage for the descriptor will be
 *                          allocated on the heap.
 * \param[in]    fmt        Format for the image
 * \param[in]    d_w        Width of the image. Must not exceed 0x08000000
 *                          (2^27).
 * \param[in]    d_h        Height of the image. Must not exceed 0x08000000
 *                          (2^27).
 * \param[in]    align      Alignment, in bytes, of the image buffer and
 *                          each row in the image (stride). Must not exceed
 *                          65536.
 * \param[in]    size_align Alignment, in pixels, of the image width and height.
 *                          Must not exceed 65536.
 * \param[in]    border     A border that is padded on four sides of the image.
 *                          Must not exceed 65536.
 *
 * \return Returns a pointer to the initialized image descriptor. If the img
 *         parameter is non-null, the value of the img parameter will be
 *         returned.
 */
aom_image_t *aom_img_alloc_with_border(aom_image_t *img, aom_img_fmt_t fmt,
                                       unsigned int d_w, unsigned int d_h,
                                       unsigned int align,
                                       unsigned int size_align,
                                       unsigned int border);

/*!\brief Set the rectangle identifying the displayed portion of the image
 *
 * Updates the displayed rectangle (aka viewport) on the image surface to
 * match the specified coordinates and size. Specifically, sets img->d_w,
 * img->d_h, and elements of the img->planes[] array.
 *
 * \param[in]    img       Image descriptor
 * \param[in]    x         leftmost column
 * \param[in]    y         topmost row
 * \param[in]    w         width
 * \param[in]    h         height
 * \param[in]    border    A border that is padded on four sides of the image.
 *
 * \return 0 if the requested rectangle is valid, nonzero (-1) otherwise.
 */
int aom_img_set_rect(aom_image_t *img, unsigned int x, unsigned int y,
                     unsigned int w, unsigned int h, unsigned int border);

/*!\brief Flip the image vertically (top for bottom)
 *
 * Adjusts the image descriptor's pointers and strides to make the image
 * be referenced upside-down.
 *
 * \param[in]    img       Image descriptor
 */
void aom_img_flip(aom_image_t *img);

/*!\brief Close an image descriptor
 *
 * Frees all allocated storage associated with an image descriptor.
 *
 * \param[in]    img       Image descriptor
 */
void aom_img_free(aom_image_t *img);

/*!\brief Get the width of a plane
 *
 * Get the width of a plane of an image
 *
 * \param[in]    img       Image descriptor
 * \param[in]    plane     Plane index
 */
int aom_img_plane_width(const aom_image_t *img, int plane);

/*!\brief Get the height of a plane
 *
 * Get the height of a plane of an image
 *
 * \param[in]    img       Image descriptor
 * \param[in]    plane     Plane index
 */
int aom_img_plane_height(const aom_image_t *img, int plane);

/*!\brief Add metadata to image.
 *
 * Adds metadata to aom_image_t.
 * Function makes a copy of the provided data parameter.
 * Metadata insertion point is controlled by insert_flag.
 *
 * \param[in]    img          Image descriptor
 * \param[in]    type         Metadata type
 * \param[in]    data         Metadata contents
 * \param[in]    sz           Metadata contents size
 * \param[in]    insert_flag  Metadata insert flag
 *
 * \return Returns 0 on success. If img or data is NULL, sz is 0, or memory
 * allocation fails, it returns -1.
 */
int aom_img_add_metadata(aom_image_t *img, uint32_t type, const uint8_t *data,
                         size_t sz, aom_metadata_insert_flags_t insert_flag);

/*!\brief Return a metadata payload stored within the image metadata array.
 *
 * Gets the metadata (aom_metadata_t) at the indicated index in the image
 * metadata array.
 *
 * \param[in] img          Pointer to image descriptor to get metadata from
 * \param[in] index        Metadata index to get from metadata array
 *
 * \return Returns a const pointer to the selected metadata, if img and/or index
 * is invalid, it returns NULL.
 */
const aom_metadata_t *aom_img_get_metadata(const aom_image_t *img,
                                           size_t index);

/*!\brief Return the number of metadata blocks within the image.
 *
 * Gets the number of metadata blocks contained within the provided image
 * metadata array.
 *
 * \param[in] img          Pointer to image descriptor to get metadata number
 * from.
 *
 * \return Returns the size of the metadata array. If img or metadata is NULL,
 * it returns 0.
 */
size_t aom_img_num_metadata(const aom_image_t *img);

/*!\brief Remove metadata from image.
 *
 * Removes all metadata in image metadata list and sets metadata list pointer
 * to NULL.
 *
 * \param[in]    img       Image descriptor
 */
void aom_img_remove_metadata(aom_image_t *img);

/*!\brief Allocate memory for aom_metadata struct.
 *
 * Allocates storage for the metadata payload, sets its type and copies the
 * payload data into the aom_metadata struct. A metadata payload buffer of size
 * sz is allocated and sz bytes are copied from data into the payload buffer.
 *
 * \param[in]    type         Metadata type
 * \param[in]    data         Metadata data pointer
 * \param[in]    sz           Metadata size
 * \param[in]    insert_flag  Metadata insert flag
 *
 * \return Returns the newly allocated aom_metadata struct. If data is NULL,
 * sz is 0, or memory allocation fails, it returns NULL.
 */
aom_metadata_t *aom_img_metadata_alloc(uint32_t type, const uint8_t *data,
                                       size_t sz,
                                       aom_metadata_insert_flags_t insert_flag);

/*!\brief Free metadata struct.
 *
 * Free metadata struct and its buffer.
 *
 * \param[in]    metadata       Metadata struct pointer
 */
void aom_img_metadata_free(aom_metadata_t *metadata);

#if defined(__cplusplus)
}  
#endif

#endif
