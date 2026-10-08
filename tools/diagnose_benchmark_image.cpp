// Native counterpart of diagnose_benchmark_image.py. Link with libarchive and
// libcrypto; cpp_support.hpp uses the existing third_party/json/include headers.
// Compiled samples and raw masks retain their existing native helper owners.
#include "cpp_digest.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using mmltk::tools::ToolError;
using mmltk::tools::json;
namespace fs = std::filesystem;
using Bytes = std::span<const unsigned char>;
constexpr std::size_t image_limit = 64 * 1024 * 1024;

[[noreturn]] void argument_error(const std::string& message) {
    throw ToolError(message, 2);
}

// Retain Python's JSON numeric representation of nonfinite annotation values.
// Ordinary JSON values still use the shared encoder, including string escaping.
std::string encode(const json& value) {
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (std::isnan(number)) return "NaN";
        if (std::isinf(number)) return number < 0 ? "-Infinity" : "Infinity";
    }
    if (!value.is_object() && !value.is_array()) return mmltk::tools::json_dump(value);
    std::string output = value.is_object() ? "{" : "[";
    bool first = true;
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (!first) output += ',';
        first = false;
        if (value.is_object()) output += mmltk::tools::json_dump(json(item.key())) + ':';
        output += encode(item.value());
    }
    output += value.is_object() ? '}' : ']';
    return output;
}

void report(const json& record) { std::cout << encode(record) << std::endl; }

std::string integer(std::string_view input, std::string_view option) {
    const auto text = mmltk::tools::trim(input);
    const auto result = mmltk::tools::text::decimal_integer(text);
    if (!result) argument_error("argument " + std::string(option) + ": invalid int value: '" + text + "'");
    return *result;
}

std::uint64_t image_identity(const std::string& text) {
    std::uint64_t result = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), result);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size())
        argument_error("image ID must fit uint64");
    return result;
}

struct Options {
    std::optional<std::string> image_id;
    std::optional<fs::path> compiled, image, archive, parquet, panoptic;
    std::optional<std::string> sample, opening_workers;
    std::vector<fs::path> indexes;
    bool cold = false, objects = false, export_runs = false, export_image = false, help = false;
};

void help() {
    std::cout << R"(Read retained benchmark image and annotation geometry through ./mmltk.

Options:
  -h, --help                   show this help message and exit
  --image-id ID                source image ID (uint64)
  --compiled PATH             inspect a compiled dataset sample
  --sample N                  zero-based eligible physical compiled sample slot
  --profile-compiled WORKERS  report trusted compiled opening time (1–64 workers)
  --cold-compiled              request file-local cache eviction before compiled opening
  --index PATH                normalized annotation index (may be repeated)
  --objects                   include normalized object records for each matching index row
  --export-mask-runs           export bounded selected-image normalized RLEs and identities
  --image PATH                inspect retained JPEG or PNG header geometry
  --archive PATH              scan a retained tar archive for the selected image
  --parquet PATH              inspect a raw COCONut mask and segment metadata
  --panoptic PATH             count RGB segment IDs in a retained panoptic PNG
  --export                    copy inspected image into build/validation/benchmark-image
)";
}

