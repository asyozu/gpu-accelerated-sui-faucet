#include "http.hpp"
#include "json_lite.hpp"
#include "pow.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
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

constexpr std::string_view kFaucetUrl = "https://faucet.testnet.sui.io";

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
    options.progress_interval = args.number<std::uint64_t>("--progress-interval", 100);
    options.batch_size = args.number<std::uint32_t>("--batch-size", 128);
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
    request.chain_id = args.required("--chain-id");
    request.checkpoint_seq = args.required("--checkpoint-seq");
    request.checkpoint_digest = args.required("--checkpoint-digest");
    request.random_bytes = args.required("--random-bytes");
    request.faucet_address = args.required("--faucet-address");
    request.recipient = args.required("--recipient");
    request.threshold = args.number<std::uint64_t>("--threshold", 0);
    request.start_nonce = args.number<std::uint64_t>("--start", 0);
    request.nonce_count = args.number<std::uint64_t>(
        "--count", std::numeric_limits<std::uint64_t>::max());
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

std::string json_text_field(std::string_view object, std::string_view key) {
    return sui_pow::json_lite::decode_string(
        sui_pow::json_lite::find_field(object, key));
}

std::string normalize_address(std::string address) {
    if (address.rfind("0x", 0) != 0 || address.size() < 3 || address.size() > 66) {
        throw std::invalid_argument("recipient must be a Sui address with 1 to 64 hex digits");
    }
    address.erase(0, 2);
    for (auto& digit : address) {
        if (!std::isxdigit(static_cast<unsigned char>(digit))) {
            throw std::invalid_argument("recipient contains a non-hexadecimal character");
        }
        digit = static_cast<char>(std::tolower(static_cast<unsigned char>(digit)));
    }
    return "0x" + std::string(64 - address.size(), '0') + address;
}

void validate_v3_challenge(std::string_view challenge, const std::string& recipient) {
    const auto require_number = [&](std::string_view key, std::uint64_t expected) {
        if (sui_pow::json_lite::parse_u64(
                sui_pow::json_lite::find_field(challenge, key)) != expected) {
            throw std::runtime_error("unsupported v3 challenge parameter: " +
                                     std::string(key));
        }
    };
    const auto require_text = [&](std::string_view key, std::string_view expected) {
        if (json_text_field(challenge, key) != expected) {
            throw std::runtime_error("unsupported v3 challenge parameter: " +
                                     std::string(key));
        }
    };
    require_number("version", 1);
    require_text("domain", "sui-faucet-pow/1");
    require_text("algorithm", "argon2d");
    require_number("argon2Version", 19);
    require_number("memorySize", 8192);
    require_number("iterations", 1);
    require_number("parallelism", 1);
    require_number("hashLength", 32);
    require_text("salt", "sui-faucet-pow-1");
    require_text("recipient", recipient);
}

