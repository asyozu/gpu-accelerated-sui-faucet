#include "http.hpp"
#include "json_lite.hpp"
#include "pow.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kChallengeApi =
    "https://faucet.testnet.sui.io/v2/request_challenge";
constexpr std::string_view kFaucetApi =
    "https://faucet.testnet.sui.io/v2/faucet_web_gas";

class Arguments {
public:
    Arguments(int argc, char** argv) {
        for (int i = 0; i < argc; ++i) {
            values_.emplace_back(argv[i]);
        }
    }

    bool flag(std::string_view name) const {
        return std::find(values_.begin(), values_.end(), name) != values_.end();
    }

    std::optional<std::string> optional(std::string_view name) const {
        const auto found = std::find(values_.begin(), values_.end(), name);
        if (found == values_.end()) {
            return std::nullopt;
        }
        const auto next = std::next(found);
        if (next == values_.end() || next->rfind("--", 0) == 0) {
            throw std::invalid_argument("missing value for " + std::string(name));
        }
        return *next;
    }

    std::optional<std::string> optional_value_or_empty(std::string_view name) const {
        const auto found = std::find(values_.begin(), values_.end(), name);
        if (found == values_.end()) {
            return std::nullopt;
        }
        const auto next = std::next(found);
        if (next == values_.end() || next->rfind("--", 0) == 0) {
            return std::string{};
        }
        return *next;
    }

    std::string required(std::string_view name) const {
        const auto value = optional(name);
        if (!value) {
            throw std::invalid_argument("missing required option " + std::string(name));
        }
        return *value;
    }

    template <typename T>
    T number(std::string_view name, T fallback) const {
        const auto value = optional(name);
        if (!value) {
            return fallback;
        }
        T parsed{};
        const auto result =
            std::from_chars(value->data(), value->data() + value->size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) {
            throw std::invalid_argument("invalid number for " + std::string(name));
        }
        return parsed;
    }

private:
    std::vector<std::string> values_;
};

sui_pow::MineOptions parse_options(const Arguments& args) {
    sui_pow::MineOptions options;
    options.batch_size = args.number<std::uint64_t>("--batch-size", 16'777'216);
    options.cuda_device = args.number<int>("--device", 0);
    options.quiet = args.flag("--quiet");
    return options;
}

namespace {
std::pair<std::string, std::pair<std::string, std::string>> parse_proxy_value(
    const std::string& value) {
    if (value.empty()) {
        return {"", {"", ""}};
    }

    std::string proxy = value;
    if (proxy.rfind("http://", 0) == 0) {
        proxy.erase(0, 7);
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
                    return {host_port, {username, password}};
                }
            }
        }
    }

    return {proxy, {"", ""}};
}

std::vector<std::string> read_proxies(const Arguments& args) {
    if (!args.flag("--proxy")) {
        return {};
    }

    const auto proxy_value = args.optional_value_or_empty("--proxy");
    if (proxy_value && !proxy_value->empty()) {
        const auto proxy_path = std::filesystem::path(*proxy_value);
        if (std::filesystem::exists(proxy_path)) {
            std::ifstream in(proxy_path);
            if (!in) {
                throw std::runtime_error("failed to read proxy file: " + *proxy_value);
            }
            std::vector<std::string> proxies;
            std::string line;
            while (std::getline(in, line)) {
                const auto start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) {
                    continue;
                }
                const auto end = line.find_last_not_of(" \t\r\n");
                const auto trimmed = line.substr(start, end - start + 1);
                if (!trimmed.empty()) {
                    proxies.push_back(trimmed);
                }
            }
            return proxies;
        }
        return {*proxy_value};
    }

    const auto proxy_file = std::filesystem::current_path() / "proxies.txt";
    std::error_code ec;
    if (!std::filesystem::exists(proxy_file, ec)) {
        std::ofstream out(proxy_file, std::ios::out | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to create proxies.txt");
        }
    }

    std::ifstream in(proxy_file);
    if (!in) {
        throw std::runtime_error("failed to read proxies.txt");
    }

    std::vector<std::string> proxies;
    std::string line;
    while (std::getline(in, line)) {
        const auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;
        }
        const auto end = line.find_last_not_of(" \t\r\n");
        const auto trimmed = line.substr(start, end - start + 1);
        if (!trimmed.empty()) {
            proxies.push_back(trimmed);
        }
    }
    return proxies;
}
}  // namespace