Options parse(int argc, char** argv) {
    constexpr std::array<std::string_view, 15> names{
        "--image-id", "--compiled", "--sample", "--profile-compiled", "--cold-compiled",
        "--index", "--objects", "--export-mask-runs", "--image", "--archive", "--parquet",
        "--panoptic", "--export", "--help", "-h"};
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--") {
            if (index + 1 < argc) argument_error("unrecognized arguments: " + std::string(argv[index + 1]));
            break;
        }
        const auto equal = argument.find('=');
        const std::string prefix = argument.substr(0, equal);
        std::string_view name;
        for (const auto candidate : names) {
            if (candidate == prefix) { name = candidate; break; }
        }
        if (name.empty() && prefix.starts_with("--")) {
            for (const auto candidate : names) {
                if (!candidate.starts_with(prefix)) continue;
                if (!name.empty()) argument_error("ambiguous option: " + prefix);
                name = candidate;
            }
        }
        if (name.empty()) argument_error("unrecognized arguments: " + argument);
        const bool flag = name == "--cold-compiled" || name == "--objects" ||
                          name == "--export-mask-runs" || name == "--export" ||
                          name == "--help" || name == "-h";
        if (flag && equal != std::string::npos)
            argument_error("argument " + std::string(name) + ": ignored explicit argument");
        if (name == "--help" || name == "-h") { options.help = true; return options; }
        if (name == "--cold-compiled") { options.cold = true; continue; }
        if (name == "--objects") { options.objects = true; continue; }
        if (name == "--export-mask-runs") { options.export_runs = true; continue; }
        if (name == "--export") { options.export_image = true; continue; }
        std::string value;
        if (equal != std::string::npos) value = argument.substr(equal + 1);
        else {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).starts_with("--"))
                argument_error("argument " + std::string(name) + ": expected one argument");
            value = argv[++index];
        }
        if (name == "--image-id") options.image_id = integer(value, name);
        else if (name == "--sample") options.sample = integer(value, name);
        else if (name == "--profile-compiled") options.opening_workers = integer(value, name);
        else {
            const fs::path path = mmltk::tools::lexical_path(value);
            if (name == "--compiled") options.compiled = path;
            else if (name == "--index") options.indexes.push_back(path);
            else if (name == "--image") options.image = path;
            else if (name == "--archive") options.archive = path;
            else if (name == "--parquet") options.parquet = path;
            else if (name == "--panoptic") options.panoptic = path;
        }
    }
    if (options.compiled.has_value() != options.sample.has_value())
        argument_error("--compiled and --sample are required together");
    if (options.opening_workers) {
        const auto& workers = *options.opening_workers;
        if (!options.compiled || workers == "0" || workers.front() == '-' ||
            workers.size() > 2 || (workers.size() == 2 && workers > "64"))
            argument_error("--profile-compiled requires --compiled and 1–64 workers");
    }
    if (options.cold && !options.opening_workers)
        argument_error("--cold-compiled requires --profile-compiled");
    return options;
}

std::string native_helper(std::string_view variable) {
    const auto executable = mmltk::tools::env(variable);
    if (executable.empty()) throw ToolError("native helper was not provided; invoke through ./mmltk");
    return executable;
}

void check_timeout(const mmltk::tools::ProcessResult& result, int seconds) {
    if (result.timed_out) throw ToolError("native diagnostic timed out after " + std::to_string(seconds) + " seconds");
    if (result.exec_errno) throw ToolError("native diagnostic: " + std::string(std::strerror(result.exec_errno)));
}

std::optional<std::string> compiled_sample(const Options& options) {
    std::vector<std::string> command{native_helper("MMLTK_COMPILED_SAMPLE_DIAGNOSTIC"),
                                     options.compiled->string(), *options.sample};
    if (options.opening_workers) {
        command.push_back(*options.opening_workers);
        command.push_back(options.cold ? "1" : "0");
    }
    const int seconds = options.opening_workers ? 540 : 120;
    mmltk::tools::RunOptions execution;
    execution.timeout = std::chrono::seconds(seconds);
    const auto completed = mmltk::tools::capture(command, execution);
    check_timeout(completed, seconds);
    if (completed.exit_code != 0) {
        const auto error = mmltk::tools::trim(completed.err);
        throw ToolError(error.empty() ? "compiled diagnostic exited with status " +
                        std::to_string(completed.signal ? -completed.signal : completed.exit_code) : error);
    }
    const auto record = json::parse(completed.out);
    if (!record.is_object()) throw ToolError("compiled diagnostic did not return an object");
    report(record);
    const auto& identity = record.at("image_id");
    if (identity.is_null()) return std::nullopt;
    if (!identity.is_number_integer()) throw ToolError("compiled diagnostic returned an invalid image ID");
    return identity.dump();
}