void run_faucet(const Arguments& args) {
    const auto recipient = normalize_address(args.required("--recipient"));
    const auto options = parse_options(args);
    const auto tries = args.number<std::uint64_t>("--try", 1);
    if (tries == 0) {
        throw std::invalid_argument("--try must be at least 1");
    }
    auto run_once = [&](const sui_pow::HttpOptions& http_options,
                        const std::string& label) {
        const auto challenge_response = sui_pow::http_get(
            std::string(kFaucetUrl) + "/v3/challenge?recipient=" + recipient,
            http_options);
        validate_v3_challenge(challenge_response, recipient);

        sui_pow::MineRequest request;
        request.chain_id = json_text_field(challenge_response, "chainId");
        request.checkpoint_seq = json_text_field(challenge_response, "checkpointSeq");
        request.checkpoint_digest = json_text_field(challenge_response, "checkpointDigest");
        request.random_bytes = json_text_field(challenge_response, "randomBytes");
        request.faucet_address = json_text_field(challenge_response, "faucetAddress");
        request.recipient = recipient;
        request.threshold = sui_pow::json_lite::parse_u64(
            sui_pow::json_lite::find_field(challenge_response, "threshold"));
        static_cast<void>(sui_pow::json_lite::parse_u64(request.checkpoint_seq));

        std::cerr << label << " checkpoint " << request.checkpoint_seq
                  << " | threshold " << request.threshold << '\n';
        const auto solution = sui_pow::mine(request, options);
        std::cerr << "Proof found: nonce " << solution.nonce << " after "
              << solution.attempts << " hashes in " << std::fixed
              << std::setprecision(2) << solution.elapsed_seconds
              << " s; submitting to faucet...\n";
        const std::string payload =
            "{\"recipient\":" + sui_pow::json_lite::escape_string(recipient) +
            ",\"checkpointSeq\":" +
            sui_pow::json_lite::escape_string(request.checkpoint_seq) +
            ",\"nonce\":" + sui_pow::json_lite::escape_string(
                                     std::to_string(solution.nonce)) +
            ",\"hashHex\":" + sui_pow::json_lite::escape_string(solution.hash) + "}";
        const auto submit_started = std::chrono::steady_clock::now();
        const auto faucet_response = sui_pow::http_post_json(
            std::string(kFaucetUrl) + "/v3/gas", payload, {}, http_options);
        const auto submit_elapsed = std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() - submit_started)
                                        .count();
        std::cerr << "Faucet responded in " << std::fixed << std::setprecision(2)
                  << submit_elapsed << " s.\n";
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

void self_test() {
    sui_pow::MineRequest request;
    request.chain_id = "8wWZfv1HjQxjB5ncmC9CQqqRqUMBiEZXnc5FhS9zRe1v";
    request.checkpoint_seq = "1713342";
    request.checkpoint_digest = "GzBdc9tbUe3s2PK1iVJcyQBj5TvdX7bJiAHWbhBDYiYb";
    request.random_bytes =
        "mJ4ZqDXQasy17GTiGfp0/OYAGCR+7reLM5+Gb39YtoAIaPuh1wj+z5J7po+eSze3";
    request.faucet_address =
        "0x949cd75a2485cc18b8258634ffd9b849a687e2e2a0b4c2444a21f007fc79adc4";
    request.recipient =
        "0x1111111111111111111111111111111111111111111111111111111111111111";
    request.threshold = 72'057'594'037'927'936ULL;
    request.start_nonce = 201;
    request.nonce_count = 1;

    sui_pow::MineOptions options;
    options.quiet = true;
    options.batch_size = 1;
    const auto result = sui_pow::mine(request, options);
    if (result.nonce != 201 ||
        result.hash != "0008faf113e0b071f5f9d5d6e1dc9321890ecb7e81059f07dedbd3dec5e902bb") {
        throw std::runtime_error("Argon2d v3 conformance vector failed");
    }

    request.threshold = 10'780'597'955'806'233'528ULL;
    request.start_nonce = 42;
    request.nonce_count = 1;
    options.batch_size = 64;
    const auto second_vector = sui_pow::mine(request, options);
    if (second_vector.nonce != 42 ||
        second_vector.hash != "959c60b9062203b7739fb94aa3577a6173fa36f0cec60e55ce714402b36ffa28") {
        throw std::runtime_error("Argon2d GPU batch vector failed");
    }

    request.random_bytes.clear();
    request.threshold = std::numeric_limits<std::uint64_t>::max();
    request.start_nonce = 0;
    options.batch_size = 1;
    const auto empty_random_result = sui_pow::mine(request, options);
    if (empty_random_result.hash !=
        "d8430e5471f4fba3c3c67a253f15dc72588d87c364117e1f138e26387050ec60") {
        throw std::runtime_error("Argon2d empty-randomness vector failed");
    }

    const std::string sample = R"({"challenge":{"chainId":"abc","checkpointSeq":"42"}})";
    const auto object = sui_pow::json_lite::find_field(sample, "challenge");
    if (sui_pow::json_lite::decode_string(
            sui_pow::json_lite::find_field(object, "chainId")) != "abc") {
        throw std::runtime_error("JSON parser self-test failed");
    }
    if (sui_pow::json_lite::parse_u64(
            sui_pow::json_lite::find_field(object, "checkpointSeq")) != 42) {
        throw std::runtime_error("JSON integer parser self-test failed");
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
    std::cout << "self-test passed: Argon2d v3 conformance vectors\n";
}

void print_help() {
    std::cout
        << "sui-pow C++ faucet PoW client\n\n"
        << "Commands:\n"
        << "  sui-pow mine --chain-id ID --checkpoint-seq N --checkpoint-digest D\n"
        << "               --random-bytes B64 --faucet-address 0xADDRESS\n"
        << "               --recipient 0xADDRESS --threshold N [options]\n"
        << "  sui-pow faucet --recipient 0xADDRESS [options]\n"
        << "  sui-pow self-test\n\n"
        << "Options:\n"
        << "  --start N --count N           restrict nonce range\n"
        << "  --progress-interval N         progress update interval, default: 100 hashes\n"
        << "  --batch-size N                GPU jobs per batch, power of two, default: 128\n"
        << "  --device N                    CUDA device index, default: 0\n"
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
            self_test();
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
