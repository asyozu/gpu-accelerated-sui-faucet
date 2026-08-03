#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sui_pow::json_lite {

std::string_view find_field(std::string_view object, std::string_view key);
std::string decode_string(std::string_view json_string);
std::string escape_string(std::string_view text);
std::uint64_t parse_u64(std::string_view value);

}  // namespace sui_pow::json_lite