std::uint64_t unsigned_at(Bytes data, std::size_t offset, std::size_t size, bool little = true) {
    if (offset > data.size() || size > data.size() - offset)
        throw ToolError("truncated binary image or annotation header");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
        const auto byte = data[offset + (little ? size - index - 1 : index)];
        value = (value << 8) | byte;
    }
    return value;
}

float float_at(Bytes data, std::size_t offset) {
    return std::bit_cast<float>(static_cast<std::uint32_t>(unsigned_at(data, offset, 4)));
}

double double_at(Bytes data, std::size_t offset) {
    return std::bit_cast<double>(unsigned_at(data, offset, 8));
}

std::vector<unsigned char> read_exact(std::ifstream& stream, std::size_t count, const std::string& error) {
    std::vector<unsigned char> data(count);
    stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count));
    if (static_cast<std::size_t>(stream.gcount()) != count) throw ToolError(error);
    return data;
}

std::uint64_t slice_offset(std::uint64_t base, std::uint64_t first, std::uint64_t count,
                           std::uint64_t stride, std::uint64_t extent, std::string_view error) {
    if (base > extent || first > (extent - base) / stride)
        throw ToolError(std::string(error));
    const auto offset = base + first * stride;
    if (count > (extent - offset) / stride) throw ToolError(std::string(error));
    return offset;
}

void annotation_rows(const fs::path& path, std::optional<std::uint64_t> image_id,
                     bool include_objects, bool export_mask_runs) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw ToolError("cannot read " + path.string());
    const auto extent = fs::file_size(path);
    const auto header = read_exact(stream, 256, "unsupported normalized index: " + path.string());
    if (unsigned_at(header, 0, 8) != 0x4D4D4C544B4E4931ULL || unsigned_at(header, 8, 4) != 3)
        throw ToolError("unsupported normalized index: " + path.string());
    const auto count = unsigned_at(header, 16, 4);
    const auto offset = unsigned_at(header, 36, 8);
    if (offset != 256 || extent < offset || count > (extent - offset) / 32)
        throw ToolError("invalid normalized image table: " + path.string());
    stream.seekg(static_cast<std::streamoff>(offset));
    for (std::uint64_t start = 0; start < count; start += 4096) {
        const auto rows = std::min<std::uint64_t>(4096, count - start);
        const auto table = read_exact(stream, static_cast<std::size_t>(rows * 32),
                                      "truncated normalized image table: " + path.string());
        for (std::size_t index = 0; index < rows; ++index) {
            const Bytes row = Bytes(table).subspan(index * 32, 32);
            const auto identity = unsigned_at(row, 0, 8);
            if (!image_id || identity != *image_id) continue;
            const auto first_box = unsigned_at(row, 8, 8);
            const auto boxes = unsigned_at(row, 16, 4);
            const auto width = unsigned_at(row, 20, 4), height = unsigned_at(row, 24, 4);
            report({{"kind", "annotation"}, {"path", path.string()}, {"row", start + index},
                    {"image_id", identity}, {"width", width}, {"height", height},
                    {"shard", unsigned_at(row, 28, 2)}, {"first_box", first_box}, {"boxes", boxes}});
            if (!include_objects && !export_mask_runs) return;
            if (boxes > 65535) throw ToolError("normalized annotation slice exceeds admission");
            const auto box_offset = slice_offset(unsigned_at(header, 44, 8), first_box, boxes, 64, extent,
                                                 "normalized annotation slice exceeds admission");
            stream.seekg(static_cast<std::streamoff>(box_offset));
            const auto payload = read_exact(stream, static_cast<std::size_t>(boxes * 64), "truncated normalized annotations");
            json exported = json::array();
            std::uint64_t exported_pairs = 0;
            for (std::size_t object = 0; object < boxes; ++object) {
                const Bytes box = Bytes(payload).subspan(object * 64, 64);
                const auto first_pair = unsigned_at(box, 16, 8), pairs = unsigned_at(box, 24, 4);
                json record{{"annotation_id", unsigned_at(box, 40, 8)}, {"class_id", unsigned_at(box, 28, 1)},
                            {"source_category_id", unsigned_at(box, 48, 8)}, {"flags", unsigned_at(box, 29, 1)},
                            {"bbox", {float_at(box, 0), float_at(box, 4), float_at(box, 8), float_at(box, 12)}},
                            {"mask_pairs", pairs}, {"original_area", double_at(box, 32)},
                            {"source_ordinal", unsigned_at(box, 56, 8)}};
                json diagnostic{{"kind", "annotation_object"}, {"path", path.string()}, {"image_id", identity}};
                diagnostic.update(record);
                report(diagnostic);
                if (!export_mask_runs) continue;
                exported_pairs += pairs;
                if (pairs > 65535 || exported_pairs > 1048576)
                    throw ToolError("selected object mask exceeds export bound");
                const auto mask_offset = slice_offset(unsigned_at(header, 52, 8), first_pair, pairs, 8, extent,
                                                      "selected object mask exceeds index extent");
                stream.seekg(static_cast<std::streamoff>(mask_offset));
                const auto mask = read_exact(stream, static_cast<std::size_t>(pairs * 8), "truncated selected object mask");
                json runs = json::array();
                for (std::size_t pair = 0; pair < pairs; ++pair)
                    runs.push_back({unsigned_at(mask, pair * 8, 4), unsigned_at(mask, pair * 8 + 4, 4)});
                record.erase("mask_pairs");
                record["runs"] = std::move(runs);
                exported.push_back(std::move(record));
            }
            if (export_mask_runs) {
                const auto completion = mmltk::tools::read_json(path.string() + ".complete.json");
                const auto output = fs::path("/evidence") / (std::to_string(identity) + ".originals.json");
                const json result{{"image_id", identity}, {"width", width}, {"height", height},
                                  {"index", path.string()}, {"annotation_sha256", completion.at("annotation_sha256")},
                                  {"objects", std::move(exported)}};
                mmltk::tools::write_text(output, encode(result) + '\n');
                report({{"kind", "mask_run_export"}, {"path", "build/validation/benchmark-image/" + output.filename().string()},
                        {"objects", boxes}});
            }
            return;
        }
    }
    if (export_mask_runs) throw ToolError("selected image is absent from normalized index");
}

