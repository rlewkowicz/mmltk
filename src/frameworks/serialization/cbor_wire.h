#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
namespace mmltk::frameworks::serialization::wire {
inline constexpr std::size_t kMaximumNestingDepth = 64U;
using ByteBuffer = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;
struct ByteSegments {
    ByteView first{};
    ByteView second{};
    [[nodiscard]] constexpr std::size_t size() const noexcept { return first.size() + second.size(); }
    [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0U; }
};
enum class ErrorCode : std::uint8_t {
    UnexpectedEof,
    NonMinimal,
    InvalidMajorType,
    InvalidAdditionalInfo,
    InvalidUtf8,
    InvalidFloat,
    Overflow,
    LimitExceeded,
    DepthExceeded,
    IndefiniteContainer,
    TrailingData,
    MalformedItem,
    DuplicateKey,
    UnknownKey,
    TypeMismatch,
};
struct Error {
    ErrorCode code = ErrorCode::MalformedItem;
    std::size_t offset = 0U;
    std::string path;
};
using DecodeError = Error;
using EncodeError = Error;
class Value {
   public:
    using Bytes = ByteBuffer;
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Bytes, Array, Object>;
    Storage storage{};
    Value() = default;
    Value(const Value&) = default;
    Value(Value&&) noexcept = default;
    Value& operator=(const Value&) = default;
    Value& operator=(Value&&) noexcept = default;
    bool operator==(const Value&) const = default;
    explicit Value(const std::int64_t value) noexcept : storage(value < 0 ? Storage{value} : Storage{static_cast<std::uint64_t>(value)}) {}
    template <class T>
        requires(!std::same_as<std::remove_cvref_t<T>, Value> && std::constructible_from<Storage, T>)
    explicit Value(T&& value) : storage(std::forward<T>(value)) {}
};
// A declaration-owned recursive budget for the dynamic CBOR vocabularies.
// Text, byte buffers, object keys, arrays, and objects are constrained at
// every nesting level; depth is measured from the root value at zero.
struct DynamicValueLimits final {
    std::size_t max_bytes = 0U;
    std::size_t max_items = 0U;
    std::size_t max_depth = 0U;
};
// Bounded mutable leaves deliberately exclude recursive CBOR containers. This
// preserves the canonical untagged representation while keeping ownership
// graphs finite and inspectable.
class FlatValue final {
   public:
    FlatValue() = default;
    FlatValue(std::monostate) noexcept;
    FlatValue(bool value) noexcept;
    FlatValue(std::int64_t value) noexcept;
    FlatValue(std::uint64_t value) noexcept;
    FlatValue(double value) noexcept;
    FlatValue(const FlatValue&) = default;
    FlatValue(FlatValue&&) noexcept = default;
    FlatValue& operator=(const FlatValue&) = default;
    FlatValue& operator=(FlatValue&&) noexcept = default;
    bool operator==(const FlatValue&) const = default;
    // Factory input is deliberately a sequence of FlatValue instances rather
    // than the private scalar variant. Every element is checked to be scalar
    // before the one flat array allocation occurs.
    [[nodiscard]] static std::expected<FlatValue, DecodeError> text(std::string_view value, std::size_t max_bytes);
    [[nodiscard]] static std::expected<FlatValue, DecodeError> bytes(ByteView value, std::size_t max_bytes);
    [[nodiscard]] static std::expected<FlatValue, DecodeError> array(std::span<const FlatValue> values, DynamicValueLimits limits);
    [[nodiscard]] static std::expected<FlatValue, DecodeError> from_value(const Value& value, DynamicValueLimits limits);
    // Exposes the finite leaf vocabulary without materializing recursive Value
    // ownership. Callers may inspect a scalar or the existing scalar array
    // directly for the duration of this call.
    template <class Visitor>
    decltype(auto) visit(Visitor&& visitor) const& {
        return std::visit(std::forward<Visitor>(visitor), storage);
    }
    template <class Visitor>
    decltype(auto) visit(Visitor&& visitor) && {
        return std::visit(std::forward<Visitor>(visitor), std::move(storage));
    }

