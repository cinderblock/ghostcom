#!/usr/bin/env bun
/**
 * Generates the Allure HTML report from allure-results/ into allure-report/,
 * preserving history/trend data across runs.
 *
 * History lives in allure-history/ (committed to git) so the trend chart
 * persists across machines and clean checkouts. Workflow:
 *   1. Copy allure-history/* → allure-results/history/*
 *   2. allure generate allure-results -o allure-report --clean
 *   3. Copy allure-report/history/* → allure-history/*
 */
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, cpSync, rmSync } from "node:fs";
import path from "node:path";

const ROOT = path.resolve(".");
const RESULTS_DIR = path.join(ROOT, "allure-results");
const REPORT_DIR  = path.join(ROOT, "allure-report");
const HISTORY_DIR = path.join(ROOT, "allure-history");

if (!existsSync(RESULTS_DIR)) {
  console.error(`No results at ${RESULTS_DIR}. Run \`bun run test\` first.`);
  process.exit(2);
}

// 1. Restore committed history so `allure generate` produces a trend chart.
if (existsSync(HISTORY_DIR)) {
  const dst = path.join(RESULTS_DIR, "history");
  if (existsSync(dst)) rmSync(dst, { recursive: true, force: true });
  mkdirSync(dst, { recursive: true });
  cpSync(HISTORY_DIR, dst, { recursive: true });
  console.log(`restored history from ${HISTORY_DIR} → ${dst}`);
} else {
  console.log("no prior history found — this run starts a fresh trend");
}

// 2. Run `allure generate`. allure-commandline is a devDependency; when
//    invoked via `bun run`, node_modules/.bin is on PATH.
const r = spawnSync(
  "allure",
  ["generate", RESULTS_DIR, "-o", REPORT_DIR, "--clean"],
  { stdio: "inherit", shell: true },
);
if (r.status !== 0) {
  console.error(`allure generate failed with code ${r.status}`);
  console.error("Requires Java on PATH. Install Temurin JRE 21:");
  console.error("  winget install EclipseAdoptium.Temurin.21.JRE");
  process.exit(r.status ?? 1);
}

// 3. Persist the updated history for the next run.
const src = path.join(REPORT_DIR, "history");
if (existsSync(src)) {
  if (existsSync(HISTORY_DIR)) rmSync(HISTORY_DIR, { recursive: true, force: true });
  mkdirSync(HISTORY_DIR, { recursive: true });
  cpSync(src, HISTORY_DIR, { recursive: true });
  console.log(`persisted history to ${HISTORY_DIR}`);
}

console.log(`\nreport: file:///${REPORT_DIR.replace(/\\/g, "/")}/index.html`);
console.log(`serve locally with: allure open ${REPORT_DIR}`);