bool starts_with(Bytes data, std::string_view prefix) {
    return data.size() >= prefix.size() &&
           std::memcmp(data.data(), prefix.data(), prefix.size()) == 0;
}

std::optional<std::uint64_t> exif_orientation(Bytes payload) {
    if (!starts_with(payload, std::string_view("Exif\0\0", 6))) return std::nullopt;
    const auto data = payload.subspan(6);
    if (data.size() < 8 || (!starts_with(data, "II") && !starts_with(data, "MM")))
        throw ToolError("invalid EXIF TIFF header");
    const bool little = starts_with(data, "II");
    if (unsigned_at(data, 2, 2, little) != 42) throw ToolError("invalid EXIF TIFF magic");
    const auto offset = unsigned_at(data, 4, 4, little);
    if (offset > data.size() - 2) throw ToolError("invalid EXIF directory offset");
    const auto count = unsigned_at(data, offset, 2, little);
    if (count > (data.size() - offset - 2) / 12) throw ToolError("invalid EXIF directory size");
    for (std::size_t index = 0; index < count; ++index) {
        const auto entry = offset + 2 + index * 12;
        if (unsigned_at(data, entry, 2, little) == 0x112 &&
            unsigned_at(data, entry + 2, 2, little) == 3 && unsigned_at(data, entry + 4, 4, little) == 1)
            return unsigned_at(data, entry + 8, 2, little);
    }
    return std::nullopt;
}

