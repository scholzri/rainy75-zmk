// Runs the config page's data-core sections and tests.js under Node, without
// a browser: node web/config/test-node.mjs (Node 18 or later). The UI tests
// need a browser and are skipped here; test.html runs everything.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const dir = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(dir, "index.html"), "utf8");
const ctx = vm.createContext({
  console, setTimeout, clearTimeout, TextEncoder, TextDecoder, atob, btoa,
  ReadableStream, WritableStream, EventTarget, Event, DOMException,
});
for (const [, id, code] of html.matchAll(/<script data-core id="([\w-]+)">([\s\S]*?)<\/script>/g)) {
  vm.runInContext(code, ctx, { filename: `index.html#${id}` });
}
vm.runInContext(readFileSync(join(dir, "tests.js"), "utf8"), ctx, { filename: "tests.js" });
const r = await ctx.RainyTestRun(ctx.RainyCore, { html, bytes: Buffer.byteLength(html), browser: false });
for (const line of r.lines) console.log(line);
console.log(`${r.passed} passed, ${r.failed} failed, ${r.skipped} skipped (browser only)`);
process.exitCode = r.failed ? 1 : 0;
