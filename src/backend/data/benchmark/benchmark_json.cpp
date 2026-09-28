#include "src/backend/data/benchmark/detail/benchmark_json.h"
#include "src/backend/data/benchmark/detail/benchmark_annotations.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <cerrno>
namespace mmltk::backend::data::benchmark_internal {
using mmltk::common::io::FileHandle;
using mmltk::common::io::errno_error;
[[nodiscard]] bool consume_json_string_token(const char token, bool& in_string, bool& escaped) noexcept {
 if (!in_string) {
  if (token == '"') {
   in_string = true;
   return true;
  }
  return false;
 }
 if (escaped) {
  escaped = false;
 } else if (token == '\\') {
  escaped = true;
 } else if (token == '"') {
  in_string = false;
 }
 return true;
}
PaddedMappedFile::PaddedMappedFile(const std::filesystem::path& path) : file_(FileHandle::open_readonly(path.string())) {
  size_ = file_.size();
  if (size_ == 0U) { throw AnnotationDocumentRejected("benchmark annotation file is empty: " + path.string()); }
  if (size_ > std::numeric_limits<std::size_t>::max() - simdjson::SIMDJSON_PADDING) { throw std::overflow_error("benchmark annotation mapping size overflow"); }
  capacity_ = size_ + simdjson::SIMDJSON_PADDING;
  void* reservation = ::mmap(nullptr, capacity_, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED) { throw errno_error("cannot reserve padded benchmark annotation mapping", path.string()); }
  data_ = static_cast<const char*>(reservation);
  void* mapped = ::mmap(reservation, size_, PROT_READ, MAP_PRIVATE | MAP_FIXED, file_.get(), 0);
  if (mapped == MAP_FAILED) {
   const int saved_errno = errno;
   (void)::munmap(reservation, capacity_);
   data_ = nullptr;
   errno = saved_errno;
   throw errno_error("cannot map benchmark annotation file", path.string());
  }
 }
PaddedMappedFile::~PaddedMappedFile() {
  if (data_ != nullptr) { (void)::munmap(const_cast<char*>(data_), capacity_); }
 }
 std::size_t PaddedMappedFile::capacity_from(const std::size_t offset) const {
  if (offset > capacity_) { throw std::runtime_error("benchmark annotation mapping offset is out of bounds"); }
  return capacity_ - offset;
 }
[[noreturn]] void reject_json_document(const simdjson::simdjson_error& error) {
 // Only errors that describe source syntax or source values are optional-source
 // rejection. Allocation, parser capacity/setup and API misuse retain their type.
 switch (error.error()) {
  case simdjson::TAPE_ERROR:
  case simdjson::DEPTH_ERROR:
  case simdjson::STRING_ERROR:
  case simdjson::T_ATOM_ERROR:
  case simdjson::F_ATOM_ERROR:
  case simdjson::N_ATOM_ERROR:
  case simdjson::NUMBER_ERROR:
  case simdjson::UTF8_ERROR:
  case simdjson::EMPTY:
  case simdjson::UNESCAPED_CHARS:
  case simdjson::UNCLOSED_STRING:
  case simdjson::INCORRECT_TYPE:
  case simdjson::NUMBER_OUT_OF_RANGE:
  case simdjson::BIGINT_ERROR:
  case simdjson::NO_SUCH_FIELD:
  case simdjson::INCOMPLETE_ARRAY_OR_OBJECT:
  case simdjson::TRAILING_CONTENT: throw AnnotationDocumentRejected(error.what());
  default: throw;
 }
}

namespace {
// Structural admission retains only offsets. Selected rows are projected once
// later; ignored COCONut values still receive the document's syntax/depth checks.
class JsonStructure final {
 const PaddedMappedFile& input_;
 mmltk::common::concurrency::CancellationObservation cancellation_;
 BenchmarkCompilePipeline* execution_;
 bool strict_;
 std::size_t checkpoint_ = 0;
public:
 std::size_t position = 0;
 JsonStructure(const PaddedMappedFile& input, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkCompilePipeline* execution, bool strict)
  : input_(input), cancellation_(cancellation), execution_(execution), strict_(strict) {}
 [[noreturn]] static void fail() { throw AnnotationDocumentRejected("invalid benchmark JSON envelope"); }
 char peek() const { return position < input_.size() ? input_.data()[position] : '\0'; }
 char take() {
  if (position == input_.size()) fail();
  if (position >= checkpoint_) {
   if (cancellation_.requested()) throw std::runtime_error("benchmark dataset compilation cancelled");
   if (execution_) execution_->cooperate();
   checkpoint_ = position > SIZE_MAX - 65536 ? SIZE_MAX : position + 65536;
  }
  return input_.data()[position++];
 }
 void space() { while (peek() && (strict_ ? peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t' : static_cast<unsigned char>(peek()) <= ' ')) take(); }
 void expect(char token) { if (take() != token) fail(); }
 unsigned hex4() {
  unsigned value = 0;
  for (int i = 0; i < 4; ++i) {
   const auto token = take();
   const auto digit = token >= '0' && token <= '9' ? token - '0' : token >= 'a' && token <= 'f' ? token - 'a' + 10 : token >= 'A' && token <= 'F' ? token - 'A' + 10 : -1;
   if (digit < 0) fail();
   value = value * 16 + static_cast<unsigned>(digit);
  }
  return value;
 }
 // Envelope keys can be arbitrarily long. Retain only enough decoded ASCII
 // for the known names, while validating every codepoint without a token buffer.
 std::string string(std::size_t capture = 0) {
  expect('"');
  std::string result;
  result.reserve(capture);
  bool retained = true;
  for (;;) {
   unsigned code = static_cast<unsigned char>(take());
   if (code == '"') return result;
   if (code < 32) fail();
   if (code == '\\') {
    const auto escaped = take();
    switch (escaped) {
     case '"': case '\\': case '/': code = escaped; break;
     case 'b': code = '\b'; break; case 'f': code = '\f'; break; case 'n': code = '\n'; break; case 'r': code = '\r'; break; case 't': code = '\t'; break;
     case 'u': {
      code = hex4();
      if (code >= 0xd800 && code <= 0xdbff) {
       expect('\\'); expect('u'); const auto low = hex4();
       if (low < 0xdc00 || low > 0xdfff) fail();
       code = 0x10000 + (code - 0xd800) * 1024 + low - 0xdc00;
      } else if (code >= 0xdc00 && code <= 0xdfff) fail();
      break;
     }
     default: fail();
    }
   } else if (code >= 128) {
    const auto first = code;
    const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 : first <= 0xef && first >= 0xe0 ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
    if (!count) fail();
    code &= (1U << (6 - count)) - 1;
    for (unsigned i = 0; i < count; ++i) { const auto next = static_cast<unsigned char>(take()); if ((next & 0xc0) != 0x80) fail(); code = (code << 6) | (next & 63); }
    if ((count == 2 && code < 0x800) || (count == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) fail();
   }
   if (retained && capture) {
    if (code >= 128 || result.size() == capture) { result.clear(); retained = false; }
    else result.push_back(static_cast<char>(code));
   }
  }
 }
 void value(unsigned depth, bool numeric_admission = true) {
  if (depth > 16) throw AnnotationDocumentRejected("annotation JSON nesting exceeds admission");
  space();
  const auto token = peek();
  if (token == '"') { (void)string(); return; }
  if (token == '{' || token == '[') {
   take(); space(); const auto closing = token == '{' ? '}' : ']';
   if (peek() == closing) { take(); return; }
   for (;;) {
    if (token == '{') { (void)string(); space(); expect(':'); }
    value(depth + 1, numeric_admission); space();
    if (peek() == closing) { take(); return; }
    expect(','); space();
   }
  }
  for (auto literal : {std::string_view{"true"}, std::string_view{"false"}, std::string_view{"null"}})
   if (token == literal.front()) { for (char character : literal) expect(character); return; }
  const auto begin = position;
  if (peek() == '-') take();
  const auto digit = [&] { return peek() >= '0' && peek() <= '9'; };
  if (peek() == '0') take();
  else { if (!digit()) fail(); while (digit()) take(); }
  if (peek() == '.') { take(); if (!digit()) fail(); while (digit()) take(); }
  if (peek() == 'e' || peek() == 'E') { take(); if (peek() == '+' || peek() == '-') take(); if (!digit()) fail(); while (digit()) take(); }
  if (numeric_admission) {
   // strtod preserves the document parser's finite-overflow/underflow policy.
   // The token ends at a structural delimiter in the padded mapping.
   char* end = nullptr;
   const auto number = std::strtod(input_.data() + begin, &end);
   if (end != input_.data() + position || !std::isfinite(number)) fail();
  }
 }
 // Stock discovery historically only balanced structure; row parsers own its
 // semantic rejection policy. Separate counts avoid an input-sized stack.
 void stock_value() {
  std::size_t objects = 0, arrays = 0;
  bool in_string = false, escaped = false;
  const auto begin = position;
  for (;;) {
   const auto token = peek();
   if (!token) { if (objects || arrays || in_string) fail(); return; }
   if (!in_string && !objects && !arrays && position != begin && (token == ',' || token == '}')) return;
   take();
   if (consume_json_string_token(token, in_string, escaped)) continue;
   if (token == '{') ++objects;
   else if (token == '[') ++arrays;
   else if (token == '}') { if (!objects) fail(); --objects; }
   else if (token == ']') { if (!arrays) fail(); --arrays; }
   if (!objects && !arrays && (token == '}' || token == ']')) return;
  }
 }
};
}
void discover_json_arrays(const PaddedMappedFile& file, std::span<const std::string_view> names, bool reject_duplicates,
 const std::function<void(std::size_t, std::span<const ByteRange>, bool)>& consume,
 mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkCompilePipeline* execution) {
 JsonStructure input(file, cancellation, execution, reject_duplicates);
 std::vector<bool> found(names.size());
 std::size_t key_limit = 0;
 for (auto name : names) key_limit = std::max(key_limit, name.size());
 std::optional<std::size_t> active;
 bool started = false, done = false, after_value = false, need_separator = false;
 std::vector<ByteRange> rows;
 const auto lanes = execution ? execution->workers() : std::size_t{1};
 const auto row_limit = lanes * 1024;
 const auto byte_limit = lanes * (256U << 10);
 rows.reserve(row_limit);
 while (!done) {
  rows.clear();
  std::optional<std::size_t> published;
  bool complete = false;
  const auto scan = [&](std::size_t) {
   if (!started) {
    if (reject_duplicates && file.size() >= 3 && static_cast<unsigned char>(file.data()[0]) == 0xef && static_cast<unsigned char>(file.data()[1]) == 0xbb && static_cast<unsigned char>(file.data()[2]) == 0xbf) { input.take(); input.take(); input.take(); }
    input.space(); input.expect('{'); input.space(); started = true;
   }
   for (;;) {
    if (active) {
     published = active;
     while (input.peek() != ']') {
      if (need_separator) { input.expect(','); input.space(); }
      if (!reject_duplicates) while (input.peek() == ',') { input.take(); input.space(); }
      if (!reject_duplicates && input.peek() == ']') break;
      if (input.peek() != '{') throw AnnotationDocumentRejected("benchmark annotation array contains a non-object value");
      const auto begin = input.position;
      if (reject_duplicates) input.value(2, false); else input.stock_value();
      rows.push_back({begin, input.position});
      input.space(); need_separator = reject_duplicates;
      if (input.peek() == ']') break;
      if (rows.size() == row_limit || rows.back().end - rows.front().begin >= byte_limit) return;
     }
     input.expect(']'); active.reset(); after_value = true; complete = true;
     return;
    }
    input.space();
    if (after_value) {
     if (input.peek() != '}') {
      input.expect(','); input.space();
      if (reject_duplicates && input.peek() == '}') JsonStructure::fail();
     }
     after_value = false;
    }
    if (input.peek() == '}') {
     input.take(); input.space(); if (input.position != file.size()) JsonStructure::fail();
     for (std::size_t i = 0; i < names.size(); ++i) if (!found[i]) throw AnnotationDocumentRejected("benchmark annotation JSON is missing array '" + std::string(names[i]) + "'");
     done = true; return;
    }
    const auto key = input.string(key_limit);
    const auto match = std::ranges::find(names, key);
    const auto selected = static_cast<std::size_t>(match - names.begin());
    input.space(); input.expect(':'); input.space();
    const bool wanted = selected < names.size();
    if (wanted && found[selected] && reject_duplicates) throw AnnotationDocumentRejected("duplicate panoptic envelope field");
    if (wanted && !found[selected]) {
     if (input.peek() != '[') throw AnnotationDocumentRejected("benchmark annotation JSON field is not an array");
     input.take(); input.space(); found[selected] = true; active = selected; need_separator = false;
    } else {
     if (reject_duplicates) input.value(1); else input.stock_value();
     after_value = true;
    }
   }
  };
  if (execution) execution->run(BenchmarkStage::Metadata, {65536, 0}, scan); else scan(0);
  if (published) consume(*published, rows, complete);
 }
}
} // namespace mmltk::backend::data::benchmark_internal
