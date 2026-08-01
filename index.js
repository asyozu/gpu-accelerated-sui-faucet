const axios = require('axios');
const { Worker } = require('worker_threads');
const os = require('os');
const path = require('path');

const RECIPIENT_ADDRESS = "0xf5daa612aecf13d9c59f8f62cc09e8dd8748205eb6fc2ad1bda966c9e30de8ea";

const API_CHALLENGE = "https://faucet.testnet.sui.io/v2/request_challenge";
const API_FAUCET = "https://faucet.testnet.sui.io/v2/faucet_web_gas";

function solvePoW(challengeData) {
    return new Promise((resolve, reject) => {
        const numThreads = os.cpus().length;
        const chunkSize = 100000;

        let resolved = false;
        let totalAttempts = 0;
        let bestZeros = 0;

        const workers = [];
        const startTime = Date.now();

        console.log(`Initialize ${numThreads} cores solve PoW...`);

        const timer = setInterval(() => {
            const elapsed = (Date.now() - startTime) / 1000;

            const mhs =
                elapsed > 0
                    ? totalAttempts / elapsed / 1e6
                    : 0;

            process.stdout.write(
                `\r${bestZeros}/${challengeData.difficulty} zeros: ${elapsed.toFixed(
                    1
                )}s • ${numThreads} cores • ${totalAttempts.toLocaleString()} tries • ${mhs.toFixed(
                    2
                )} MH/s`
            );
        }, 100);

        for (let i = 0; i < numThreads; i++) {
            const worker = new Worker(path.join(__dirname, "worker.js"), {
                workerData: {
                    workerIndex: i,
                    workerCount: numThreads,
                    chunkSize,
                    challenge: challengeData.challenge,
                    ts: challengeData.ts,
                    difficulty: challengeData.difficulty,
                },
            });

            workers.push(worker);

            worker.on("message", (msg) => {
                if (resolved) return;

                if (msg.type === "progress") {
                    totalAttempts += msg.attempts || 0;

                    if (msg.zeros > bestZeros)
                        bestZeros = msg.zeros;

                    return;
                }

                const nonce =
                    msg?.nonce ??
                    msg?.solution?.nonce ??
                    msg?.result?.nonce;

                const hash =
                    msg?.hash ??
                    msg?.solution?.hash ??
                    msg?.result?.hash;

                if (nonce !== undefined && hash !== undefined) {
                    resolved = true;

                    clearInterval(timer);
                    process.stdout.write("\n");

                    workers.forEach((w) => w.terminate());

                    resolve({
                        nonce,
                        hash,
                    });
                }
            });

            worker.on("error", (err) => {
                if (resolved) return;

                resolved = true;

                clearInterval(timer);

                workers.forEach((w) => w.terminate());

                reject(err);
            });
        }
    });
}

async function main() {
    try {
        const { data: challengeRes } = await axios.post(
            API_CHALLENGE,
            {},
            {
                headers: {
                    "Content-Type": "application/json",
                },
            }
        );

        const { challenge, signature } = challengeRes;

        console.log(
            `Challenge: ${challenge.challenge} (ts: ${challenge.ts}, difficulty: ${challenge.difficulty})`
        );

        const startTime = Date.now();

        const solution = await solvePoW(challenge);

        const elapsed = ((Date.now() - startTime) / 1000).toFixed(2);

        console.log(
            `Nonce found in ${elapsed}s! Nonce: ${solution.nonce}`
        );

        const payload = {
            FixedAmountRequest: {
                recipient: RECIPIENT_ADDRESS,
                solution: {
                    nonce: solution.nonce,
                    hash: solution.hash,
                    challenge,
                    signature,
                },
            },
        };

        const { data: faucetRes } = await axios.post(
            API_FAUCET,
            payload,
            {
                headers: {
                    "Content-Type": "application/json",
                    Origin: "https://faucet.sui.io",
                    Referer: "https://faucet.sui.io/",
                },
            }
        );

        console.log(`Status : ${faucetRes.status}`);

        if (Array.isArray(faucetRes.coins_sent)) {
            faucetRes.coins_sent.forEach((coin) => {
                console.log(
                    `Amount: ${(coin.amount / 1e9).toLocaleString()} SUI`
                );
                console.log(`id: ${coin.id}`);
                console.log(
                    `transferTxDigest: ${coin.transferTxDigest}`
                );
            });
        }
    } catch (error) {
        if (error.response?.data) {
            const data = error.response.data;

            if (data.status) {
                const statusName = Object.keys(data.status)[0];
                console.log(`Status: ${statusName}`);

                const detail = data.status[statusName];

                if (typeof detail === "object" && detail !== null) {
                    for (const [key, value] of Object.entries(detail)) {
                        console.log(`${key}: ${value}`);
                    }
                } else {
                    console.log(detail);
                }

                if (data.coins_sent) {
                    console.log("Coins Sent:");
                    console.log(JSON.stringify(data.coins_sent, null, 2));
                }

                return;
            }

            console.error(JSON.stringify(data, null, 2));
            return;
        }

        console.error("Error:", error.message);
    }
}

main();