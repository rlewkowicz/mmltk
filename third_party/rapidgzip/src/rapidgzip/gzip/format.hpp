#pragma once

#include <cstddef>
#include <filereader/FileReader.hpp>
#include <filereader/Shared.hpp>
#include <indexed_bzip2/bzip2.hpp>
#include <optional>
#include <rapidgzip/blockfinder/Bgzf.hpp>
#include <rapidgzip/gzip/deflate.hpp>
#include <rapidgzip/gzip/gzip.hpp>
#include <utility>

namespace rapidgzip {
[[nodiscard]] inline std::optional<std::pair<FileType, size_t> > determineFileTypeAndOffset(
    const UniqueFileReader& fileReader) {
    if (!fileReader) {
        return std::nullopt;
    }

    gzip::BitReader bitReader{fileReader->clone()};
    const auto [gzipHeader, gzipError] = gzip::readHeader(bitReader);
    if (gzipError == Error::NONE) {
        return std::make_pair(blockfinder::Bgzf::isBgzfFile(fileReader) ? FileType::BGZF : FileType::GZIP,
                              bitReader.tell());
    }

    bitReader.seek(0);
    const auto [zlibHeader, zlibError] = zlib::readHeader(bitReader);
    if (zlibError == Error::NONE) {
        return std::make_pair(FileType::ZLIB, bitReader.tell());
    }

    bzip2::BitReader bzip2BitReader{fileReader->clone()};
    try {
        bzip2::readBzip2Header(bzip2BitReader);
        return std::make_pair(FileType::BZIP2, bzip2BitReader.tell());
    } catch (const std::exception&) {
    }

    bitReader.seek(0);
    deflate::Block block;
    if (block.readHeader(bitReader) == Error::NONE) {
        return std::make_pair(FileType::DEFLATE, 0);
    }

    return std::nullopt;
}

#ifdef WITH_PYTHON_SUPPORT
[[nodiscard]] std::string determineFileTypeAsString(PyObject* pythonObject) {
    const auto detectedType =
        determineFileTypeAndOffset(ensureSharedFileReader(std::make_unique<PythonFileReader>(pythonObject)));
    return toString(detectedType ? detectedType->first : FileType::NONE);
}
#endif
}
