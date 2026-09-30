#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <algorithm>
namespace {
struct StbiArena { unsigned char* data; std::size_t capacity, used; bool exhausted; };
thread_local StbiArena* active_arena = nullptr;
thread_local int (*heap_guard)(std::size_t, void*) = nullptr;
thread_local void* heap_guard_user = nullptr;
constexpr std::size_t arena_alignment = alignof(std::max_align_t);
void* image_allocate(std::size_t bytes) {
 if (!active_arena) return heap_guard && !heap_guard(bytes,heap_guard_user) ? nullptr : std::malloc(bytes);
 auto& arena = *active_arena;
 if (bytes > std::numeric_limits<std::size_t>::max() - arena_alignment) { arena.exhausted = true; return nullptr; }
 const auto extent = (bytes + arena_alignment - 1) / arena_alignment * arena_alignment;
 if (extent > arena.capacity - arena.used) { arena.exhausted = true; return nullptr; }
 auto* result = arena.data + arena.used; arena.used += extent; return result;
}
void image_free(void* pointer) { if (!active_arena) std::free(pointer); }
void* image_reallocate(void* pointer, std::size_t old_bytes, std::size_t bytes) {
 if (!active_arena) return heap_guard && !heap_guard(bytes,heap_guard_user) ? nullptr : std::realloc(pointer, bytes);
 if (bytes <= old_bytes) return pointer;
 auto* result = image_allocate(bytes);
 if (result && pointer) std::memcpy(result, pointer, old_bytes);
 return result;
}
}
#define STBI_MALLOC(bytes) image_allocate(bytes)
#define STBI_FREE(pointer) image_free(pointer)
#define STBI_REALLOC_SIZED(pointer, old_bytes, bytes) image_reallocate(pointer, old_bytes, bytes)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

void stbi_set_heap_allocation_guard_thread(int (*guard)(std::size_t,void*),void* user) {
 heap_guard=guard; heap_guard_user=user;
}

