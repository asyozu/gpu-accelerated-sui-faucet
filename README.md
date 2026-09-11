# Sui Faucet PoW Client

A C++17/CUDA client for the public Sui faucet v3 automation API. It fetches a recipient-bound challenge, computes Argon2d proofs on an NVIDIA GPU, and submits the exact proof fields required by `/v3/gas`.

## Proof Protocol

The client implements proof version 1. The password is the following eight UTF-8 fields joined with newline characters and no trailing newline:

```text
sui-faucet-pow/1
<chainId>
<checkpointSeq>
<checkpointDigest>
<randomBytes>
<faucetAddress>
<recipient>
<nonce>
```

It hashes this preimage with Argon2d v=19, 8192 KiB, one iteration, one lane, salt `sui-faucet-pow-1`, and a 32-byte output. The first eight output bytes are interpreted as a big-endian unsigned integer; a proof is accepted locally when that value is below the challenge's `threshold`. The full 32-byte result is submitted as 64 lowercase hexadecimal characters.

Challenge hashing parameters are checked before mining. Unknown versions or parameter changes are rejected rather than silently producing incompatible proofs. The CUDA kernel and host-side block initialization are from [WebDollar/argon2-gpu](https://github.com/WebDollar/argon2-gpu), pinned to commit `f5fce223d16769f202449c9b0837ad3533304acd`; the selected source files are covered by its MIT license, reproduced in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Build on Windows

Prerequisites:

1. NVIDIA GPU with CUDA support and a current driver.
2. CUDA Toolkit 13.x with `nvcc` (tested with CUDA 13.4 on a GTX 1660 Super).
3. Visual Studio 2022 Build Tools with the Desktop development with C++ workload.
4. CMake 3.24 or newer.
5. Network access during the first configure to download the pinned Argon2 GPU source archive.

From a Visual Studio Developer PowerShell or Native Tools prompt:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build\Release\sui-pow.exe`. CMake targets CUDA architecture `75` for the GTX 1660 Super. `self-test` checks the official v3 vectors on the GPU, including a challenge with empty checkpoint randomness.

## Request SUI

```powershell
build\Release\sui-pow.exe faucet --recipient 0xYOUR_64_HEX_CHARACTER_ADDRESS
```

The client uses `https://faucet.testnet.sui.io`. A fresh challenge is fetched for each `--try` attempt; the client does not automatically grind a replacement proof after an uncertain submission.

Useful options:

- `--try N` requests N independent payouts in sequence; default is 1.
- `--proxy HOST:PORT[:USER:PASS]` routes challenge and payout calls through an HTTP proxy.
- `--proxy` without a value reads proxy entries from `proxies.txt` in the current directory.
- `--progress-interval N` sets the mining progress interval; default is 100 hashes. After solving, the client reports the nonce and mining time, then reports how long the faucet takes to respond.
- `--batch-size N` sets the number of GPU jobs per batch; it must be a power of two and defaults to 128. Each job reserves 8 MiB of device memory, so the default batch uses about 1 GiB. Reduce it if other GPU applications need memory.
- `--device N` selects a CUDA device index; default is 0.
- `--quiet` hides mining progress.

For an uncertain submission, preserve the original endpoint and proof fields. Resolve any returned transaction digest before requesting another payout. See the [Faucet automation guide](https://faucet.sui.io/how-to-use/#automation) and [proof-of-work specification](https://faucet.sui.io/pow-spec/) for protocol details and recovery behavior.

## Standalone Mining

`mine` accepts the public challenge fields directly and emits a JSON solution:

```powershell
build\Release\sui-pow.exe mine `
  --chain-id CHAIN_ID `
  --checkpoint-seq CHECKPOINT_SEQUENCE `
  --checkpoint-digest CHECKPOINT_DIGEST `
  --random-bytes RANDOM_BYTES_BASE64 `
  --faucet-address 0xFAUCET_ADDRESS `
  --recipient 0xRECIPIENT `
  --threshold THRESHOLD
```

Use `--start N` and `--count N` to restrict the u64 nonce range. `--batch-size` controls GPU occupancy and memory use during standalone mining as well.