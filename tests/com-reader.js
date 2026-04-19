// Reads from a COM port using raw sync FileStream and reports to stdout.
// Usage: bun run tests/com-reader.js COM10 6 8000
// Exit 0 = received expected bytes, Exit 1 = timeout

const [,, port = 'COM10', bytesStr = '6', timeoutStr = '8000'] = process.argv;
const BYTES = parseInt(bytesStr, 10);
const TIMEOUT_MS = parseInt(timeoutStr, 10);
const path = `\\\\.\\${port}`;

process.stdout.write(`STARTING port=${port}\n`);

let fd;
try {
  // Open COM port synchronously (no FILE_FLAG_OVERLAPPED)
  fd = require('node:fs').openSync(path, 'r+');
  process.stdout.write(`OPEN\n`);
} catch (e) {
  process.stdout.write(`ERROR:${e.message}\n`);
  process.exit(1);
}

const buf = Buffer.alloc(BYTES);
let total = 0;
const deadline = Date.now() + TIMEOUT_MS;

while (total < BYTES && Date.now() < deadline) {
  try {
    const n = require('node:fs').readSync(fd, buf, total, BYTES - total, null);
    if (n > 0) total += n;
  } catch (e) {
    // Might get EAGAIN or similar — loop
  }
}

require('node:fs').closeSync(fd);
const str = buf.slice(0, total).toString('latin1');
process.stdout.write(`RECEIVED:${total}:${str}\n`);
process.exit(total >= BYTES ? 0 : 1);