   private:
    using Scalar = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, ByteBuffer>;
    using Array = std::vector<Scalar>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, ByteBuffer, Array>;
    FlatValue(std::string value);
    FlatValue(ByteBuffer value);
    explicit FlatValue(Storage value) : storage(std::move(value)) {}
    Storage storage{};
    friend class Reader;
    friend bool dynamic_value_within_limits(const FlatValue& value, DynamicValueLimits limits) noexcept;
};
[[nodiscard]] bool dynamic_value_within_limits(const Value& value, DynamicValueLimits limits) noexcept;
[[nodiscard]] bool dynamic_value_within_limits(const FlatValue& value, DynamicValueLimits limits) noexcept;
// Encoder-only substitution for one reflected array. Raw CBOR items never
// enter Value's semantic vocabulary or the runtime schema.
struct RawArrayItems {
    const Value::Array* target = nullptr;
    std::span<const ByteSegments> items{};
};
enum class AllocationKind : std::uint8_t { Text, Bytes, Sequence, Object };
struct DecodePathElement {
    std::string_view name;
    std::string_view object_kind;
};
struct AllocationRequest {
    std::span<const DecodePathElement> path;
    AllocationKind kind{};
    std::size_t size = 0U;
};
struct AllocationPolicy {
    const void* context = nullptr;
    bool (*allows)(const void*, const AllocationRequest&) noexcept = nullptr;
};
struct Limits {
    std::size_t max_bytes = 0U;
    std::size_t max_items = 0U;
    std::size_t max_depth = kMaximumNestingDepth;
    AllocationPolicy allocation_policy{};
};
// Fixed caller-owned workspace for allocation-free structural map-key
// validation. A span with Limits::max_items entries is sufficient for every
// valid input admitted by those limits, including nested maps.
struct StructuralValidationScratch final {
    std::span<std::uint32_t> key_offsets{};
};
// CBOR's argument encoding is shared by the reader, writer, and counter.  Keeping
// it here makes all budget calculations use the same shortest-form rule.
[[nodiscard]] constexpr std::size_t head_size(const std::uint64_t value) noexcept {
    return value < 24U ? 1U : value <= 0xffU ? 2U : value <= 0xffffU ? 3U : value <= 0xffffffffU ? 5U : 9U;
}
enum class CanonicalFloatWidth : std::uint8_t {
    Half = 2U,
    Single = 4U,
    Double = 8U,
};
struct CanonicalFloatEncoding final {
    CanonicalFloatWidth width = CanonicalFloatWidth::Double;
    std::uint64_t bits = 0U;
};
// The dynamic and fixed encoders share this one shortest-form decision so
// every emitted float is accepted by the canonical reader.
[[nodiscard]] std::expected<CanonicalFloatEncoding, ErrorCode> canonical_float_encoding(double value) noexcept;
class Reader {
   public:
    class PathScope final {
       public:
        PathScope() = default;
        PathScope(const PathScope&) = delete;
        PathScope& operator=(const PathScope&) = delete;
        PathScope(PathScope&& other) noexcept : reader_(std::exchange(other.reader_, nullptr)) {}
        PathScope& operator=(PathScope&& other) noexcept {
            if (this != &other) {
                release();
                reader_ = std::exchange(other.reader_, nullptr);
            }
            return *this;
        }
        ~PathScope() { release(); }

