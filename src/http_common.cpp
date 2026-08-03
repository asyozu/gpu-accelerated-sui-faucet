#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace sui_pow {

std::string normalize_http_proxy(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    std::string proxy = input;
    std::string lowercase = proxy;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](char value) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(value)));
    });
    if (lowercase.rfind("http://", 0) == 0) {
        proxy.erase(0, 7);
    } else if (lowercase.find("://") != std::string::npos) {
        throw std::invalid_argument(
            "--proxy supports HTTP CONNECT proxies only; use host:port or "
            "http://host:port");
    }
    while (!proxy.empty() && proxy.back() == '/') {
        proxy.pop_back();
    }

    if (proxy.find('[') == std::string::npos) {
        const auto last_colon = proxy.rfind(':');
        if (last_colon != std::string::npos) {
            const auto second_last_colon =
                proxy.rfind(':', last_colon == 0 ? std::string::npos : last_colon - 1);
            if (second_last_colon != std::string::npos && last_colon > second_last_colon + 1 &&
                second_last_colon > 0) {
                const auto host_port = proxy.substr(0, second_last_colon);
                const auto username =
                    proxy.substr(second_last_colon + 1, last_colon - second_last_colon - 1);
                const auto password = proxy.substr(last_colon + 1);
                if (!host_port.empty() && !username.empty() && !password.empty()) {
                    proxy = host_port;
                }
            }
        }
    }

    const auto invalid_character = std::find_if(
        proxy.begin(), proxy.end(), [](char value) {
            const auto byte = static_cast<unsigned char>(value);
            return std::isspace(byte) != 0 || value == '/' || value == '?' ||
                   value == '#' || value == '@' || value == ';';
        });
    if (proxy.empty() || invalid_character != proxy.end()) {
        throw std::invalid_argument(
            "invalid --proxy; expected host:port or http://host:port");
    }

    std::size_t port_separator = std::string::npos;
    if (proxy.front() == '[') {
        const auto closing_bracket = proxy.find(']');
        if (closing_bracket <= 1 || closing_bracket + 1 >= proxy.size() ||
            proxy[closing_bracket + 1] != ':') {
            throw std::invalid_argument(
                "invalid IPv6 --proxy; expected [address]:port");
        }
        port_separator = closing_bracket + 1;
    } else {
        port_separator = proxy.rfind(':');
        if (port_separator == std::string::npos || port_separator == 0 ||
            proxy.find(':') != port_separator) {
            throw std::invalid_argument(
                "invalid --proxy; expected host:port or http://host:port");
        }
    }

    const auto port_text = proxy.substr(port_separator + 1);
    unsigned int port = 0;
    if (port_text.empty()) {
        throw std::invalid_argument("proxy port is missing");
    }
    for (const char value : port_text) {
        if (value < '0' || value > '9') {
            throw std::invalid_argument("proxy port must be numeric");
        }
        const auto digit = static_cast<unsigned int>(value - '0');
        if (port > 6'553U || (port == 6'553U && digit > 5U)) {
            throw std::invalid_argument("proxy port must be between 1 and 65535");
        }
        port = port * 10U + digit;
    }
    if (port == 0) {
        throw std::invalid_argument("proxy port must be between 1 and 65535");
    }
    return proxy;
}

}  // namespace sui_pow
