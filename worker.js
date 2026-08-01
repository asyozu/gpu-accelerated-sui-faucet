const { parentPort, workerData } = require('worker_threads');
const fs = require('fs');
const path = require('path');

process.on('uncaughtException', (err) => {
    parentPort.postMessage({ type: 'error', message: "Uncaught Exception: " + err.stack });
});

process.on('unhandledRejection', (reason) => {
    parentPort.postMessage({ type: 'error', message: "Unhandled Rejection: " + reason });
});

let messageHandler = null;

global.self = {
    location: {
        href: 'http://localhost/pow.worker.js'
    },
    postMessage: (msg) => {
        parentPort.postMessage(msg);
    },
    addEventListener: (type, listener) => {
        if (type === 'message') {
            messageHandler = listener;
        }
    },
    set onmessage(handler) {
        messageHandler = handler;
    },
    get onmessage() {
        return messageHandler;
    }
};

global.location = global.self.location;
global.postMessage = global.self.postMessage;
global.addEventListener = global.self.addEventListener;
global.importScripts = function() {};

parentPort.on('message', (data) => {
    if (messageHandler) {
        messageHandler({ data });
    }
});

global.fetch = async function(url) {
    const urlStr = url.toString();
    if (urlStr.includes('.wasm') || urlStr.endsWith('wasm_pow_bg.wasm')) {
        const wasmPath = path.join(__dirname, 'wasm_pow_bg.wasm');
        const buffer = fs.readFileSync(wasmPath);
        return new Response(buffer, {
            headers: { 'Content-Type': 'application/wasm' }
        });
    }
    throw new Error(`Fetch mock does not support this URL: ${urlStr}`);
};

try {
    require('./pow.worker.js');

    if (messageHandler && workerData) {
        messageHandler({ data: workerData });
    }
} catch (err) {
    parentPort.postMessage({ type: 'error', message: "Require Error: " + err.stack });
}