namespace {
// Inspection controls have fixed automatic lifetime, independent of image
// dimensions. The decoder's JPEG control allocation remains in its arena.
#ifndef STBI_NO_JPEG
static_assert(sizeof(stbi__jpeg)<=32*1024,"JPEG inspection control exceeds its bounded stack window");
#endif
#ifndef STBI_NO_GIF
static_assert(sizeof(stbi__gif)<=36*1024,"GIF inspection control exceeds its bounded stack window");
#endif
struct WorkspaceSize {
 std::size_t total = 0;
 bool valid = true;
 void add(std::size_t count, std::size_t element = 1) {
  const auto maximum = std::numeric_limits<std::size_t>::max();
  if (!valid || count > (maximum - arena_alignment) / element) { valid = false; return; }
  const auto extent = (count * element + arena_alignment - 1) / arena_alignment * arena_alignment;
  if (extent > maximum - total) { valid = false; return; }
  total += extent;
 }
};
}
size_t stbi_workspace_png_profile(unsigned w,unsigned h,size_t encoded_bytes) {
 const auto width=static_cast<std::size_t>(w),height=static_cast<std::size_t>(h);
 if (!width || !height || width > std::numeric_limits<std::size_t>::max()/height) return 0;
 const auto pixels=width*height;
 WorkspaceSize size;
  // Geometric IDAT reallocations retain every predecessor in the arena.
  size.add(std::max<std::size_t>(4096, encoded_bytes), 4);
  const unsigned depth_bytes = 2;
  // Seven Adam7 passes contain at most the full pixel population and seven
  // filter bytes per row. The inflate capacity doubles until that extent fits.
  size.add(pixels, 4 * depth_bytes * 4);
  size.add(height, 7 * 4);
  // Final canvas plus all interlace pass outputs; palette/alpha expansion and
  // 16-bit conversion can each retain a further complete output.
  size.add(pixels, 4 * depth_bytes * 3);
  size.add(pixels, 4 * (depth_bytes + 1));
  size.add(width, 2 * 4 * depth_bytes * 7);
  size.add(64, arena_alignment);
 return size.valid ? size.total : 0;
}
size_t stbi_workspace_required(stbi_uc const* encoded, int len, int desired) {
 int w = 0, h = 0, channels = 0;
 if (desired < 1 || desired > 4) return stbi__err("bad req_comp", "Invalid requested channel count");
 // This is the same allocation-free inspection entry point used by callers.
 if (!stbi_info_from_memory(encoded, len, &w, &h, &channels)) return 0;
 if (w <= 0 || h <= 0) return stbi__err("bad dimensions", "Invalid image dimensions");
 const auto width = static_cast<std::size_t>(w), height = static_cast<std::size_t>(h);
 if (width > std::numeric_limits<std::size_t>::max() / height) return stbi__err("workspace capacity", "Supplied image workspace size overflow");
 const auto pixels = width * height;
 WorkspaceSize size;
 stbi__context source;
 stbi__start_mem(&source, encoded, len);
#ifndef STBI_NO_PNG
 if (stbi__png_test(&source)) {
  // Ancillary chunks do not allocate decode storage. Count the actual IDAT
  // stream, preserving the decoder's content-based chunk interpretation.
  std::size_t idat=0, offset=8;
  while (offset<=static_cast<std::size_t>(len) && static_cast<std::size_t>(len)-offset>=8) {
   const auto* chunk=encoded+offset;
   const auto length=(std::size_t(chunk[0])<<24)|(std::size_t(chunk[1])<<16)|(std::size_t(chunk[2])<<8)|chunk[3];
   if (std::memcmp(chunk+4,"IEND",4)==0) break;
   // Leave malformed/truncated chunk policy with the decoder. In particular,
   // STB accepts a terminal IEND without requiring another complete CRC.
   if (length>static_cast<std::size_t>(len)-offset-8) break;
   if (std::memcmp(chunk+4,"IDAT",4)==0) idat+=length;
   offset+=length+12;
  }
  const auto bytes=stbi_workspace_png_profile(static_cast<unsigned>(w),static_cast<unsigned>(h),idat);
  return bytes ? bytes : stbi__err("workspace capacity", "Supplied image workspace size overflow");
 } else
#endif
#ifndef STBI_NO_JPEG
 if (stbi__jpeg_test(&source)) {
  // Up to four MCU-padded component planes and progressive short coefficients.
  const auto padded_width = (width + 31) / 32 * 32;
  const auto padded_height = (height + 31) / 32 * 32;
  if (padded_width > std::numeric_limits<std::size_t>::max() / padded_height) return stbi__err("workspace capacity", "Supplied image workspace size overflow");
  size.add(padded_width * padded_height, 4 * (sizeof(stbi_uc) + sizeof(short)));
  size.add(width + 3, 4);
  // Only the actual decoder allocates control storage; format probes are local.
  size.add(sizeof(stbi__jpeg));
  size.add(pixels, desired); size.add(1);
  size.add(16, arena_alignment + 15);
 } else
#endif
#ifndef STBI_NO_GIF
 if (stbi__gif_test(&source)) {
  // The first-frame decoder's control state is on its stack.
  size.add(pixels, 4 + 4 + 1 + desired);
  size.add(8, arena_alignment);
 } else
#endif
 {
  // BMP/TGA/PNM source, optional conversion, 16-bit reduction and TGA palette.
  size.add(pixels, 4 * sizeof(stbi__uint16) + desired * sizeof(stbi__uint16) + desired);
#ifndef STBI_NO_TGA
  if (stbi__tga_test(&source) && len>=18 && encoded[1])
   size.add(std::size_t(encoded[5])+(std::size_t(encoded[6])<<8),4);
#endif
  size.add(8, arena_alignment);
 }
 return size.valid ? size.total : stbi__err("workspace capacity", "Supplied image workspace size overflow");
}
stbi_uc* stbi_load_from_memory_workspace(stbi_uc const* buffer, int len, int* x, int* y, int* channels,
 int desired, void* workspace, size_t bytes) {
 const auto required = stbi_workspace_required(buffer, len, desired);
 if (!required) return nullptr;
 if (required > bytes || !workspace || reinterpret_cast<std::uintptr_t>(workspace) % arena_alignment)
  return stbi__errpuc("workspace capacity", "Supplied image workspace is too small");
 StbiArena arena{static_cast<unsigned char*>(workspace), bytes, 0, false};
 auto* previous = active_arena; active_arena = &arena;
 auto* pixels = stbi_load_from_memory(buffer, len, x, y, channels, desired);
 active_arena = previous;
 if (arena.exhausted) return stbi__errpuc("workspace capacity", "Supplied image workspace is too small");
 return pixels;
}
