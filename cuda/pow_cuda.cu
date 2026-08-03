#include "pow.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace sui_pow {
namespace {

constexpr int kBlockThreads = 128;
constexpr int kMaxPrefix = 32;
constexpr int kMaxSuffix = 320;
constexpr int kMaxMessage = kMaxPrefix + 10 + kMaxSuffix;

__constant__ std::uint8_t c_prefix[kMaxPrefix];
__constant__ std::uint8_t c_suffix[kMaxSuffix];
__constant__ int c_prefix_length;
__constant__ int c_suffix_length;

__constant__ std::uint8_t c_sigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
};

__device__ __forceinline__ std::uint64_t rotate_right(std::uint64_t value,
                                                       int shift) {
    return (value >> shift) | (value << (64 - shift));
}

__device__ __forceinline__ std::uint64_t load64_le(const std::uint8_t* input) {
    return static_cast<std::uint64_t>(input[0]) |
           (static_cast<std::uint64_t>(input[1]) << 8) |
           (static_cast<std::uint64_t>(input[2]) << 16) |
           (static_cast<std::uint64_t>(input[3]) << 24) |
           (static_cast<std::uint64_t>(input[4]) << 32) |
           (static_cast<std::uint64_t>(input[5]) << 40) |
           (static_cast<std::uint64_t>(input[6]) << 48) |
           (static_cast<std::uint64_t>(input[7]) << 56);
}

