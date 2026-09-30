#pragma once
#include <simdjson.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>
namespace mmltk::backend::data::detail {
// External JSON integers beyond the signed/unsigned domains retain their finite
// floating interpretation. Field-specific identity/flag rules belong to callers.
// Both consumers use the parser's raw token only for this numeric fallback.
template<class Consumer,class Reject>
void consume_compile_json_number(simdjson::ondemand::value value,Consumer&& consume,Reject&& reject) {
 auto token=value.raw_json_token();
 while (!token.empty() && static_cast<unsigned char>(token.back())<=' ') token.remove_suffix(1);
 auto parsed=value.get_number();
 auto digits=token;
 if (digits.starts_with('-')) digits.remove_prefix(1);
 const bool large_integer=!digits.empty() && digits.front()>='1' && digits.front()<='9' &&
  std::ranges::all_of(digits,[](char character) { return character>='0' && character<='9'; });
 if (parsed.error()==simdjson::BIGINT_ERROR || (parsed.error()==simdjson::NUMBER_ERROR && large_integer)) {
  double fallback=0;
  const auto result=std::from_chars(token.data(),token.data()+token.size(),fallback);
  auto end=result.ptr;
  while (end!=token.data()+token.size() && static_cast<unsigned char>(*end)<=' ') ++end;
  if (result.ec!=std::errc{} || end!=token.data()+token.size() || !std::isfinite(fallback)) reject("JSON number exceeds admission");
  consume(fallback); return;
 }
 const auto number=parsed.value();
 if (number.is_uint64()) consume(number.get_uint64());
 else if (number.is_int64()) consume(number.get_int64());
 else consume(number.get_double());
}
}