sui_pow::HttpOptions parse_http_options(const Arguments& args) {
    sui_pow::HttpOptions options;
    if (!args.flag("--proxy")) {
        return options;
    }

    const auto proxy_value = args.optional_value_or_empty("--proxy");
    if (!proxy_value || proxy_value->empty()) {
        return options;
    }

    const auto parsed_proxy = parse_proxy_value(*proxy_value);
    options.proxy = sui_pow::normalize_http_proxy(parsed_proxy.first);
    options.proxy_username = parsed_proxy.second.first;
    options.proxy_password = parsed_proxy.second.second;
    if (options.proxy.empty() &&
        (!options.proxy_username.empty() || !options.proxy_password.empty())) {
        throw std::invalid_argument(
            "proxy credentials require --proxy host:port[:user:pass]");
    }
    return options;
}

sui_pow::MineRequest parse_mine_request(const Arguments& args) {
    sui_pow::MineRequest request;
    request.ts = args.number<std::uint64_t>("--ts", 0);
    if (!args.optional("--ts")) {
        throw std::invalid_argument("missing required option --ts");
    }
    const auto difficulty = args.number<unsigned int>("--difficulty", 0);
    if (!args.optional("--difficulty") || difficulty > 128) {
        throw std::invalid_argument("--difficulty must be between 3 and 128");
    }
    request.difficulty = static_cast<std::uint16_t>(difficulty);
    request.challenge = args.required("--challenge");
    request.start_nonce = args.number<std::uint64_t>("--start", 0);
    if (request.start_nonce >= sui_pow::kNonceSpace) {
        throw std::invalid_argument("--start must fit in the u32 nonce space");
    }
    const auto available = sui_pow::kNonceSpace - request.start_nonce;
    const auto count = args.number<std::uint64_t>("--count", available);
    request.end_nonce = request.start_nonce + std::min(count, available);
    return request;
}

void print_solution(const sui_pow::Solution& solution) {
    const double rate = solution.elapsed_seconds > 0
                            ? solution.attempts / solution.elapsed_seconds / 1'000'000.0
                            : 0.0;
    std::cout << "{\n"
              << "  \"nonce\": " << solution.nonce << ",\n"
              << "  \"hash\": \"" << solution.hash << "\",\n"
              << "  \"attempts\": " << solution.attempts << ",\n"
              << "  \"elapsed_seconds\": " << solution.elapsed_seconds << ",\n"
              << "  \"mh_s\": " << rate << "\n"
              << "}\n";
}

void run_mine(const Arguments& args) {
    const auto request = parse_mine_request(args);
    const auto options = parse_options(args);
    print_solution(sui_pow::mine(request, options));
}

void run_faucet(const Arguments& args) {
    const auto recipient = args.required("--recipient");
    const auto options = parse_options(args);
    const auto tries = args.number<std::uint64_t>("--try", 1);
    if (tries == 0) {
        throw std::invalid_argument("--try must be at least 1");
    }

    auto run_once = [&](const sui_pow::HttpOptions& http_options,
                        const std::string& label) {
        const auto response = sui_pow::http_post_json(
            std::string(kChallengeApi), "{}", {}, http_options);
        const auto challenge_object =
            sui_pow::json_lite::find_field(response, "challenge");
        const auto signature = sui_pow::json_lite::find_field(response, "signature");

        sui_pow::MineRequest request;
        request.ts = sui_pow::json_lite::parse_u64(
            sui_pow::json_lite::find_field(challenge_object, "ts"));
        const auto difficulty = sui_pow::json_lite::parse_u64(
            sui_pow::json_lite::find_field(challenge_object, "difficulty"));
        if (difficulty > 128) {
            throw std::runtime_error("challenge difficulty is outside the supported range");
        }
        request.difficulty = static_cast<std::uint16_t>(difficulty);
        request.challenge = sui_pow::json_lite::decode_string(
            sui_pow::json_lite::find_field(challenge_object, "challenge"));

        std::cerr << label << " challenge: " << request.challenge
                  << " (ts: " << request.ts << ", difficulty: " << request.difficulty
                  << ")\n";
        const auto solution = sui_pow::mine(request, options);

        const std::string payload =
            "{\"FixedAmountRequest\":{\"recipient\":" +
            sui_pow::json_lite::escape_string(recipient) +
            ",\"solution\":{\"nonce\":" + std::to_string(solution.nonce) +
            ",\"hash\":" + sui_pow::json_lite::escape_string(solution.hash) +
            ",\"challenge\":" + std::string(challenge_object) +
            ",\"signature\":" + std::string(signature) + "}}}";

        const auto faucet_response = sui_pow::http_post_json(
            std::string(kFaucetApi),
            payload,
            "Origin: https://faucet.sui.io\r\nReferer: https://faucet.sui.io/\r\n",
            http_options);
        std::cout << label << "\n" << faucet_response << '\n';
    };

    if (!args.flag("--proxy")) {
        for (std::uint64_t attempt = 0; attempt < tries; ++attempt) {
            run_once(parse_http_options(args),
                     "direct attempt " + std::to_string(attempt + 1) + "/" +
                         std::to_string(tries));
        }
        return;
    }

    const auto proxied_value = args.optional_value_or_empty("--proxy");
    if (proxied_value && !proxied_value->empty()) {
        const auto parsed_proxy = parse_proxy_value(*proxied_value);
        sui_pow::HttpOptions http_options;
        http_options.proxy = sui_pow::normalize_http_proxy(parsed_proxy.first);
        http_options.proxy_username = parsed_proxy.second.first;
        http_options.proxy_password = parsed_proxy.second.second;

        for (std::uint64_t attempt = 0; attempt < tries; ++attempt) {
            run_once(http_options,
                     "proxy " + *proxied_value + " attempt " +
                         std::to_string(attempt + 1) + "/" + std::to_string(tries));
        }
        return;
    }

    const auto proxy_values = read_proxies(args);
    if (proxy_values.empty()) {
        std::cout << "No proxies found in proxies.txt; created a new empty file.\n";
        return;
    }

    for (const auto& proxy_value : proxy_values) {
        const auto parsed_proxy = parse_proxy_value(proxy_value);
        sui_pow::HttpOptions http_options;
        http_options.proxy = sui_pow::normalize_http_proxy(parsed_proxy.first);
        http_options.proxy_username = parsed_proxy.second.first;
        http_options.proxy_password = parsed_proxy.second.second;

        for (std::uint64_t attempt = 0; attempt < tries; ++attempt) {
            run_once(http_options,
                     "proxy " + proxy_value + " attempt " +
                         std::to_string(attempt + 1) + "/" + std::to_string(tries));
        }
    }
}

