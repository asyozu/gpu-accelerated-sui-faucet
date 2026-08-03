# sui-pow GPU-only

A C++17/CUDA implementation dedicated strictly to PoW mining on NVIDIA GPUs. The CPU miner, AVX2 support, scalar BLAKE2b, and fallback mechanisms have been completely removed. The CPU only performs mandatory host-side CUDA orchestration: calling APIs, parsing JSON/HTTP, launching kernels, and retrieving solutions.

## Verified Algorithm

```text
message = decimal(ts) + ":" + decimal(nonce) + ":" + decimal(difficulty) + ":" + challenge
first   = lowercase_hex(BLAKE2b-512(UTF-8(message)))
hash    = lowercase_hex(BLAKE2b-512(ASCII(first)))
valid   = hash has at least `difficulty` consecutive leading '0' characters
```

Both BLAKE2b passes run within the same CUDA thread. Intermediate digests are never copied back to the CPU; each batch only checks a `found` flag. The winning nonce and 64-byte digest are transferred to the host only when a valid solution is found.

## GTX 1660 Super Kernel

The project retains a single kernel verified for `sm_75`. It uses native `uint64_t` BLAKE2b operations and preserves the twelve rounds inside an actual loop (`#pragma unroll 1`). Compiled with CUDA 13.3 on a GTX 1660 Super, the kernel consumes 168 registers, a 512-byte stack frame, produces zero spills, and achieves **333.04 MH/s** at a batch size of `16777216`.

Experimental direct-word and `uint32_t` pair implementations have been removed: direct-word only reached **214.11 MH/s**, while `uint32_t` pairs failed to outperform the reference kernel and did not match the WASM vector on hardware. Unrolled variants, register-clamping builds, kernel selector enums, and the `--gpu-kernel` option have also been removed. Block size is fixed at `128`; default batch size is `16777216`.

## Building on Windows

Prerequisites:

1. NVIDIA GPU driver.
2. CUDA Toolkit with `nvcc` (verified on CUDA 13.3).
3. Visual Studio / Build Tools 2022 with the **Desktop development with C++** workload.
4. MSVC v143, Windows SDK, and CMake tools for Windows.

Open the **x64 Native Tools Command Prompt for VS 2022**:

```bat
cmake -S . -B build-gpu -G "Visual Studio 17 2022" -A x64
cmake --build build-gpu --config Release
ctest --test-dir build-gpu -C Release --output-on-failure
```

The output binary is located at:

```text
build-gpu\Release\sui-pow.exe
```

CMake locks the CUDA architecture to `75` (matching the GTX 1660 Super). Verbose `ptxas` output is enabled by default to log register counts, stack frame sizes, spill stores, and spill loads during build time.

## GPU Self-Test

The self-test requires an NVIDIA GPU and validates the kernel against the test vector from `wasm_pow_bg.wasm`:

```bat
build-gpu\Release\sui-pow.exe self-test
```

Expected output:

```text
self-test passed: CUDA kernel matches wasm_pow_bg.wasm
```

## Running the Faucet

```bat
sui-pow.exe faucet --recipient 0xYOUR_SUI_ADDRESS
```

Select a specific CUDA device if necessary:

```bat
sui-pow.exe faucet --recipient 0xYOUR_SUI_ADDRESS --device 0
```

### HTTP Proxy

The `--proxy` flag applies to both challenge retrieval and solution submission requests. If omitted, the program falls back to the default Windows automatic proxy configuration.

Unauthenticated proxy:

```bat
sui-pow.exe faucet --recipient 0xYOUR_SUI_ADDRESS --proxy 127.0.0.1:8080
```

The `http://` prefix is optional:

```bat
sui-pow.exe faucet --recipient 0xYOUR_SUI_ADDRESS --proxy http://127.0.0.1:8080
```

HTTP Basic authentication with inline credentials:

```bat
sui-pow.exe faucet --recipient 0xYOUR_SUI_ADDRESS --proxy 199.7.142.28:35042:FSYbzM:nUjeyW
```

The format is `host:port:username:password`.

The client uses WinHTTP's HTTP CONNECT tunneling to reach the two faucet HTTPS endpoints. SOCKS4, SOCKS5, and proxies using the `https://` scheme are not supported. Using a proxy only changes your network routing; it does not bypass recipient-based rate limits or faucet cooldown policies.

## Benchmark

Setting difficulty to `128` will practically guarantee traversing the entire range and printing `nonce range exhausted`; this is standard behavior for benchmark runs.

```bat
sui-pow.exe mine --ts 1700000000000 --difficulty 128 --challenge abc --count 500000000
```

The progress output reports the active GPU, total attempts, and current MH/s. The default batch size is `16777216`; if you encounter display stuttering or Windows driver timeout errors (TDR), reduce it to `8388608`, `4194304`, `1048576`, or lower.

To inspect register, stack, and spill statistics without generating a new build directory, pass `--clean-first`:

```bat
cmake -S . -B build-gpu -G "Visual Studio 17 2022" -A x64
cmake --build build-gpu --config Release --clean-first --verbose > build.log 2>&1
findstr /i "ptxas registers spill stack frame" build.log
```

`findstr` is a separate second command; do not append it directly to the end of `cmake --build` without the pipe operator (`|`).

## Notes

- There is no CPU fallback. Systems without an NVIDIA GPU / CUDA support will not build or run the miner.
- Removing CPU code streamlines the codebase and eliminates unused branches; it does not inherently increase GPU MH/s.
- The returned hash difficulty is still verified on the host prior to submitting to the faucet, but it is not re-hashed on the CPU.