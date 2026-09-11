#include "pow.hpp"

#include <argon2-cuda/cudaexception.h>
#include <argon2-cuda/kernels.h>
#include <argon2-gpu-common/argon2-common.h>
#include <argon2-gpu-common/argon2params.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace sui_pow {
namespace {

constexpr char kSalt[] = "sui-faucet-pow-1";

std::string digest_hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output(64, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        output[index * 2] = digits[digest[index] >> 4];
        output[index * 2 + 1] = digits[digest[index] & 0x0f];
    }
    return output;
}

std::uint64_t leading_value(const std::array<std::uint8_t, 32>& digest) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value = (value << 8) | digest[index];
    }
    return value;
}

}  // namespace

std::string build_preimage(const MineRequest& request, std::uint64_t nonce) {
    return "sui-faucet-pow/1\n" + request.chain_id + "\n" +
           request.checkpoint_seq + "\n" + request.checkpoint_digest + "\n" +
           request.random_bytes + "\n" + request.faucet_address + "\n" +
           request.recipient + "\n" + std::to_string(nonce);
}

Solution mine(const MineRequest& request, const MineOptions& options) {
    if (request.threshold == 0 || options.progress_interval == 0 ||
        request.nonce_count == 0 || options.batch_size == 0 ||
        (options.batch_size & (options.batch_size - 1)) != 0) {
        throw std::invalid_argument(
            "threshold, progress interval, nonce count, and power-of-two batch size must be positive");
    }

    argon2::cuda::CudaException::check(cudaSetDevice(options.cuda_device));
    cudaDeviceProp device{};
    argon2::cuda::CudaException::check(
        cudaGetDeviceProperties(&device, options.cuda_device));
    if (device.major < 6) {
        throw std::runtime_error("CUDA compute capability 6.0 or newer is required");
    }

    constexpr std::uint32_t kMemoryKiB = 8192;
    constexpr std::uint32_t kIterations = 1;
    constexpr std::uint32_t kLanes = 1;
    constexpr std::size_t kHashBytes = 32;
    constexpr std::size_t kBlockBytes = argon2::ARGON2_BLOCK_SIZE;
    const auto salt = std::string("sui-faucet-pow-1");
    argon2::Argon2Params params(kHashBytes,
                                salt.data(),
                                salt.size(),
                                nullptr,
                                0,
                                nullptr,
                                0,
                                kIterations,
                                kMemoryKiB,
                                kLanes);
    const auto segment_blocks = params.getSegmentBlocks();
    argon2::cuda::KernelRunner runner(argon2::ARGON2_D,
                                      argon2::ARGON2_VERSION_13,
                                      kIterations,
                                      kLanes,
                                      segment_blocks,
                                      options.batch_size,
                                      false,
                                      false);
    const auto jobs_per_block = std::min<std::uint32_t>(16, options.batch_size);
    const auto initial_memory_bytes = kLanes * 2 * kBlockBytes;
    std::vector<std::uint8_t> initial_memory(initial_memory_bytes);
    std::vector<std::uint8_t> final_memory(kLanes * kBlockBytes);
    std::array<std::uint8_t, kHashBytes> digest{};

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t attempts = 0;
    while (attempts < request.nonce_count) {
        const auto batch_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            options.batch_size, request.nonce_count - attempts));
        std::vector<std::uint64_t> nonces(batch_count);
        for (std::uint32_t index = 0; index < batch_count; ++index) {
            const auto offset = attempts + index;
            if (offset > std::numeric_limits<std::uint64_t>::max() - request.start_nonce) {
                throw std::runtime_error("nonce range exhausted");
            }
            nonces[index] = request.start_nonce + offset;
            const auto preimage = build_preimage(request, nonces[index]);
            params.fillFirstBlocks(initial_memory.data(),
                                   preimage.data(),
                                   preimage.size(),
                                   argon2::ARGON2_D,
                                   argon2::ARGON2_VERSION_13);
            runner.writeInputMemory(index, initial_memory.data());
        }
        for (std::uint32_t index = batch_count; index < options.batch_size; ++index) {
            runner.writeInputMemory(index, initial_memory.data());
        }

        runner.run(1, jobs_per_block);
        runner.finish();
        for (std::uint32_t index = 0; index < batch_count; ++index) {
            runner.readOutputMemory(index, final_memory.data());
            params.finalize(digest.data(), final_memory.data());
            ++attempts;
            if (leading_value(digest) < request.threshold) {
                if (!options.quiet) {
                    std::cerr << '\n';
                }
                const auto elapsed = std::chrono::duration<double>(
                                         std::chrono::steady_clock::now() - started)
                                         .count();
                return {nonces[index], digest_hex(digest), attempts, elapsed};
            }
        }

        if (!options.quiet && attempts % options.progress_interval < batch_count) {
            const auto elapsed = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            const auto rate = elapsed > 0 ? attempts / elapsed : 0.0;
            std::cerr << '\r' << device.name << " | " << attempts << " tries | "
                      << std::fixed << std::setprecision(2) << rate << " H/s"
                      << std::flush;
        }
        if (request.start_nonce > std::numeric_limits<std::uint64_t>::max() - attempts) {
            break;
        }
    }

    if (!options.quiet) {
        std::cerr << '\n';
    }
    throw std::runtime_error("nonce range exhausted without a solution");
}

}  // namespace sui_pow