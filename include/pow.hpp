#pragma once

#include <cstdint>
#include <string>

namespace sui_pow {

constexpr std::uint64_t kNonceSpace = std::uint64_t{1} << 32;

struct MineRequest {
    std::uint64_t ts{};
    std::uint16_t difficulty{};
    std::string challenge;
    std::uint64_t start_nonce{};
    std::uint64_t end_nonce{kNonceSpace};
};

struct MineOptions {
    std::uint64_t batch_size{16'777'216};
    int cuda_device{};
    bool quiet{};
};

struct Solution {
    std::uint32_t nonce{};
    std::string hash;
    std::uint64_t attempts{};
    double elapsed_seconds{};
};

Solution mine(const MineRequest& request, const MineOptions& options);

}  // namespace sui_pow
