#pragma once

#include <cstdint>
#include <string>

namespace sui_pow {

struct MineRequest {
    std::string chain_id;
    std::string checkpoint_seq;
    std::string checkpoint_digest;
    std::string random_bytes;
    std::string faucet_address;
    std::string recipient;
    std::uint64_t threshold{};
    std::uint64_t start_nonce{};
    std::uint64_t nonce_count{UINT64_MAX};
};

struct MineOptions {
    std::uint64_t progress_interval{100};
    std::uint32_t batch_size{128};
    int cuda_device{};
    bool quiet{};
};

struct Solution {
    std::uint64_t nonce{};
    std::string hash;
    std::uint64_t attempts{};
    double elapsed_seconds{};
};

std::string build_preimage(const MineRequest& request, std::uint64_t nonce);
Solution mine(const MineRequest& request, const MineOptions& options);

}  // namespace sui_pow
