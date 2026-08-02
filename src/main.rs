use blake2::{Blake2b512, Digest};
use clap::{Args, Parser, Subcommand};
use reqwest::blocking::Client;
use reqwest::header::{CONTENT_TYPE, ORIGIN, REFERER};
use serde_json::{json, Value};
use std::error::Error;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{mpsc, Arc};
use std::thread;
use std::time::{Duration, Instant};

const API_CHALLENGE: &str = "https://faucet.testnet.sui.io/v2/request_challenge";
const API_FAUCET: &str = "https://faucet.testnet.sui.io/v2/faucet_web_gas";
const NONCE_SPACE: u64 = (u32::MAX as u64) + 1;
const HEX: &[u8; 16] = b"0123456789abcdef";

#[derive(Parser)]
#[command(version, about)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Solve a supplied challenge and print the solution as JSON.
    Mine(MineArgs),
    /// Request a Sui testnet challenge, solve it, and submit one faucet request.
    Faucet(FaucetArgs),
}

#[derive(Args, Clone)]
struct Tuning {
    /// Worker threads (defaults to available logical CPUs).
    #[arg(long)]
    threads: Option<usize>,
    /// Nonces claimed by each worker at a time.
    #[arg(long, default_value_t = 100_000)]
    chunk_size: u64,
    /// Disable progress output on stderr.
    #[arg(long)]
    quiet: bool,
}

#[derive(Args)]
struct MineArgs {
    #[arg(long)]
    ts: u64,
    #[arg(long)]
    difficulty: u16,
    #[arg(long)]
    challenge: String,
    /// First nonce to inspect.
    #[arg(long, default_value_t = 0)]
    start: u64,
    /// Maximum number of nonces to inspect.
    #[arg(long)]
    count: Option<u64>,
    #[command(flatten)]
    tuning: Tuning,
}

#[derive(Args)]
struct FaucetArgs {
    /// Sui address that receives the testnet gas.
    #[arg(long)]
    recipient: String,
    #[command(flatten)]
    tuning: Tuning,
}

#[derive(Debug)]
struct Solution {
    nonce: u32,
    hash: String,
    attempts: u64,
    elapsed: Duration,
}

fn main() -> Result<(), Box<dyn Error>> {
    match Cli::parse().command {
        Command::Mine(args) => {
            let end = range_end(args.start, args.count)?;
            let solution = mine(
                args.ts,
                args.difficulty,
                &args.challenge,
                args.start,
                end,
                &args.tuning,
            )?;
            println!("{}", solution_json(&solution));
        }
        Command::Faucet(args) => run_faucet(&args)?,
    }
    Ok(())
}

fn range_end(start: u64, count: Option<u64>) -> Result<u64, Box<dyn Error>> {
    if start >= NONCE_SPACE {
        return Err("--start must fit in the u32 nonce space".into());
    }
    Ok(match count {
        Some(n) => start.saturating_add(n).min(NONCE_SPACE),
        None => NONCE_SPACE,
    })
}

fn worker_count(requested: Option<usize>) -> usize {
    requested
        .unwrap_or_else(|| thread::available_parallelism().map_or(1, usize::from))
        .max(1)
}

