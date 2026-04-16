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
import { existsSync, mkdirSync, cpSync, rmSync, readdirSync } from "node:fs";
import path from "node:path";
import os from "node:os";

const ROOT = path.resolve(".");
const RESULTS_DIR = path.join(ROOT, "allure-results");
const REPORT_DIR  = path.join(ROOT, "allure-report");
const HISTORY_DIR = path.join(ROOT, "allure-history");
// Orphan-branch worktree (see scripts/publish-allure-history.ts) — when
// present, history is sourced from (and written back to) there so the
// trend persists across clean checkouts via the `allure-history` branch.
const HISTORY_WT  = path.join(ROOT, ".allure-history-wt");

// 0. Ensure JAVA_HOME is set. Allure needs Java 8+. Auto-detect the
//    portable Temurin JRE under %USERPROFILE%\.jre (which is where
//    this project installs it if winget/msiexec can't elevate).
if (!process.env.JAVA_HOME || !existsSync(process.env.JAVA_HOME)) {
  const candidates: string[] = [];
  const userJre = path.join(os.homedir(), ".jre");
  if (existsSync(userJre)) {
    for (const entry of readdirSync(userJre)) {
      const p = path.join(userJre, entry, "bin", "java.exe");
      if (existsSync(p)) candidates.push(path.join(userJre, entry));
    }
  }
  const systemJre = "C:\\Program Files\\Eclipse Adoptium";
  if (existsSync(systemJre)) {
    for (const entry of readdirSync(systemJre)) {
      const p = path.join(systemJre, entry, "bin", "java.exe");
      if (existsSync(p)) candidates.push(path.join(systemJre, entry));
    }
  }
  if (candidates.length > 0) {
    process.env.JAVA_HOME = candidates[0];
    process.env.PATH = path.join(candidates[0], "bin") + path.delimiter + (process.env.PATH ?? "");
    console.log(`JAVA_HOME auto-detected: ${process.env.JAVA_HOME}`);
  } else {
    console.error("No JRE found. Install Temurin 21:");
    console.error("  winget install EclipseAdoptium.Temurin.21.JRE");
    console.error("  (or extract a ZIP under %USERPROFILE%\\.jre\\)");
    process.exit(2);
  }
}

if (!existsSync(RESULTS_DIR)) {
  console.error(`No results at ${RESULTS_DIR}. Run \`bun run test\` first.`);
  process.exit(2);
}

// 1. Restore committed history so `allure generate` produces a trend
//    chart. Prefer the orphan-branch worktree when checked out; fall
//    back to a local allure-history/ directory.
const histSource = existsSync(HISTORY_WT) ? HISTORY_WT
                 : existsSync(HISTORY_DIR) ? HISTORY_DIR
                 : null;
if (histSource) {
  const dst = path.join(RESULTS_DIR, "history");
  if (existsSync(dst)) rmSync(dst, { recursive: true, force: true });
  mkdirSync(dst, { recursive: true });
  for (const f of readdirSync(histSource)) {
    if (f === ".git") continue; // don't ingest worktree metadata
    cpSync(path.join(histSource, f), path.join(dst, f), { recursive: true });
  }
  console.log(`restored history from ${histSource} → ${dst}`);
} else {
  console.log("no prior history found — this run starts a fresh trend");
}

// 2. Run `allure generate`. allure-commandline is a devDependency; when
//    invoked via `bun run`, node_modules/.bin is on PATH. We pass env
//    explicitly — Bun's spawnSync doesn't always forward mutations to
//    `process.env` to child processes.
const r = spawnSync(
  "allure",
  ["generate", RESULTS_DIR, "-o", REPORT_DIR, "--clean"],
  { stdio: "inherit", shell: true, env: { ...process.env } },
);
if (r.status !== 0) {
  console.error(`allure generate failed with code ${r.status}`);
  console.error("Requires Java on PATH. Install Temurin JRE 21:");
  console.error("  winget install EclipseAdoptium.Temurin.21.JRE");
  process.exit(r.status ?? 1);
}

// 3. Persist the updated history for the next run. Write to both the
//    orphan-branch worktree (if present) and the plain allure-history/
//    directory — the publish script pushes whichever is populated.
const src = path.join(REPORT_DIR, "history");
if (existsSync(src)) {
  for (const dst of [HISTORY_WT, HISTORY_DIR]) {
    if (dst === HISTORY_WT && !existsSync(HISTORY_WT)) continue;
    if (existsSync(dst)) {
      for (const f of readdirSync(dst)) {
        if (f === ".git") continue;
        rmSync(path.join(dst, f), { recursive: true, force: true });
      }
    } else {
      mkdirSync(dst, { recursive: true });
    }
    for (const f of readdirSync(src)) {
      cpSync(path.join(src, f), path.join(dst, f), { recursive: true });
    }
    console.log(`persisted history to ${dst}`);
  }
}

console.log(`\nreport: file:///${REPORT_DIR.replace(/\\/g, "/")}/index.html`);
console.log(`serve locally with: allure open ${REPORT_DIR}`);