json image_geometry(Bytes data) {
    if (starts_with(data, "\x89PNG\r\n\x1a\n"))
        return {{"encoding", "PNG"}, {"width", unsigned_at(data, 16, 4, false)},
                {"height", unsigned_at(data, 20, 4, false)}};
    if (!starts_with(data, "\xff\xd8")) throw ToolError("image is neither JPEG nor PNG");
    json result{{"encoding", "JPEG"}, {"orientation", nullptr}};
    std::size_t offset = 2;
    while (offset < data.size()) {
        if (data[offset] != 255) throw ToolError("invalid JPEG marker");
        while (offset < data.size() && data[offset] == 255) ++offset;
        if (offset >= data.size()) throw ToolError("truncated JPEG marker");
        const auto marker = data[offset++];
        if (marker == 0xDA || marker == 0xD9) break;
        if (marker == 1 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        const auto length = unsigned_at(data, offset, 2, false);
        if (length < 2 || length > data.size() - offset) throw ToolError("invalid JPEG segment extent");
        const auto payload = data.subspan(offset + 2, length - 2);
        if (marker == 0xE1) {
            if (const auto orientation = exif_orientation(payload)) result["orientation"] = *orientation;
        }
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            result["width"] = unsigned_at(payload, 3, 2, false);
            result["height"] = unsigned_at(payload, 1, 2, false);
        }
        offset += length;
    }
    if (!result.contains("width")) throw ToolError("JPEG has no dimension header");
    return result;
}



void inspect_image(Bytes data, bool export_image, json identity) {
    if (data.size() > image_limit) throw ToolError("image exceeds diagnostic's 64 MiB bound");
    json record{{"kind", "image"}};
    record.update(identity);
    record["bytes"] = data.size();
    record["sha256"] = mmltk::tools::sha256(data, "image SHA-256 calculation failed");
    record.update(image_geometry(data));
    report(record);
    if (export_image) {
        const auto suffix = starts_with(data, "\x89PNG") ? ".png" : ".jpg";
        const auto path = fs::path("/evidence") / (identity.at("image_id").dump() + suffix);
        mmltk::tools::write_text(path, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
        report({{"kind", "export"}, {"path", "build/validation/benchmark-image/" + path.filename().string()}});
    }
}

void inspect_file(const fs::path& path, std::uint64_t identity, bool export_image) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw ToolError("cannot read " + path.string());
    std::array<unsigned char, 65536> buffer{};
    std::vector<unsigned char> data;
    while (data.size() <= image_limit && stream) {
        const auto count = std::min(buffer.size(), image_limit + 1 - data.size());
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count));
        data.insert(data.end(), buffer.begin(), buffer.begin() + stream.gcount());
    }
    if (stream.bad()) throw ToolError("read failed: " + path.string());
    inspect_image(data, export_image, {{"path", path.string()}, {"image_id", identity}});
}

bool member_matches(std::string_view name, std::uint64_t identity) {
    // Python's regex search accepts a trailing digit run, including names such
    // as prefix123.jpg, and permits one final newline after the extension.
    if (name.ends_with('\n')) name.remove_suffix(1);
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string extension(name.substr(dot + 1));
    for (auto& ch : extension) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    if (extension != "jpg" && extension != "jpeg" && extension != "png") return false;
    auto begin = dot;
    while (begin > 0 && name[begin - 1] >= '0' && name[begin - 1] <= '9') --begin;
    if (begin == dot) return false;
    auto digits = name.substr(begin, dot - begin);
    while (digits.size() > 1 && digits.front() == '0') digits.remove_prefix(1);
    return digits == std::to_string(identity);
}

