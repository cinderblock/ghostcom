#!/usr/bin/env bun
/**
 * Runs all test files sequentially (Bun's parallel runner conflicts with
 * driver state) and emits one JUnit XML per file into allure-results/.
 *
 * Continues past failures so every file produces output, then propagates
 * the aggregate status as the exit code.
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, rmSync, readdirSync } from "node:fs";
import path from "node:path";

const TESTS = [
  "tests/e2e.test.ts",
  "tests/compat.test.ts",
  "tests/enumeration.test.ts",
  "tests/robustness.test.ts",
];

const RESULTS_DIR = path.resolve("allure-results");

// Fresh results dir each run; history (if present) is restored by
// generate-allure-report.js before `allure generate`.
if (existsSync(RESULTS_DIR)) {
  for (const f of readdirSync(RESULTS_DIR)) {
    if (f === "history") continue;
    rmSync(path.join(RESULTS_DIR, f), { recursive: true, force: true });
  }
} else {
  mkdirSync(RESULTS_DIR, { recursive: true });
}

function sleepMs(ms: number) { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms); }

function runCleanup() {
  console.log("── cleanup ports ──");
  // Multiple rounds — Driver Verifier slows teardown; ports from
  // previously killed processes need time to deregister.
  for (let round = 0; round < 3; round++) {
    const r = spawnSync("bun", ["run", "tests/cleanup.js"], { stdio: "inherit", shell: true });
    if (r.status !== 0) console.warn(`cleanup exit=${r.status}`);
    sleepMs(2000);
  }
}

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
      "--bail",
      "--timeout", "30000",
      "--reporter=junit",
      `--reporter-outfile=${outFile}`,
    ],
    {
      stdio: "inherit",
      shell: true,
      // Hard kill if bun test doesn't exit within 120s.  Native addon
      // TSFNs can keep the event loop alive after all tests pass; this
      // ensures the runner always makes progress.
      timeout: 120_000,
    },
  );
  if (r.status !== 0) {
    totalFailed++;
    console.log(`(${file} exited with ${r.status})`);
  }
}

runCleanup();

console.log(`\n── summary ──`);
console.log(`test files run: ${TESTS.length}`);
console.log(`test files with failures: ${totalFailed}`);
console.log(`results written to: ${RESULTS_DIR}`);

process.exit(totalFailed === 0 ? 0 : 1);