__device__ __forceinline__ void store64_le(std::uint8_t* output,
                                            std::uint64_t value) {
#pragma unroll
    for (int i = 0; i < 8; ++i) {
        output[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

__device__ __forceinline__ void mix(std::uint64_t& a,
                                    std::uint64_t& b,
                                    std::uint64_t& c,
                                    std::uint64_t& d,
                                    std::uint64_t x,
                                    std::uint64_t y) {
    a = a + b + x;
    d = rotate_right(d ^ a, 32);
    c += d;
    b = rotate_right(b ^ c, 24);
    a = a + b + y;
    d = rotate_right(d ^ a, 16);
    c += d;
    b = rotate_right(b ^ c, 63);
}

// Proven GTX 1660 SUPER path: the real round loop lets NVCC use 168
// registers without spills and reached 333.04 MH/s with a 16M batch.
__device__ __forceinline__ void compress(std::uint64_t state[8],
                                         const std::uint8_t block[128],
                                         std::uint64_t bytes,
                                         bool last) {
    constexpr std::uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL,
        0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL,
        0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,
        0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL,
        0x5be0cd19137e2179ULL,
    };
    std::uint64_t message[16];
    std::uint64_t work[16];
#pragma unroll
    for (int i = 0; i < 16; ++i) {
        message[i] = load64_le(block + i * 8);
    }
#pragma unroll
    for (int i = 0; i < 8; ++i) {
        work[i] = state[i];
        work[i + 8] = iv[i];
    }
    work[12] ^= bytes;
    if (last) {
        work[14] = ~work[14];
    }

#pragma unroll 1
    for (int round = 0; round < 12; ++round) {
        const auto* s = c_sigma[round];
        mix(work[0], work[4], work[8], work[12], message[s[0]], message[s[1]]);
        mix(work[1], work[5], work[9], work[13], message[s[2]], message[s[3]]);
        mix(work[2], work[6], work[10], work[14], message[s[4]], message[s[5]]);
        mix(work[3], work[7], work[11], work[15], message[s[6]], message[s[7]]);
        mix(work[0], work[5], work[10], work[15], message[s[8]], message[s[9]]);
        mix(work[1], work[6], work[11], work[12], message[s[10]], message[s[11]]);
        mix(work[2], work[7], work[8], work[13], message[s[12]], message[s[13]]);
        mix(work[3], work[4], work[9], work[14], message[s[14]], message[s[15]]);
    }
#pragma unroll
    for (int i = 0; i < 8; ++i) {
        state[i] ^= work[i] ^ work[i + 8];
    }
}

__device__ __forceinline__ void blake2b_512_device(
    const std::uint8_t* input,
    int length,
    std::uint8_t output[64]) {
    std::uint64_t state[8] = {
        0x6a09e667f3bcc908ULL ^ 0x01010040ULL,
        0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL,
        0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,
        0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL,
        0x5be0cd19137e2179ULL,
    };
    std::uint64_t processed = 0;
    while (length > 128) {
        processed += 128;
        compress(state, input, processed, false);
        input += 128;
        length -= 128;
    }
    std::uint8_t final_block[128];
#pragma unroll
    for (int i = 0; i < 128; ++i) {
        final_block[i] = i < length ? input[i] : 0;
    }
    processed += static_cast<std::uint64_t>(length);
    compress(state, final_block, processed, true);
#pragma unroll
    for (int i = 0; i < 8; ++i) {
        store64_le(output + i * 8, state[i]);
    }
}

__device__ __forceinline__ int decimal_u32(std::uint32_t value,
                                           std::uint8_t output[10]) {
    std::uint8_t reversed[10];
    int length = 0;
    do {
        reversed[length++] = static_cast<std::uint8_t>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (int i = 0; i < length; ++i) {
        output[i] = reversed[length - i - 1];
    }
    return length;
}

__device__ __forceinline__ bool meets_difficulty(const std::uint8_t hash[64],
                                                 std::uint16_t difficulty) {
    const int full_bytes = difficulty / 2;
    for (int i = 0; i < full_bytes; ++i) {
        if (hash[i] != 0) {
            return false;
        }
    }
    return (difficulty & 1) == 0 || (hash[full_bytes] & 0xf0) == 0;
}

// The only mining kernel: tuned for the user's sm_75 GTX 1660 SUPER.
__global__ void mine_kernel(std::uint64_t start_nonce,
                            std::uint64_t count,
                            std::uint16_t difficulty,
                            int* found,
                            std::uint32_t* nonce_output,
                            std::uint8_t* hash_output) {
    const auto index =
        static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= count || *found != 0) {
        return;
    }
    const auto nonce = static_cast<std::uint32_t>(start_nonce + index);

    std::uint8_t message[kMaxMessage];
    int message_length = 0;
    for (int i = 0; i < c_prefix_length; ++i) {
        message[message_length++] = c_prefix[i];
    }
    std::uint8_t decimal[10];
    const int decimal_length = decimal_u32(nonce, decimal);
    for (int i = 0; i < decimal_length; ++i) {
        message[message_length++] = decimal[i];
    }
    for (int i = 0; i < c_suffix_length; ++i) {
        message[message_length++] = c_suffix[i];
    }

    std::uint8_t first_digest[64];
    std::uint8_t first_hex[128];
    std::uint8_t second_digest[64];
    constexpr char hex[] = "0123456789abcdef";
    blake2b_512_device(message, message_length, first_digest);
#pragma unroll
    for (int i = 0; i < 64; ++i) {
        first_hex[i * 2] = static_cast<std::uint8_t>(hex[first_digest[i] >> 4]);
        first_hex[i * 2 + 1] =
            static_cast<std::uint8_t>(hex[first_digest[i] & 0x0f]);
    }
    blake2b_512_device(first_hex, 128, second_digest);

    if (meets_difficulty(second_digest, difficulty) &&
        atomicCAS(found, 0, 1) == 0) {
        *nonce_output = nonce;
#pragma unroll
        for (int i = 0; i < 64; ++i) {
            hash_output[i] = second_digest[i];
        }
    }
}

void check_cuda(cudaError_t error, const char* operation) {
    if (error != cudaSuccess) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 cudaGetErrorString(error));
    }
}

template <typename T>
class DeviceBuffer {
public:
    explicit DeviceBuffer(std::size_t count) {
        check_cuda(cudaMalloc(reinterpret_cast<void**>(&data_), count * sizeof(T)),
                   "cudaMalloc");
    }
    ~DeviceBuffer() { cudaFree(data_); }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    T* get() const { return data_; }

private:
    T* data_{};
};

std::string digest_hex(const std::uint8_t digest[64]) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output(128, '\0');
    for (int i = 0; i < 64; ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return output;
}

}  // namespace