       private:
        explicit PathScope(Reader& reader) noexcept : reader_(&reader) {}
        void release() noexcept;
        Reader* reader_ = nullptr;
        friend class Reader;
    };
    Reader(ByteSegments bytes, Limits limits, StructuralValidationScratch scratch = {}) noexcept;
    [[nodiscard]] std::expected<Value, DecodeError> read();
    // These operations are the reader's reusable typed-projection hook. They
    // share the canonical CBOR head, shortest-form, UTF-8, float, depth, item,
    // byte, and allocation checks with read(); they do not expose parser state
    // or a second CBOR vocabulary.
    [[nodiscard]] std::expected<FlatValue, DecodeError> read_flat();
    [[nodiscard]] std::expected<FlatValue, DecodeError> read_scalar_item(std::size_t depth);
    [[nodiscard]] std::expected<Value, DecodeError> read_value_item(std::size_t depth);
    [[nodiscard]] std::expected<FlatValue, DecodeError> read_flat_item(std::size_t depth);
    [[nodiscard]] std::expected<std::size_t, DecodeError> begin_array_item(std::size_t depth);
    [[nodiscard]] std::expected<ByteSegments, DecodeError> borrow_bytes_item(std::size_t depth);
    [[nodiscard]] std::expected<std::size_t, DecodeError> begin_object_item(std::size_t depth);
    [[nodiscard]] std::expected<std::string, DecodeError> read_object_key(std::size_t depth);
    // Allocation-free exact text projection for fixed protocol keys and
    // discriminants. The item still passes canonical head, byte, item, and
    // depth admission through this Reader.
    [[nodiscard]] std::expected<std::size_t, DecodeError> read_text_choice(std::size_t depth, std::span<const std::string_view> choices);
    [[nodiscard]] std::expected<void, DecodeError> expect_text_item(std::size_t depth, std::string_view expected);
    [[nodiscard]] PathScope enter_path(std::string_view name);
    [[nodiscard]] std::expected<void, DecodeError> finish();
    [[nodiscard]] std::expected<bool, DecodeError> next_is_null() const;
    [[nodiscard]] DecodeError contextualize(DecodeError error) const;
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t items_read() const noexcept { return items_; }

   private:
    struct ItemHead {
        std::uint8_t initial = 0U;
        std::uint8_t major = 0U;
        std::uint8_t additional = 0U;
    };
    struct TextRange {
        std::size_t item_offset = 0U;
        std::size_t data_offset = 0U;
        std::size_t size = 0U;
    };
    [[nodiscard]] std::expected<std::byte, DecodeError> peek_byte() const;
    [[nodiscard]] std::expected<std::byte, DecodeError> byte();
    [[nodiscard]] std::expected<std::uint64_t, DecodeError> argument(std::uint8_t additional);
    [[nodiscard]] std::expected<std::size_t, DecodeError> size_argument(std::uint8_t additional);
    // Only called after the whole payload passes EOF and byte-budget admission.
    [[nodiscard]] ByteSegments payload_ranges(std::size_t count) const noexcept;
    [[nodiscard]] std::expected<ByteBuffer, DecodeError> bytes(std::size_t count);
    [[nodiscard]] bool allocation_allowed(AllocationKind kind, std::size_t size) const noexcept;
    [[nodiscard]] std::expected<ItemHead, DecodeError> item_head(std::size_t depth);
    [[nodiscard]] std::expected<ByteSegments, DecodeError> borrow_string_item(std::size_t depth, std::uint8_t major);
    [[nodiscard]] std::expected<TextRange, DecodeError> read_structural_object_key(std::size_t depth);
    [[nodiscard]] std::expected<void, DecodeError> insert_structural_object_key(TextRange key, std::span<std::uint32_t> table);
    [[nodiscard]] std::expected<void, DecodeError> validate_structural();
    template <class T>
    [[nodiscard]] std::expected<T, DecodeError> read_document(std::expected<T, DecodeError> (Reader::*read_root)(std::size_t)) {
        if (input_.size() > limits_.max_bytes) return std::unexpected(error(ErrorCode::LimitExceeded));
        auto result = (this->*read_root)(0U);
        if (!result) return std::unexpected(result.error());
        auto completed = finish();
        return completed ? std::expected<T, DecodeError>(std::move(*result)) : std::unexpected(completed.error());
    }
    [[nodiscard]] std::expected<FlatValue, DecodeError> read_flat_scalar(ItemHead head, bool apply_allocation_policy, bool materialize = true);
    [[nodiscard]] std::expected<std::size_t, DecodeError> begin_container_item(std::size_t depth, std::uint8_t expected_major, AllocationKind allocation_kind);
    [[nodiscard]] std::expected<Value, DecodeError> read_item(std::size_t depth, bool apply_allocation_policy = true, bool materialize = true);
    [[nodiscard]] std::expected<std::string, DecodeError> text(std::size_t count);
    [[nodiscard]] DecodeError error(ErrorCode code) const;
    ByteSegments input_{};
    Limits limits_{};
    StructuralValidationScratch structural_scratch_{};
    std::size_t structural_scratch_cursor_ = 0U;
    std::size_t offset_ = 0U;
    std::size_t items_ = 0U;
    std::vector<DecodePathElement> path_;
    friend std::expected<void, DecodeError> validate_raw_item_structural(ByteView bytes, Limits limits, StructuralValidationScratch scratch);
};
class CountingEncoder;
class Writer {
   public:
    explicit Writer(ByteBuffer& destination, Limits limits, RawArrayItems raw_array = {}) noexcept
        : dynamic_destination_(&destination), limits_(limits), raw_array_(raw_array) {}
    [[nodiscard]] std::expected<void, EncodeError> write(const Value& value);
    [[nodiscard]] std::expected<void, EncodeError> append_raw_item(ByteSegments item);
    [[nodiscard]] std::size_t bytes_written() const noexcept;

