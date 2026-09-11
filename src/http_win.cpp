#include "http.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace sui_pow {
namespace {

class InternetHandle {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() {
        if (value_ != nullptr) {
            WinHttpCloseHandle(value_);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    HINTERNET get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

private:
    HINTERNET value_;
};

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        throw std::runtime_error("invalid UTF-8 input");
    }
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        output.data(),
                        length);
    return output;
}

[[noreturn]] void throw_windows(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed with Windows error " +
                             std::to_string(GetLastError()));
}

}  // namespace

static std::string http_request(const std::string& url,
                                const wchar_t* method,
                                const std::string& body,
                                const std::string& extra_headers,
                                const HttpOptions& options) {
    if (options.proxy.empty() &&
        (!options.proxy_username.empty() || !options.proxy_password.empty())) {
        throw std::invalid_argument(
            "proxy credentials require --proxy host:port[:user:pass]");
    }
    const auto wide_url = utf8_to_wide(url);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
        throw_windows("WinHttpCrackUrl");
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    const auto wide_proxy = utf8_to_wide(normalize_http_proxy(options.proxy));
    const auto access_type = wide_proxy.empty()
                                 ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
                                 : WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    InternetHandle session(WinHttpOpen(
        L"sui-pow-cpp/1.0",
        access_type,
        wide_proxy.empty() ? WINHTTP_NO_PROXY_NAME : wide_proxy.c_str(),
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session) {
        throw_windows("WinHttpOpen");
    }
    WinHttpSetTimeouts(session.get(), 15'000, 15'000, 30'000, 30'000);

    InternetHandle connection(WinHttpConnect(
        session.get(), host.c_str(), components.nPort, 0));
    if (!connection) {
        throw_windows("WinHttpConnect");
    }
    const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.get(),
                                              method,
                                              path.c_str(),
                                              nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              flags));
    if (!request) {
        throw_windows("WinHttpOpenRequest");
    }

    std::wstring proxy_username;
    std::wstring proxy_password;
    if (!options.proxy_username.empty()) {
        proxy_username = utf8_to_wide(options.proxy_username);
        proxy_password = utf8_to_wide(options.proxy_password);
        if (!WinHttpSetCredentials(request.get(),
                                   WINHTTP_AUTH_TARGET_PROXY,
                                   WINHTTP_AUTH_SCHEME_BASIC,
                                   proxy_username.c_str(),
                                   proxy_password.c_str(),
                                   nullptr)) {
            throw_windows("WinHttpSetCredentials(proxy)");
        }
    }

    std::string headers = "Content-Type: application/json\r\n" + extra_headers;
    const auto wide_headers = utf8_to_wide(headers);
    if (!WinHttpSendRequest(request.get(),
                            wide_headers.c_str(),
                            static_cast<DWORD>(wide_headers.size()),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA
                                         : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()),
                            0)) {
        throw_windows("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw_windows("WinHttpReceiveResponse");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status,
                             &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw_windows("WinHttpQueryHeaders");
    }

    std::string response;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            throw_windows("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            break;
        }
        const auto offset = response.size();
        response.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(
                request.get(), response.data() + offset, available, &received)) {
            throw_windows("WinHttpReadData");
        }
        response.resize(offset + received);
    }

    if (status < 200 || status >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(status) + ": " + response);
    }
    return response;
}

std::string http_post_json(const std::string& url,
                           const std::string& body,
                           const std::string& extra_headers,
                           const HttpOptions& options) {
    return http_request(url, L"POST", body, extra_headers, options);
}

std::string http_get(const std::string& url, const HttpOptions& options) {
    return http_request(url, L"GET", {}, {}, options);
}

}  // namespace sui_pow
