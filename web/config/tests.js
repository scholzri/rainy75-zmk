/*
 * Tests of the config page (index.html): its data-core sections under Node
 * (test-node.mjs) and in the browser (test.html), plus UI tests on
 * index.html?demo that only the browser runs. Each test gets the page's
 * RainyCore object (C) and env: {html, bytes, browser, frames}.
 */
"use strict";
(() => {
  const tests = [];
  const test = (name, fn) => tests.push({ name, fn, ui: false });
  const uiTest = (name, fn) => tests.push({ name, fn, ui: true });
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const settle = () => sleep(0);

  function same(a, b) {
    if (a === b) return true;
    if (ArrayBuffer.isView(a) && ArrayBuffer.isView(b)) {
      return a.length === b.length && Array.prototype.every.call(a, (x, i) => x === b[i]);
    }
    if (Array.isArray(a) || Array.isArray(b)) {
      return Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((x, i) => same(x, b[i]));
    }
    if (a && b && typeof a === "object" && typeof b === "object") {
      const ka = Object.keys(a);
      const kb = Object.keys(b);
      return ka.length === kb.length && ka.every((k) => Object.prototype.hasOwnProperty.call(b, k) && same(a[k], b[k]));
    }
    return false;
  }
  const show = (v) => {
    try {
      return JSON.stringify(v, (k, x) => (ArrayBuffer.isView(x) ? Array.from(x) : x));
    } catch (e) {
      return String(v);
    }
  };
  const t = {
    eq(a, b, msg) {
      if (!same(a, b)) throw new Error((msg ? msg + ": " : "") + "got " + show(a) + ", expected " + show(b));
    },
    ok(v, msg) {
      if (!v) throw new Error(msg || "expected a true value");
    },
    throws(fn, re) {
      try {
        fn();
      } catch (e) {
        if (re && !re.test(e.message)) throw new Error("wrong error: " + e.message);
        return e;
      }
      throw new Error("expected an error");
    },
    async rejects(p, re) {
      try {
        await p;
      } catch (e) {
        if (re && !re.test(e.message)) throw new Error("wrong error: " + e.message);
        return e;
      }
      throw new Error("expected a rejection");
    },
  };
  const bytes = (...a) => Uint8Array.from(a.flatMap((x) => (ArrayBuffer.isView(x) ? Array.from(x) : x)));
  const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
  const fromHex = (s) => Uint8Array.from(s.match(/../g) || [], (x) => parseInt(x, 16));
  const ascii = (s) => Uint8Array.from(s, (ch) => ch.charCodeAt(0));

  // ---- Task 1: the file's rules, CBOR, SMP header ----

  test("static: one file under 100000 bytes", (C, env) => {
    t.ok(env.bytes < 100000, "index.html is " + env.bytes + " bytes");
  });

  test("static: CSP meta blocks the network, first after charset", (C, env) => {
    const m = /<meta charset="utf-8">\s*<meta http-equiv="Content-Security-Policy" content="([^"]+)">/.exec(env.html);
    t.ok(m, "CSP meta tag right after the charset");
    t.ok(m[1].startsWith("default-src 'none'; "), "default-src 'none' first");
    t.ok(!/connect-src/.test(m[1]), "no connect-src: default-src 'none' covers it");
  });

  test("static: nothing loaded from anywhere", (C, env) => {
    t.ok(!/<script[^>]*\ssrc=/i.test(env.html), "no script src");
    t.ok(!/<link[^>]*rel="stylesheet"/i.test(env.html), "no stylesheet link");
    t.ok(!/@import|url\(/.test(env.html), "no CSS import or url()");
    t.ok(!/\bfetch\(|XMLHttpRequest|WebSocket|EventSource|sendBeacon/.test(env.html), "no network API");
    for (const m of env.html.matchAll(/https?:\/\/[^\s"'<>]*/g)) {
      const before = env.html.slice(m.index - 6, m.index);
      t.ok(before === 'href="' && m[0].startsWith("https://github.com/scholzri/rainy75-zmk/"), "only links to the repository: " + m[0]);
    }
  });

  test("static: no em or en dashes", (C, env) => {
    const dashes = new RegExp("[" + String.fromCharCode(0x2013, 0x2014) + "]");
    t.ok(!dashes.test(env.html), "found an em or en dash");
  });

  test("CBOR: encodes scalars as rainy75_cfg.py does", (C) => {
    t.eq(hex(C.cborEncode(true)), "f5");
    t.eq(hex(C.cborEncode(false)), "f4");
    t.eq(hex(C.cborEncode(null)), "f6");
    t.eq(hex(C.cborEncode(5)), "05");
    t.eq(hex(C.cborEncode(24)), "1818");
    t.eq(hex(C.cborEncode(300)), "19012c");
    t.eq(hex(C.cborEncode(0xffffff)), "1a00ffffff");
    t.eq(hex(C.cborEncode(-1)), "20");
    t.eq(hex(C.cborEncode("ab")), "626162");
    t.eq(hex(C.cborEncode(bytes(1, 2))), "420102");
  });

  test("CBOR: encodes lists and maps (definite length)", (C) => {
    t.eq(hex(C.cborEncode(["a", "b"])), "8261616162");
    t.eq(hex(C.cborEncode([])), "80");
    t.eq(hex(C.cborEncode(new Array(24).fill("a")).subarray(0, 2)), "9818");
    t.eq(hex(C.cborEncode({})), "a0");
    t.eq(hex(C.cborEncode({ k: "rgb.on", v: true })), "a2616b667267622e6f6e6176f5");
  });

  test("CBOR: indefinite-length output for the simulator", (C) => {
    t.eq(hex(C.cborEncode({ s: [1] }, { indefinite: true })), "bf61739f01ffff");
  });

  test("CBOR: refuses what the protocol never sends", (C) => {
    t.throws(() => C.cborEncode(1.5), /not an integer/);
    t.throws(() => C.cborEncode(2 ** 32), /out of range/);
    t.throws(() => C.cborEncode(undefined), /cannot encode/);
  });

  test("CBOR: decodes definite and indefinite containers", (C) => {
    t.eq(C.cborDecode(fromHex("a26272630062737681f5")), { rc: 0, sv: [true] });
    t.eq(C.cborDecode(fromHex("bf627263006273769ff5f4ffff")), { rc: 0, sv: [true, false] });
    t.eq(C.cborDecode(fromHex("9f9f01ff9f02ffff")), [[1], [2]]);
    t.eq(C.cborDecode(fromHex("bfff")), {});
  });

  test("CBOR: decodes every integer width, strings, bytes, null", (C) => {
    t.eq(C.cborDecode(fromHex("17")), 23);
    t.eq(C.cborDecode(fromHex("18ff")), 255);
    t.eq(C.cborDecode(fromHex("19ffff")), 65535);
    t.eq(C.cborDecode(fromHex("1affffffff")), 4294967295);
    t.eq(C.cborDecode(fromHex("1b001fffffffffffff")), 9007199254740991);
    t.eq(C.cborDecode(fromHex("38ff")), -256);
    t.eq(C.cborDecode(fromHex("6668c3a46c6c6f")), "h" + String.fromCharCode(0xe4) + "llo");
    t.eq(C.cborDecode(fromHex("43010203")), bytes(1, 2, 3));
    t.eq(C.cborDecode(fromHex("f6")), null);
  });

  test("CBOR: round trip of a list reply", (C) => {
    const v = { rc: 0, s: [["rgb.on", "b", null, null, 0], ["rgb.val", "u", 16, 255, 0]], next: 2 };
    t.eq(C.cborDecode(C.cborEncode(v)), v);
    t.eq(C.cborDecode(C.cborEncode(v, { indefinite: true })), v);
  });

  test("CBOR: rejects broken input", (C) => {
    t.throws(() => C.cborDecode(fromHex("82f5")), /truncated/);
    t.throws(() => C.cborDecode(fromHex("bf6172")), /truncated/);
    t.throws(() => C.cborDecode(fromHex("f5f5")), /trailing/);
    t.throws(() => C.cborDecode(fromHex("c0f5")), /major type 6/);
    t.throws(() => C.cborDecode(fromHex("fa00000000")), /simple value 26/);
    t.throws(() => C.cborDecode(fromHex("7fff")), /indefinite-length string/);
    t.throws(() => C.cborDecode(fromHex("1bffffffffffffffff")), /too large/);
    t.throws(() => C.cborDecode(fromHex("62c328")), /./);
  });

  test("SMP: request header as restore_original.py builds it", (C) => {
    t.eq(hex(C.smpRequest(C.SMP.READ, 67, 0, 0, {})), "0000000100430000a0");
    t.eq(hex(C.smpRequest(C.SMP.WRITE, 67, 3, 255, { k: "rgb.on", v: false }).subarray(0, 8)), "0200000d0043ff03");
  });

  test("SMP: parses a reply frame", (C) => {
    const f = C.smpFrame(3, 67, 3, 7, C.cborEncode({ rc: 0, v: 120 }, { indefinite: true }));
    const m = C.smpParse(f);
    t.eq([m.op, m.group, m.cmd, m.seq, m.len], [3, 67, 3, 7, f.length - 8]);
    t.eq(m.body, { rc: 0, v: 120 });
    t.eq(C.smpParse(C.smpFrame(1, 1, 0, 0, new Uint8Array(0))).body, {});
    t.throws(() => C.smpParse(bytes(1, 0, 0, 5, 0, 67, 0, 0, 0xa0)), /shorter/);
    t.throws(() => C.smpParse(bytes(1, 0)), /too short/);
  });

  // end of tests

  async function runOne(fn, C, env) {
    let timer;
    const limit = new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error("timed out after 10 s")), 10000);
    });
    try {
      await Promise.race([Promise.resolve().then(() => fn(C, env)), limit]);
    } finally {
      clearTimeout(timer);
    }
  }

  globalThis.RainyTestRun = async (C, env) => {
    const lines = [];
    let passed = 0;
    let failed = 0;
    let skipped = 0;
    for (const { name, fn, ui } of tests) {
      if (ui && !env.browser) {
        skipped++;
        continue;
      }
      try {
        await runOne(fn, C, env);
        passed++;
        lines.push("ok - " + name);
      } catch (e) {
        failed++;
        lines.push("not ok - " + name + ": " + ((e && e.message) || e));
      }
    }
    return { lines, passed, failed, skipped };
  };
})();