fn mine(
    ts: u64,
    difficulty: u16,
    challenge: &str,
    start_nonce: u64,
    end_nonce: u64,
    tuning: &Tuning,
) -> Result<Solution, Box<dyn Error>> {
    if difficulty < 3 {
        return Err("difficulty must be greater than 2".into());
    }
    if tuning.chunk_size == 0 {
        return Err("--chunk-size must be greater than zero".into());
    }

    let threads = worker_count(tuning.threads);
    let cursor = Arc::new(AtomicU64::new(start_nonce));
    let attempts = Arc::new(AtomicU64::new(0));
    let best_zeros = Arc::new(AtomicU32::new(0));
    let stop = Arc::new(AtomicBool::new(false));
    let challenge = Arc::<str>::from(challenge);
    let (sender, receiver) = mpsc::channel();
    let started = Instant::now();

    for _ in 0..threads {
        let cursor = Arc::clone(&cursor);
        let attempts = Arc::clone(&attempts);
        let best_zeros = Arc::clone(&best_zeros);
        let stop = Arc::clone(&stop);
        let challenge = Arc::clone(&challenge);
        let sender = sender.clone();
        let chunk_size = tuning.chunk_size;

        thread::spawn(move || {
            let mut prefix = Blake2b512::new();
            prefix.update(ts.to_string().as_bytes());
            prefix.update(b":");
            let suffix = format!(":{difficulty}:{}", &*challenge);
            let mut decimal = itoa::Buffer::new();
            let mut first_hex = [0_u8; 128];
            let mut local_best = 0_usize;

            while !stop.load(Ordering::Relaxed) {
                let chunk_start = cursor.fetch_add(chunk_size, Ordering::Relaxed);
                if chunk_start >= end_nonce {
                    break;
                }
                let chunk_end = chunk_start.saturating_add(chunk_size).min(end_nonce);
                let mut local_attempts = 0_u64;

                for nonce in chunk_start..chunk_end {
                    if stop.load(Ordering::Relaxed) {
                        break;
                    }

                    let mut first = prefix.clone();
                    first.update(decimal.format(nonce).as_bytes());
                    first.update(suffix.as_bytes());
                    let first_digest = first.finalize();
                    hex_into(&first_digest, &mut first_hex);

                    let second_digest = Blake2b512::digest(&first_hex);
                    let zeros = leading_hex_zeros(&second_digest);
                    local_attempts += 1;
                    if zeros > local_best {
                        local_best = zeros;
                        best_zeros.fetch_max(zeros as u32, Ordering::Relaxed);
                    }

                    if zeros >= difficulty as usize {
                        stop.store(true, Ordering::Relaxed);
                        attempts.fetch_add(local_attempts, Ordering::Relaxed);
                        let result = (nonce as u32, hex_string(&second_digest));
                        let _ = sender.send(result);
                        return;
                    }
                }
                attempts.fetch_add(local_attempts, Ordering::Relaxed);
            }
        });
    }
    drop(sender);

    loop {
        match receiver.recv_timeout(Duration::from_millis(250)) {
            Ok((nonce, hash)) => {
                if !tuning.quiet {
                    eprintln!();
                }
                return Ok(Solution {
                    nonce,
                    hash,
                    attempts: attempts.load(Ordering::Relaxed),
                    elapsed: started.elapsed(),
                });
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {
                if !tuning.quiet {
                    let tried = attempts.load(Ordering::Relaxed);
                    let seconds = started.elapsed().as_secs_f64();
                    let rate = if seconds > 0.0 {
                        tried as f64 / seconds / 1_000_000.0
                    } else {
                        0.0
                    };
                    eprint!(
                        "\r{}/{} zeros: {:.1}s | {} threads | {} tries | {:.2} MH/s",
                        best_zeros.load(Ordering::Relaxed),
                        difficulty,
                        seconds,
                        threads,
                        tried,
                        rate
                    );
                }
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                if !tuning.quiet {
                    eprintln!();
                }
                return Err("nonce range exhausted without a solution".into());
            }
        }
    }
}

#[inline]
fn leading_hex_zeros(hash: &[u8]) -> usize {
    let mut zeros = 0;
    for &byte in hash {
        if byte == 0 {
            zeros += 2;
        } else {
            if byte >> 4 == 0 {
                zeros += 1;
            }
            break;
        }
    }
    zeros
}

#[inline]
fn hex_into(input: &[u8], output: &mut [u8]) {
    debug_assert_eq!(output.len(), input.len() * 2);
    for (byte, pair) in input.iter().zip(output.chunks_exact_mut(2)) {
        pair[0] = HEX[(byte >> 4) as usize];
        pair[1] = HEX[(byte & 0x0f) as usize];
    }
}

fn hex_string(input: &[u8]) -> String {
    let mut bytes = vec![0_u8; input.len() * 2];
    hex_into(input, &mut bytes);
    // The lookup table contains ASCII only.
    unsafe { String::from_utf8_unchecked(bytes) }
}

fn solution_json(solution: &Solution) -> Value {
    let seconds = solution.elapsed.as_secs_f64();
    json!({
        "nonce": solution.nonce,
        "hash": solution.hash,
        "attempts": solution.attempts,
        "elapsed_seconds": seconds,
        "mh_s": if seconds > 0.0 { solution.attempts as f64 / seconds / 1_000_000.0 } else { 0.0 }
    })
}

fn value_u64(value: &Value, name: &str) -> Result<u64, Box<dyn Error>> {
    value
        .as_u64()
        .or_else(|| value.as_str().and_then(|s| s.parse().ok()))
        .ok_or_else(|| format!("challenge.{name} is missing or invalid").into())
}

fn run_faucet(args: &FaucetArgs) -> Result<(), Box<dyn Error>> {
    let client = Client::builder().timeout(Duration::from_secs(120)).build()?;
    let challenge_response: Value = client
        .post(API_CHALLENGE)
        .header(CONTENT_TYPE, "application/json")
        .json(&json!({}))
        .send()?
        .error_for_status()?
        .json()?;

    let challenge_value = challenge_response
        .get("challenge")
        .ok_or("response does not contain challenge")?;
    let signature = challenge_response
        .get("signature")
        .ok_or("response does not contain signature")?
        .clone();
    let ts = value_u64(&challenge_value["ts"], "ts")?;
    let difficulty = u16::try_from(value_u64(
        &challenge_value["difficulty"],
        "difficulty",
    )?)
    .map_err(|_| "challenge.difficulty does not fit in u16")?;
    let challenge = challenge_value["challenge"]
        .as_str()
        .ok_or("challenge.challenge is missing or invalid")?;

    eprintln!("challenge: {challenge} (ts: {ts}, difficulty: {difficulty})");
    let solution = mine(ts, difficulty, challenge, 0, NONCE_SPACE, &args.tuning)?;

    let payload = json!({
        "FixedAmountRequest": {
            "recipient": args.recipient,
            "solution": {
                "nonce": solution.nonce,
                "hash": solution.hash,
                "challenge": challenge_value,
                "signature": signature
            }
        }
    });
    let response: Value = client
        .post(API_FAUCET)
        .header(CONTENT_TYPE, "application/json")
        .header(ORIGIN, "https://faucet.sui.io")
        .header(REFERER, "https://faucet.sui.io/")
        .json(&payload)
        .send()?
        .error_for_status()?
        .json()?;

    println!("{}", serde_json::to_string_pretty(&response)?);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn leading_zero_nibbles_match_hex_rule() {
        assert_eq!(leading_hex_zeros(&[0x00, 0x0a, 0xff]), 3);
        assert_eq!(leading_hex_zeros(&[0x01]), 1);
        assert_eq!(leading_hex_zeros(&[0x10]), 0);
    }

    #[test]
    fn reverse_engineered_vector_matches_wasm() {
        let mut first = Blake2b512::new();
        first.update(b"1700000000000:0:64:abc");
        let mut first_hex = [0_u8; 128];
        hex_into(&first.finalize(), &mut first_hex);
        let hash = hex_string(&Blake2b512::digest(&first_hex));
        assert_eq!(hash, "ecf90cec21a5ba151a8d57318540340215a0e2cc6a0e2e75cdd08cc4e98462061c790b80895348866e468bafb619a625f46e2d828070ae5afe190f4453f345fd");
    }
}
