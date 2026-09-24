#pragma once
#include <cstdio>
#include <cstddef>
namespace mmltk::backend::imaging::raster::detail {
void validate_png_extent(std::size_t width, std::size_t height, std::size_t channels, std::size_t stride, const char* path);
// Both paths use the same checked stb callback sink. Stream ownership transfers
// to write_png_stream, including when encoding, flushing, or closing fails.
int write_png_stream(std::FILE*, const char* path, int width, int height, int channels, const void* pixels, int stride);
int write_png_file(const char* path, int width, int height, int channels, const void* pixels, int stride);
}