Solution mine(const MineRequest& request, const MineOptions& options) {
    if (request.difficulty < 3 || request.difficulty > 128) {
        throw std::invalid_argument("difficulty must be between 3 and 128");
    }
    if (request.start_nonce >= kNonceSpace || request.end_nonce > kNonceSpace ||
        request.start_nonce >= request.end_nonce || options.batch_size == 0) {
        throw std::invalid_argument("invalid GPU mining range or batch size");
    }
    check_cuda(cudaSetDevice(options.cuda_device), "cudaSetDevice");
    cudaDeviceProp properties{};
    check_cuda(cudaGetDeviceProperties(&properties, options.cuda_device),
               "cudaGetDeviceProperties");
    if (properties.major < 6) {
        throw std::runtime_error("CUDA compute capability 6.0 or newer is required");
    }

    const auto prefix = std::to_string(request.ts) + ":";
    const auto suffix =
        ":" + std::to_string(request.difficulty) + ":" + request.challenge;
    if (prefix.size() > kMaxPrefix || suffix.size() > kMaxSuffix) {
        throw std::runtime_error("challenge is too long for CUDA constant memory");
    }
    const int prefix_length = static_cast<int>(prefix.size());
    const int suffix_length = static_cast<int>(suffix.size());
    check_cuda(cudaMemcpyToSymbol(c_prefix, prefix.data(), prefix.size()),
               "copy CUDA prefix");
    check_cuda(cudaMemcpyToSymbol(c_suffix, suffix.data(), suffix.size()),
               "copy CUDA suffix");
    check_cuda(cudaMemcpyToSymbol(
                   c_prefix_length, &prefix_length, sizeof(prefix_length)),
               "copy CUDA prefix length");
    check_cuda(cudaMemcpyToSymbol(
                   c_suffix_length, &suffix_length, sizeof(suffix_length)),
               "copy CUDA suffix length");

    DeviceBuffer<int> found_device(1);
    DeviceBuffer<std::uint32_t> nonce_device(1);
    DeviceBuffer<std::uint8_t> hash_device(64);
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t attempts = 0;

    for (auto cursor = request.start_nonce; cursor < request.end_nonce;) {
        const auto count = std::min(options.batch_size, request.end_nonce - cursor);
        const auto block_count = (count + kBlockThreads - 1) / kBlockThreads;
        if (block_count > static_cast<std::uint64_t>(properties.maxGridSize[0])) {
            throw std::runtime_error("CUDA batch size exceeds the device grid limit");
        }
        check_cuda(cudaMemset(found_device.get(), 0, sizeof(int)), "reset CUDA result");
        const auto blocks = static_cast<unsigned int>(block_count);
        mine_kernel<<<blocks, kBlockThreads>>>(cursor,
                                               count,
                                               request.difficulty,
                                               found_device.get(),
                                               nonce_device.get(),
                                               hash_device.get());
        check_cuda(cudaGetLastError(), "launch CUDA kernel");
        check_cuda(cudaDeviceSynchronize(), "run CUDA kernel");

        int found = 0;
        check_cuda(cudaMemcpy(&found,
                              found_device.get(),
                              sizeof(found),
                              cudaMemcpyDeviceToHost),
                   "read CUDA result");
        attempts += count;
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (!options.quiet) {
            const auto rate = elapsed > 0 ? attempts / elapsed / 1'000'000.0 : 0.0;
            std::cerr << '\r' << properties.name << " | " << attempts
                      << " tries | " << std::fixed
                      << std::setprecision(2) << rate << " MH/s" << std::flush;
        }
        if (found != 0) {
            Solution solution;
            std::uint8_t digest[64];
            check_cuda(cudaMemcpy(&solution.nonce,
                                  nonce_device.get(),
                                  sizeof(solution.nonce),
                                  cudaMemcpyDeviceToHost),
                       "read CUDA nonce");
            check_cuda(cudaMemcpy(digest,
                                  hash_device.get(),
                                  sizeof(digest),
                                  cudaMemcpyDeviceToHost),
                       "read CUDA hash");
            solution.hash = digest_hex(digest);
            if (solution.hash.compare(0,
                                      request.difficulty,
                                      std::string(request.difficulty, '0')) != 0) {
                throw std::runtime_error("CUDA returned a digest below difficulty");
            }
            solution.attempts = attempts;
            solution.elapsed_seconds = elapsed;
            if (!options.quiet) {
                std::cerr << '\n';
            }
            return solution;
        }
        cursor += count;
    }

    if (!options.quiet) {
        std::cerr << '\n';
    }
    throw std::runtime_error("nonce range exhausted without a solution");
}

}  // namespace sui_pow
