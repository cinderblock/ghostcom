#!/usr/bin/env bun
/**
 * Publishes the current allure-history/ trend data to the orphan branch
 * `allure-history`. Creates the branch if missing, otherwise updates it.
 *
 * Runs outside the main worktree via `git worktree add` so the current
 * checkout is undisturbed — master stays on master.
 *
 * Usage:
 *   bun run scripts/publish-allure-history.ts [--message "baseline run"]
 */
import { spawnSync } from "node:child_process";
import { existsSync, cpSync, rmSync, mkdirSync, readdirSync } from "node:fs";
import path from "node:path";

const HISTORY_DIR = path.resolve("allure-history");
const WORKTREE    = path.resolve(".allure-history-wt"); // gitignored
const BRANCH      = "allure-history";

if (!existsSync(HISTORY_DIR) || readdirSync(HISTORY_DIR).length === 0) {
  console.error(`No history at ${HISTORY_DIR}. Run \`bun run test:report\` first.`);
  process.exit(2);
}

const argMsgIdx = process.argv.indexOf("--message");
const message = argMsgIdx > 0
  ? process.argv[argMsgIdx + 1]
  : `allure history ${new Date().toISOString()}`;

function run(cmd: string, args: string[], opts: { cwd?: string } = {}): { status: number; stdout: string } {
  const r = spawnSync(cmd, args, { encoding: "utf8", ...opts });
  return { status: r.status ?? 1, stdout: (r.stdout ?? "") + (r.stderr ?? "") };
}

// 1. Ensure the orphan branch exists.
const branchCheck = run("git", ["show-ref", "--verify", `refs/heads/${BRANCH}`]);
if (branchCheck.status !== 0) {
  console.log(`Creating orphan branch ${BRANCH}…`);
  // Create an orphan branch with an empty initial commit, out-of-tree.
  const tmp = path.resolve(`.allure-history-init-${process.pid}`);
  mkdirSync(tmp, { recursive: true });
  try {
    const r1 = run("git", ["worktree", "add", "--detach", tmp]);
    if (r1.status !== 0) throw new Error(`worktree add: ${r1.stdout}`);
    const r2 = run("git", ["switch", "--orphan", BRANCH], { cwd: tmp });
    if (r2.status !== 0) throw new Error(`switch --orphan: ${r2.stdout}`);
    const r3 = run("git", ["commit", "--allow-empty", "-m", "allure-history: initial orphan commit"], { cwd: tmp });
    if (r3.status !== 0) throw new Error(`initial commit: ${r3.stdout}`);
  } finally {
    run("git", ["worktree", "remove", "--force", tmp]);
    if (existsSync(tmp)) rmSync(tmp, { recursive: true, force: true });
  }
}

// 2. Check out the orphan branch into a sibling worktree.
if (existsSync(WORKTREE)) {
  run("git", ["worktree", "remove", "--force", WORKTREE]);
  if (existsSync(WORKTREE)) rmSync(WORKTREE, { recursive: true, force: true });
}
const add = run("git", ["worktree", "add", WORKTREE, BRANCH]);
if (add.status !== 0) {
  console.error(`worktree add failed: ${add.stdout}`);
  process.exit(1);
}

// 3. Replace the worktree's content with the latest HISTORY_DIR.
for (const f of readdirSync(WORKTREE)) {
  if (f === ".git") continue;
  rmSync(path.join(WORKTREE, f), { recursive: true, force: true });
}
cpSync(HISTORY_DIR, WORKTREE, { recursive: true });

// 4. Commit + publish.
const addAll = run("git", ["add", "-A"], { cwd: WORKTREE });
if (addAll.status !== 0) {
  console.error(`git add -A failed: ${addAll.stdout}`);
  process.exit(1);
}
const status = run("git", ["status", "--porcelain"], { cwd: WORKTREE });
if (status.stdout.trim() === "") {
  console.log("no history changes to publish");
} else {
  const commit = run("git", ["commit", "-m", message], { cwd: WORKTREE });
  if (commit.status !== 0) {
    console.error(`commit failed: ${commit.stdout}`);
    process.exit(1);
  }
  console.log(commit.stdout.trim());
}

// 5. Keep the worktree around for next time (faster — no re-add).
console.log(`worktree retained at ${WORKTREE} (gitignored)`);
console.log(`branch: ${BRANCH}`);
