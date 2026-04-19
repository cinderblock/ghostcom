#!/usr/bin/env bun
/**
 * Runs all test files sequentially, generates the Allure report, and
 * ensures the Allure web server is running so the report is always
 * viewable at http://<host>:4040.
 *
 * Bun's parallel test runner conflicts with driver state, so files
 * run one at a time. Each file produces a JUnit XML in allure-results/.
 * After all files finish, the Allure report is regenerated and the
 * file server is (re)started.
 */
import { spawnSync, spawn } from "node:child_process";
import { existsSync, mkdirSync, rmSync, readdirSync } from "node:fs";
import path from "node:path";

const TESTS = [
  "tests/e2e.test.ts",
  "tests/compat.test.ts",
  "tests/enumeration.test.ts",
  "tests/robustness.test.ts",
];

const ROOT = path.resolve(".");
const RESULTS_DIR = path.join(ROOT, "allure-results");
const REPORT_DIR  = path.join(ROOT, "allure-report");
const ALLURE_PORT = 4040;

// ── Helpers ─────────────────────────────────────────────────────

function sleepMs(ms: number) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function killOrphanBun() {
  /* Kill any stray bun.exe processes left over from a previous test
   * file that timed out. These orphans keep native-addon handles open
   * on \\.\GhostCOMControl, which prevents the next file from making
   * a clean start and causes cascading timeouts. Exclude our own bun
   * (running this script) and spare the running Allure server. */
  const myPid = process.pid;
  spawnSync("powershell", [
    "-NoProfile", "-Command",
    `Get-Process bun -ErrorAction SilentlyContinue | ` +
    `Where-Object { $_.Id -ne ${myPid} -and ` +
    `(Get-NetTCPConnection -OwningProcess $_.Id -LocalPort ${ALLURE_PORT} -ErrorAction SilentlyContinue) -eq $null } | ` +
    `Stop-Process -Force -ErrorAction SilentlyContinue`,
  ], { stdio: "ignore" });
  sleepMs(500);
}

function runCleanup() {
  console.log("── cleanup ports ──");
  for (let round = 0; round < 3; round++) {
    const r = spawnSync("bun", ["run", "tests/cleanup.js"], {
      stdio: "inherit",
      shell: true,
    });
    if (r.status !== 0) console.warn(`cleanup exit=${r.status}`);
    sleepMs(2000);
  }
}

function killProcessOnPort(port: number) {
  try {
    const r = spawnSync("powershell", [
      "-NoProfile", "-Command",
      `Get-NetTCPConnection -LocalPort ${port} -ErrorAction SilentlyContinue | ` +
      `ForEach-Object { Stop-Process -Id $_.OwningProcess -Force -ErrorAction SilentlyContinue }`,
    ], { encoding: "utf8" });
  } catch { /* ignore */ }
}

function startAllureServer() {
  killProcessOnPort(ALLURE_PORT);
  sleepMs(1000);

  const server = spawn("bun", ["-e", `
    Bun.serve({
      port: ${ALLURE_PORT},
      hostname: "0.0.0.0",
      fetch(req) {
        const url = new URL(req.url);
        let p = url.pathname === "/" ? "/index.html" : url.pathname;
        return new Response(Bun.file("${REPORT_DIR.replace(/\\/g, "/")}" + p));
      },
    });
  `], {
    stdio: "ignore",
    detached: true,
    shell: true,
  });
  server.unref();
  console.log(`allure server started on http://0.0.0.0:${ALLURE_PORT} (pid ${server.pid})`);
}

function generateReport() {
  console.log("\n── generating allure report ──");
  const r = spawnSync("bun", ["run", "scripts/generate-allure-report.ts"], {
    stdio: "inherit",
    shell: true,
    timeout: 60_000,
  });
  if (r.status !== 0) {
    console.warn(`report generation exited with ${r.status}`);
  }
}

// ── Main ────────────────────────────────────────────────────────

// Fresh results dir (preserve history subdir).
if (existsSync(RESULTS_DIR)) {
  for (const f of readdirSync(RESULTS_DIR)) {
    if (f === "history") continue;
    rmSync(path.join(RESULTS_DIR, f), { recursive: true, force: true });
  }
} else {
  mkdirSync(RESULTS_DIR, { recursive: true });
}

// Run each test file.
let totalFailed = 0;
for (const file of TESTS) {
  console.log(`\n── ${file} ──`);
  runCleanup();
  const outFile = path.join(
    RESULTS_DIR,
    path.basename(file).replace(/\.ts$/, ".xml"),
  );
  const r = spawnSync(
    "bun",
    [
      "test",
      file,
      "--timeout", "30000",
      "--reporter=junit",
      `--reporter-outfile=${outFile}`,
    ],
    {
      stdio: "inherit",
      shell: true,
      timeout: 180_000,
    },
  );
  if (r.status !== 0) {
    totalFailed++;
    console.log(`(${file} exited with ${r.status})`);
    /* Timeout or crash — child bun may have been orphaned and is
     * still holding driver handles. Clean it up so the next file
     * can start fresh. */
    killOrphanBun();
  }
}

runCleanup();

// ── Summary ─────────────────────────────────────────────────────

console.log(`\n── summary ──`);
console.log(`test files run: ${TESTS.length}`);
console.log(`test files with failures: ${totalFailed}`);
console.log(`results written to: ${RESULTS_DIR}`);

// ── Always generate Allure report ───────────────────────────────

generateReport();

// ── Always (re)start the Allure server ──────────────────────────

startAllureServer();

process.exit(totalFailed === 0 ? 0 : 1);
