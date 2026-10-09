export module mmltk.tools.cpp_text;
import std;
export {
 namespace mmltk::tools::text {
 inline bool oneOf(std::string_view value, std::initializer_list<std::string_view> choices) { return std::find(choices.begin(), choices.end(), value) != choices.end(); }
 class JsonStringState {
 public:
  // True for every byte belonging to a quoted string, including its quotes.
  bool consume(char value) {
   const bool inside = quoted_ || value == '"';
   if (quoted_) {
    if (escaped_)
     escaped_ = false;
    else if (value == '\\')
     escaped_ = true;
    else if (value == '"')
     quoted_ = false;
   } else if (value == '"')
    quoted_ = true;
   return inside;
  }
  bool quoted() const { return quoted_; }

 private:
  bool quoted_ = false, escaped_ = false;
 };
 constexpr bool digit(char16_t ch) { return ch >= u'0' && ch <= u'9'; }
 constexpr bool letter(char16_t ch) { return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z'); }
 constexpr bool word(char16_t ch) { return letter(ch) || digit(ch) || ch == u'_'; }
 // ECMAScript whitespace, including its BOM and excluding Python's additional
 // control separators. Keep language-specific predicates explicit at callers.
 constexpr bool whitespace(char16_t ch) {
  return ch == u' ' || (ch >= u'\t' && ch <= u'\r') || ch == 0x00a0 || ch == 0x1680 || (ch >= 0x2000 && ch <= 0x200a) || ch == 0x2028 || ch == 0x2029 || ch == 0x202f || ch == 0x205f || ch == 0x3000 ||
         ch == 0xfeff;
 }
 inline std::u16string_view trimUnits(std::u16string_view value) {
  while (!value.empty() && whitespace(value.front())) value.remove_prefix(1);
  while (!value.empty() && whitespace(value.back())) value.remove_suffix(1);
  return value;
 }
 template <class Char>
 std::vector<std::size_t> lineStarts(std::basic_string_view<Char> source) {
  std::vector<std::size_t> result{0};
  for (std::size_t i = 0; i < source.size(); ++i)
   if (source[i] == '\n') result.push_back(i + 1);
  return result;
 }
 inline std::string trim_newlines(std::string value) {
  while (!value.empty() && value.back() == '\n') value.pop_back();
  return value;
 }
 inline std::string decode_utf8(std::string_view bytes, bool universal_newlines = false) {
  std::string result;
  result.reserve(bytes.size());
  for (std::size_t i = 0; i < bytes.size();) {
   const auto first = static_cast<unsigned char>(bytes[i]);
   if (universal_newlines && first == '\r') {
    result += '\n';
    i += i + 1 < bytes.size() && bytes[i + 1] == '\n' ? 2 : 1;
    continue;
   }
   const std::size_t count = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
   if (count == 1) {
    result += bytes[i++];
    continue;
   }
   std::size_t valid = 1;
   while (count && valid < count && i + valid < bytes.size()) {
    const auto next = static_cast<unsigned char>(bytes[i + valid]);
    if (next < 0x80 || next > 0xbf || (valid == 1 && ((first == 0xe0 && next < 0xa0) || (first == 0xed && next > 0x9f) || (first == 0xf0 && next < 0x90) || (first == 0xf4 && next > 0x8f)))) break;
    ++valid;
   }
   if (count && valid == count)
    result.append(bytes.substr(i, count));
   else
    result += "\xef\xbf\xbd";
   i += valid;
  }
  return result;
 }
 inline char repr_quote(std::string_view value) { return value.find('\'') != std::string_view::npos && value.find('"') == std::string_view::npos ? '"' : '\''; }
 inline bool append_repr_escape(std::string& result, std::uint32_t ch, char quote) {
  if (ch == static_cast<unsigned char>(quote) || ch == '\\') {
   result += '\\';
   result += static_cast<char>(ch);
  } else if (ch == '\n')
   result += "\\n";
  else if (ch == '\r')
   result += "\\r";
  else if (ch == '\t')
   result += "\\t";
  else
   return false;
  return true;
 }
 inline std::string byte_repr(std::string_view value, bool escape_non_ascii = false) {
  const char quote = repr_quote(value);
  std::string result(1, quote);
  constexpr char hex[] = "0123456789abcdef";
  for (const unsigned char ch : value) {
   if (append_repr_escape(result, ch, quote)) continue;
   if (ch < 32 || ch == 127 || (escape_non_ascii && ch >= 128)) {
    result += "\\x";
    result += hex[ch >> 4];
    result += hex[ch & 15];
   } else
    result += static_cast<char>(ch);
  }
  return result + quote;
 }
 // Arbitrary precision decimal selectors: preserve sign and normalize digit
 // separators without narrowing to a machine integer.
 inline std::optional<std::string> decimal_integer(std::string_view text) {
  std::string digits;
  std::size_t index = 0;
  const bool negative = !text.empty() && text.front() == '-';
  if (!text.empty() && (text.front() == '-' || text.front() == '+')) ++index;
  bool previous_digit = false;
  for (; index < text.size(); ++index) {
   const auto ch = static_cast<unsigned char>(text[index]);
   if (digit(ch)) {
    digits += static_cast<char>(ch);
    previous_digit = true;
   } else if (ch == '_' && previous_digit && index + 1 < text.size() && digit(static_cast<unsigned char>(text[index + 1])))
    previous_digit = false;
   else
    return std::nullopt;
  }
  if (digits.empty()) return std::nullopt;
  const auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return "0";
  return (negative ? "-" : "") + digits.substr(first);
 }
 }  // namespace mmltk::tools::text
}