   private:
    explicit Writer(std::span<std::byte> destination, Limits limits, RawArrayItems raw_array = {}) noexcept
        : fixed_destination_(destination), limits_(limits), raw_array_(raw_array) {}
    explicit Writer(Limits limits, RawArrayItems raw_array) noexcept : limits_(limits), raw_array_(raw_array), count_only_(true) {}
    friend std::expected<std::size_t, EncodeError> encode(const Value&, std::span<std::byte>, Limits);
    friend class CountingEncoder;
    [[nodiscard]] std::size_t destination_size() const noexcept;
    void truncate(std::size_t size) noexcept;
    [[nodiscard]] std::expected<void, EncodeError> append(ByteView bytes);
    [[nodiscard]] std::expected<void, EncodeError> head(std::uint8_t major, std::uint64_t argument);
    [[nodiscard]] std::expected<void, EncodeError> put(std::byte value);
    [[nodiscard]] std::expected<void, EncodeError> append_raw_item(ByteSegments item, std::size_t depth);
    [[nodiscard]] std::expected<void, EncodeError> write_item(const Value& value, std::size_t depth);
    ByteBuffer* dynamic_destination_ = nullptr;
    std::span<std::byte> fixed_destination_{};
    std::size_t fixed_size_ = 0U;
    Limits limits_{};
    RawArrayItems raw_array_{};
    std::size_t items_ = 0U;
    bool count_only_ = false;
};
class CountingEncoder {
   public:
    explicit CountingEncoder(Limits limits, RawArrayItems raw_array = {}) noexcept : limits_(limits), raw_array_(raw_array) {}
    [[nodiscard]] std::expected<std::size_t, EncodeError> measure(const Value& value);

   private:
    Limits limits_{};
    RawArrayItems raw_array_{};
};
[[nodiscard]] std::expected<Value, DecodeError> decode(ByteSegments bytes, Limits limits);
[[nodiscard]] std::expected<void, EncodeError> encode(const Value& value, ByteBuffer& destination, Limits limits);
[[nodiscard]] std::expected<void, EncodeError> encode(const Value& value, RawArrayItems raw_array, ByteBuffer& destination, Limits limits);
[[nodiscard]] std::expected<std::size_t, EncodeError> encode(const Value& value, std::span<std::byte> destination, Limits limits);
[[nodiscard]] std::expected<void, DecodeError> validate_raw_item(ByteSegments bytes, Limits limits);
// Allocation-free structural validation for one contiguous fixed transport
// buffer. It shares the canonical reader language without materializing Value.
// Scratch with limits.max_items entries proves capacity for nested map keys.
[[nodiscard]] std::expected<void, DecodeError> validate_raw_item_structural(ByteView bytes, Limits limits, StructuralValidationScratch scratch);
}  // namespace mmltk::frameworks::serialization::wire
