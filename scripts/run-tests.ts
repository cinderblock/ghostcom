#!/usr/bin/env bun
/**
 * Runs all test files sequentially and regenerates the Allure report.
 * The Allure web server runs independently under the "Allure Server"
 * scheduled task (launched at logon) and serves the regenerated
 * report automatically via lazy file reads — we deliberately do NOT
 * touch that server here, since killing and respawning it would
 * orphan an unsupervised process and race the task's restart policy.
 *
 * Bun's parallel test runner conflicts with driver state, so files
 * run one at a time. Each file produces a JUnit XML in allure-results/.
 * After all files finish, the Allure report is regenerated.
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, rmSync, readdirSync } from "node:fs";
import path from "node:path";

const TESTS = [
  "tests/e2e.test.ts",
  "tests/compat.test.ts",
  "tests/enumeration.test.ts",
  "tests/robustness.test.ts",
  // Bun × serialport regression tracker — expected to fail today
  // (uv_async_send from CreateThread doesn't wake Bun's loop; see
  // ISSUES.md). When Bun fixes it, these tests start passing and we
  // notice immediately. Kept in the Bun suite so the report surfaces it.
  "tests/serialport-pkg.bun.test.ts",
];

// Node × serialport — the "does it actually work?" suite, run under
// `node --test` rather than `bun test`. Skipped gracefully when Node
// is not on PATH so Bun-only machines keep working.
const NODE_TESTS = [
  "tests/serialport-pkg.node.test.mjs",
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

/**
 * Return the node executable name if Node.js is on PATH, else null.
 * We test with `node --version` rather than `where` so a PATH shim
 * that fails to run is treated as absent.
 */
function detectNode(): string | null {
  const r = spawnSync("node", ["--version"], {
    stdio: ["ignore", "pipe", "pipe"],
    shell: true,
    timeout: 5_000,
  });
  if (r.status !== 0) return null;
  const ver = (r.stdout?.toString() ?? "").trim();
  console.log(`node detected: ${ver}`);
  return "node";
}

/**
 * Run a single node:test file. Emits a JUnit XML into allure-results
 * (node 20.13+). Returns 0 on success, non-zero on any test failure.
 */
function runNodeTest(file: string, outFile: string): number {
  const r = spawnSync(
    "node",
    [
      "--test",
      "--test-reporter=junit",
      `--test-reporter-destination=${outFile}`,
      "--test-reporter=spec",
      "--test-reporter-destination=stdout",
      file,
    ],
    {
      stdio: "inherit",
      shell: true,
      timeout: 180_000,
    },
  );
  return r.status ?? 1;
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

// ── Node.js test suite (serialport package coverage) ───────────────
// Runs after all Bun tests so the driver is in a clean state. These
// tests are optional — if Node.js isn't installed, we log a skip and
// carry on.

const nodeExe = detectNode();
let nodeFilesRun = 0;
let nodeSkipped = false;

if (!nodeExe) {
  console.log(
    "\n── node suite skipped: `node` not found on PATH ──\n" +
    "   Install Node.js (https://nodejs.org or `winget install OpenJS.NodeJS`)\n" +
    "   to enable serialport-pkg.node.test.mjs — the real-world coverage\n" +
    "   for the `serialport` npm package.",
  );
  nodeSkipped = true;
} else {
  for (const file of NODE_TESTS) {
    console.log(`\n── ${file} (node --test) ──`);
    runCleanup();
    const outFile = path.join(
      RESULTS_DIR,
      path.basename(file).replace(/\.(mjs|cjs|js|ts)$/, ".xml"),
    );
    const status = runNodeTest(file, outFile);
    nodeFilesRun++;
    if (status !== 0) {
      totalFailed++;
      console.log(`(${file} exited with ${status})`);
    }
  }
  runCleanup();
}

// ── Summary ─────────────────────────────────────────────────────

console.log(`\n── summary ──`);
console.log(`bun test files run:  ${TESTS.length}`);
console.log(`node test files run: ${nodeFilesRun}${nodeSkipped ? " (skipped — no node)" : ""}`);
console.log(`test files with failures: ${totalFailed}`);
console.log(`results written to: ${RESULTS_DIR}`);

// ── Always generate Allure report ───────────────────────────────
// The "Allure Server" scheduled task is already serving allure-report/
// and will pick up regenerated files on the next request.

generateReport();

process.exit(totalFailed === 0 ? 0 : 1);
