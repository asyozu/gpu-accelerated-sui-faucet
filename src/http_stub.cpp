#include "http.hpp"

#include <stdexcept>

namespace sui_pow {

std::string http_post_json(const std::string&,
                           const std::string&,
                           const std::string&,
                           const HttpOptions&) {
    throw std::runtime_error("the faucet HTTP client currently targets Windows WinHTTP");
}

std::string http_get(const std::string&, const HttpOptions&) {
    throw std::runtime_error("the faucet HTTP client currently targets Windows WinHTTP");
}

}  // namespace sui_pow
