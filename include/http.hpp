#pragma once

#include <string>

namespace sui_pow {

struct HttpOptions {
    std::string proxy;
    std::string proxy_username;
    std::string proxy_password;
};

std::string normalize_http_proxy(const std::string& input);

std::string http_post_json(const std::string& url,
                           const std::string& body,
                           const std::string& extra_headers = {},
                           const HttpOptions& options = {});

}  // namespace sui_pow
