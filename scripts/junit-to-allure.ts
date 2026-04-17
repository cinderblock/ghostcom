#!/usr/bin/env bun
/**
 * Converts JUnit XML files in allure-results/ to Allure-native JSON
 * result files that `allure generate` can consume.
 *
 * Bun's JUnit reporter produces standard JUnit XML; Allure requires its
 * own {uuid}-result.json format. This bridge makes the two work together.
 */
import { readdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { randomUUID } from "node:crypto";
import { createHash } from "node:crypto";
import path from "node:path";

interface TestResult {
  uuid: string;
  historyId: string;
  name: string;
  fullName: string;
  status: "passed" | "failed" | "broken" | "skipped" | "unknown";
  statusDetails?: { message?: string; trace?: string };
  stage: "finished";
  labels: Array<{ name: string; value: string }>;
  links: never[];
  parameters: never[];
  start: number;
  stop: number;
}

function parseTimeMs(s: string | undefined): number {
  return s ? Math.round(parseFloat(s) * 1000) : 0;
}

function historyId(suite: string, name: string): string {
  return createHash("md5").update(`${suite}#${name}`).digest("hex");
}

/** Crude XML attribute extractor — works for well-formed JUnit XML. */
function attr(tag: string, name: string): string | undefined {
  const re = new RegExp(`${name}\\s*=\\s*"([^"]*)"`, "i");
  const m = tag.match(re);
  return m?.[1];
}

export function convertJunitDir(resultsDir: string): number {
  if (!existsSync(resultsDir)) return 0;

  const xmlFiles = readdirSync(resultsDir).filter(f => f.endsWith(".xml"));
  let converted = 0;
  const now = Date.now();

  for (const xmlFile of xmlFiles) {
    const xml = readFileSync(path.join(resultsDir, xmlFile), "utf8");

    // Extract the file-level suite name (from the outer <testsuite> with a file= attr).
    const fileSuiteMatch = xml.match(/<testsuite[^>]+file\s*=\s*"([^"]+)"/);
    const fileLabel = fileSuiteMatch?.[1] ?? xmlFile.replace(/\.xml$/, "");

    // Find all <testcase> elements (self-closing or with body for failure).
    const testcaseRegex = /<testcase\s[^>]*?(?:\/>|>[\s\S]*?<\/testcase>)/g;
    let m: RegExpExecArray | null;

    while ((m = testcaseRegex.exec(xml)) !== null) {
      const tc = m[0];
      const name = attr(tc, "name") ?? "unknown";
      const classname = attr(tc, "classname") ?? "";
      const time = parseTimeMs(attr(tc, "time"));

      // Determine status.
      let status: TestResult["status"] = "passed";
      let statusDetails: TestResult["statusDetails"] | undefined;

      // Match both self-closing <failure .../> and <failure ...>body</failure>
      const failMatch = tc.match(/<failure[^>]*?(?:message\s*=\s*"([^"]*)")?[^>]*?(?:\/>|>([\s\S]*?)<\/failure>)/);
      const errorMatch = tc.match(/<error[^>]*?(?:message\s*=\s*"([^"]*)")?[^>]*?(?:\/>|>([\s\S]*?)<\/error>)/);
      const skipped = /<skipped/.test(tc);

      if (failMatch) {
        status = "failed";
        const failType = attr(failMatch[0], "type") ?? "";
        statusDetails = {
          message: failMatch[1] ?? failType ?? "assertion failed",
          trace: failMatch[2]?.trim() ?? "",
        };
      } else if (errorMatch) {
        status = "broken";
        const errType = attr(errorMatch[0], "type") ?? "";
        statusDetails = {
          message: errorMatch[1] ?? errType ?? "error",
          trace: errorMatch[2]?.trim() ?? "",
        };
      } else if (skipped) {
        status = "skipped";
      }

      const uuid = randomUUID();
      const result: TestResult = {
        uuid,
        historyId: historyId(classname, name),
        name,
        fullName: classname ? `${classname} > ${name}` : name,
        status,
        stage: "finished",
        labels: [
          { name: "suite", value: classname || fileLabel },
          { name: "parentSuite", value: fileLabel },
          { name: "host", value: "ghostcom-test-vm" },
          { name: "thread", value: "main" },
          { name: "framework", value: "bun:test" },
          { name: "language", value: "typescript" },
        ],
        links: [],
        parameters: [],
        start: now - time,
        stop: now,
      };
      if (statusDetails) result.statusDetails = statusDetails;

      writeFileSync(
        path.join(resultsDir, `${uuid}-result.json`),
        JSON.stringify(result, null, 2),
      );
      converted++;
    }
  }

  return converted;
}

// Run directly: `bun run scripts/junit-to-allure.ts [results-dir]`
if (import.meta.main) {
  const dir = process.argv[2] || path.resolve("allure-results");
  const n = convertJunitDir(dir);
  console.log(`converted ${n} test cases from JUnit XML → Allure JSON in ${dir}`);
}