void self_test(const Arguments& args) {
    const std::string expected_solution =
        "000f018401dcc5a1bad32216583c488c75da95f160a5a730d252e8a87d9c862fd"
        "6e2b5580df624d3a270dbdbb65fc36caf9fc6383ac8f40c7bbd6c3449fac158";

    sui_pow::MineRequest request;
    request.ts = 1'700'000'000'000ULL;
    request.difficulty = 3;
    request.challenge = "abc";
    request.start_nonce = 386;
    request.end_nonce = 387;

    sui_pow::MineOptions options;
    options.cuda_device = args.number<int>("--device", 0);
    options.batch_size = 1;
    options.quiet = true;
    const auto result = sui_pow::mine(request, options);
    if (result.nonce != 386 || result.hash != expected_solution) {
        throw std::runtime_error("CUDA/WASM parity vector failed");
    }

    const std::string sample =
        R"({"challenge":{"challenge":"abc","ts":1700000000000,"difficulty":3},"signature":"sig"})";
    const auto object = sui_pow::json_lite::find_field(sample, "challenge");
    if (sui_pow::json_lite::decode_string(
            sui_pow::json_lite::find_field(object, "challenge")) != "abc") {
        throw std::runtime_error("JSON parser self-test failed");
    }
    if (sui_pow::normalize_http_proxy("http://127.0.0.1:8080/") !=
        "127.0.0.1:8080") {
        throw std::runtime_error("HTTP proxy parser self-test failed");
    }
    if (sui_pow::normalize_http_proxy("127.0.0.1:8080:USER:PASS") !=
        "127.0.0.1:8080") {
        throw std::runtime_error("HTTP proxy credential parser self-test failed");
    }
    try {
        static_cast<void>(
            sui_pow::normalize_http_proxy("socks5://127.0.0.1:1080"));
        throw std::runtime_error("HTTP proxy parser accepted SOCKS unexpectedly");
    } catch (const std::invalid_argument&) {
    }
    std::cout << "self-test passed: CUDA kernel matches wasm_pow_bg.wasm\n";
}

void print_help() {
    std::cout
        << "sui-pow GPU-only C++/CUDA\n\n"
        << "Commands:\n"
        << "  sui-pow mine --ts N --difficulty N --challenge TEXT [options]\n"
        << "  sui-pow faucet --recipient 0xADDRESS [options]\n"
        << "  sui-pow self-test\n\n"
        << "Options:\n"
        << "  --device N                    CUDA device, default: 0\n"
        << "  --batch-size N                CUDA launch size, default: 16777216\n"
        << "  --start N --count N           restrict nonce range\n"
        << "  --proxy HOST:PORT[:USER:PASS] optional explicit proxy override\n"
        << "  --try N                       repeat direct attempts N times; with --proxy, repeat each proxy N times\n"
        << "  --quiet                       hide progress\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_help();
            return 0;
        }
        const std::string command = argv[1];
        const Arguments args(argc - 2, argv + 2);
        if (command == "mine") {
            run_mine(args);
        } else if (command == "faucet") {
            run_faucet(args);
        } else if (command == "self-test") {
            self_test(args);
        } else if (command == "help" || command == "--help" || command == "-h") {
            print_help();
        } else {
            throw std::invalid_argument("unknown command: " + command);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
