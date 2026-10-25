#!/usr/bin/env node
// Maps in Basecamp: the view's surface arrives through Logos.Surface
// (docs/architecture.md section 16.6 as realised on the platform, RFC-3..5).
//
// The maps_view backend runs in its own ui-host --gui process as a client of
// Basecamp's nested compositor; this test looks at the host side only: the
// Surface item in Main.qml binds, reports a Wayland transport, carries the
// view id the backend registered with maps_core, and the tab shows frames.
//
// Usage:
//   nix run .#ui-tests                    # Basecamp dev --test (offscreen host)
//   node tests/basecamp-surface.mjs       # against a running Basecamp with the
//                                         # inspector on QML_INSPECTOR_PORT
// Environment:
//   LOGOS_QT_MCP        logos-qt-mcp checkout or package (framework.mjs)
//   MAPS_SURFACE_SHOT   write a PNG of the Basecamp window here when set
//
// Self-contained on purpose: the module builder copies each tests/*.mjs into
// the store alone.

import { writeFileSync } from "node:fs";
import { resolve } from "node:path";

const root = process.env.LOGOS_QT_MCP || new URL("../result-mcp", import.meta.url).pathname;
const { test, run } = await import(resolve(root, "test-framework/framework.mjs"));

async function find(app, name) {
  const res = await app.findByProperty("objectName", name);
  if (res.error || !res.matches?.length) throw new Error(`no object named "${name}"`);
  return res.matches[0].id;
}

async function props(app, id) {
  const res = await app.getProperties(id);
  if (res.error) throw new Error(`getProperties: ${res.error}`);
  return Object.fromEntries(res.properties.map((p) => [p.name, p.value]));
}

async function waitProp(app, name, property, check, { timeout = 60000 } = {}) {
  let last;
  await app.waitFor(async () => {
    last = (await props(app, await find(app, name)))[property];
    if (!check(last)) throw new Error("not yet");
  }, { timeout, interval: 500, description: `${name}.${property} (last: ${JSON.stringify(last)})` });
  return last;
}

test("maps: Logos.Surface binds the backend's surface", async (app) => {
  // The item exists as soon as the tab's QML loads; `ready` follows the
  // first frame the backend commits.
  await waitProp(app, "maps.surface", "module", (v) => v === "maps_view", { timeout: 90000 });
  await waitProp(app, "maps.surface", "ready", (v) => v === true, { timeout: 120000 });

  const p = await props(app, await find(app, "maps.surface"));
  if (!/^wayland-(shm|dmabuf)$/.test(p.transport))
    throw new Error(`transport should be a Wayland one, got ${JSON.stringify(p.transport)}`);
  if (!String(p.viewId).startsWith("maps_view/basecamp/main/"))
    throw new Error(`viewId should be maps_view/basecamp/main/<n>, got ${JSON.stringify(p.viewId)}`);
  if (!(p.width > 0 && p.height > 0))
    throw new Error(`surface has no size: ${p.width}x${p.height}`);
  console.log(`  surface ${p.viewId} via ${p.transport}, ${p.width}x${p.height}`);
});

test("maps: the host window can be captured with the surface in it", async (app) => {
  const shot = await app.screenshot();
  if (shot.error) throw new Error(`screenshot: ${shot.error}`);
  if (!(shot.width > 0 && shot.height > 0 && shot.image))
    throw new Error("empty screenshot");
  if (process.env.MAPS_SURFACE_SHOT) {
    writeFileSync(process.env.MAPS_SURFACE_SHOT, Buffer.from(shot.image, "base64"));
    console.log(`  wrote ${process.env.MAPS_SURFACE_SHOT} (${shot.width}x${shot.height})`);
  }
});

run();