void inspect_archive(const fs::path& path, std::uint64_t identity, bool export_image) {
    report({{"kind", "archive_scan"}, {"path", path.string()}, {"image_id", identity}});
    std::unique_ptr<struct archive, decltype(&archive_read_free)> archive(archive_read_new(), archive_read_free);
    if (!archive) throw ToolError("cannot allocate archive reader");
    const auto check = [&](int result) {
        if (result < ARCHIVE_OK) {
            const char* cause = archive_error_string(archive.get());
            throw ToolError(cause ? cause : "archive read failed");
        }
    };
    check(archive_read_support_filter_none(archive.get()));
    check(archive_read_support_filter_gzip(archive.get()));
    check(archive_read_support_filter_bzip2(archive.get()));
    check(archive_read_support_filter_xz(archive.get()));
    check(archive_read_support_format_tar(archive.get()));
    check(archive_read_open_filename(archive.get(), path.c_str(), 10240));
    struct archive_entry* entry = nullptr;
    while (true) {
        const auto status = archive_read_next_header(archive.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        check(status);
        const char* name = archive_entry_pathname(entry);
        if (!name || archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) ||
            !member_matches(name, identity)) continue;
        const auto size = archive_entry_size(entry);
        if (size < 0) throw ToolError("invalid archive member size");
        if (static_cast<std::uint64_t>(size) > image_limit)
            throw ToolError("image exceeds diagnostic's 64 MiB bound");
        std::vector<unsigned char> data(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto bytes = archive_read_data(archive.get(), data.data() + offset, data.size() - offset);
            if (bytes < 0) {
                const char* cause = archive_error_string(archive.get());
                throw ToolError(cause ? cause : "archive image read failed");
            }
            if (bytes == 0) throw ToolError("truncated archive image");
            offset += static_cast<std::size_t>(bytes);
        }
        inspect_image(data, export_image, {{"archive", path.string()}, {"member", name}, {"image_id", identity}});
        return;
    }
    throw ToolError("requested image is absent from archive");
}

int run(int argc, char** argv) {
    auto options = parse(argc, argv);
    if (options.help) { help(); return 0; }
    // Capability queries are deliberately performed before opening any inputs,
    // including before source-ID and mask-export admission in the original CLI.
    if (mmltk::tools::env("MMLTK_BENCHMARK_NATIVE_REQUIREMENTS") == "1") {
        std::cout << "native:";
        if (options.compiled) std::cout << "compiled";
        if (options.parquet || options.panoptic) std::cout << (options.compiled ? ",mask" : "mask");
        std::cout << '\n';
        return 0;
    }
    if (options.compiled) {
        const auto identity = compiled_sample(options);
        if (options.image_id && options.image_id != identity)
            argument_error("--image-id disagrees with the compiled sample");
        options.image_id = identity;
    }
    if (!options.image_id && !options.compiled)
        argument_error("--image-id or --compiled with --sample is required");
    const auto identity = options.image_id ? std::optional(image_identity(*options.image_id)) : std::nullopt;
    if (!identity && (!options.indexes.empty() || options.image || options.archive || options.parquet || options.panoptic))
        argument_error("this compiled sample has no source image ID");
    if (options.export_runs && options.indexes.size() != 1)
        argument_error("--export-mask-runs requires exactly one --index");
    if (options.parquet || options.panoptic) {
        const auto executable = native_helper("MMLTK_COCONUT_MASK_DIAGNOSTIC");
        for (const auto& [mode, path] : std::array<std::pair<std::string_view, std::optional<fs::path>>, 2>{
                 std::pair{"parquet", options.parquet}, std::pair{"png", options.panoptic}}) {
            if (!path) continue;
            mmltk::tools::RunOptions execution;
            execution.timeout = std::chrono::seconds(120);
            const auto result = mmltk::tools::run({executable, std::string(mode), path->string(),
                                                 std::to_string(*identity), options.export_image ? "1" : "0"}, execution);
            check_timeout(result, 120);
            if (result.exit_code) throw ToolError("mask diagnostic exited with status " +
                                                  std::to_string(result.signal ? -result.signal : result.exit_code));
        }
    }
    for (const auto& path : options.indexes) annotation_rows(path, identity, options.objects, options.export_runs);
    if (options.image) inspect_file(*options.image, *identity, options.export_image);
    if (options.archive) inspect_archive(*options.archive, *identity, options.export_image);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const ToolError& error) {
        std::cerr << (error.status == 2 ? "diagnose_benchmark_image: error: " : "benchmark image diagnostic: ")
                  << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << "benchmark image diagnostic: " << error.what() << '\n';
        return 1;
    }
}
