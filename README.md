# sui-pow-rust

A native Rust replacement for the `index.js -> miner.js -> worker.js -> pow.worker.js -> wasm_pow_bg.wasm` pipeline.

## Algorithm Reverse-Engineered from WASM

For each `u32` nonce, WASM executes:

```text
message = decimal(ts) + ":" + decimal(nonce) + ":" + decimal(difficulty) + ":" + challenge
first   = lowercase_hex(BLAKE2b-512(UTF-8(message)))
hash    = lowercase_hex(BLAKE2b-512(ASCII(first)))
valid   = hash starts with at least `difficulty` '0' characters
```

Note: This uses double BLAKE2b-512 hashing, where the second pass takes the **128-byte ASCII hex** string from the first pass rather than the raw 64-byte digest directly.

The legacy flow maps to the Rust implementation as follows:

| Legacy Component | Role | Rust Version |
| --- | --- | --- |
| `index.js` | Calls two faucet APIs and builds payload | `faucet` subcommand |
| `miner.js` | Spawns one Node worker per logical CPU | Native `std::thread` workers |
| `worker.js` | Emulates Web Worker and loads WASM | No longer needed |
| `pow.worker.js` | Splits nonce space into chunks | Atomic chunk allocator |
| `wasm_pow_bg.wasm::mine_range_scan` | Hashes each nonce, counts leading zero hex chars | Optimized native loop |

`difficulty` represents the number of leading **hex zero characters**, not zero bytes or zero bits. The success probability per attempt is `16^-difficulty`, with an expected attempt count of `16^difficulty`.

Direct reference test vector against the original WASM:

```text
ts         = 1700000000000
nonce      = 0
difficulty = 64
challenge  = abc
hash       = ecf90cec21a5ba151a8d57318540340215a0e2cc6a0e2e75cdd08cc4e98462061c790b80895348866e468bafb619a625f46e2d828070ae5afe190f4453f345fd
```

Another complete solution vector: with the same `ts/challenge` and `difficulty = 3`, the first valid nonce is `386`, yielding a hash starting with `000f0184...`. This output was executed directly via `wasm_pow_bg.wasm` and verified against an independent implementation.

## Build

Requires Rust stable:

```bash
cargo build --release
```

The compiled binary is located at `target/release/sui-pow`.

## Solve Challenge Only

```bash
./target/release/sui-pow mine \
  --ts 1700000000000 \
  --difficulty 4 \
  --challenge abc
```

Limit the search range for testing:

```bash
./target/release/sui-pow mine \
  --ts 1700000000000 \
  --difficulty 4 \
  --challenge abc \
  --threads 8 \
  --start 0 \
  --count 1000000 \
  --quiet
```

## Run Full Testnet Faucet Flow

```bash
./target/release/sui-pow faucet \
  --recipient 0xYOUR_SUI_ADDRESS
```

This command fetches a new challenge, solves it across all available logical CPUs, and submits a single request for testnet gas. Optional flags include `--threads N`, `--chunk-size N`, or `--quiet`.

## Why This Is Faster Than the Node + WASM Pipeline

- Eliminates the overhead of spawning multiple Node workers and separate WASM instances.
- Each native thread pre-computes the BLAKE2b state of the `ts:` prefix and clones that state for every nonce.
- Uses `itoa` and stack buffers instead of `format!` inside the hot nonce loop.
- Only allocates/formats the final hex string when a valid solution is found; checks leading zeros directly on the binary digest.
- The release profile enables fat LTO, a single codegen unit, and symbol stripping.

Benchmark on your actual target hardware; performance depends on CPU architecture, core count, frequency governor, and Rust compiler version. Run `cargo test --release` to verify against the original WASM test vectors.