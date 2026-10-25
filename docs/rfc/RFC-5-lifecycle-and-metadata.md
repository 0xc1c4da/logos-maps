# RFC-5: `view_surface` metadata key and surface lifecycle

**Repositories:** `logos-module-builder`, `logos-basecamp` · **Status:** implemented (workspace, see §Status) · **Depends on:** RFC-1..4

## Part A — `metadata.json`: `view_surface`

### Problem

Hosts need to know, before spawning, that a `ui_qml` module's backend wants a GUI process
and a compositor socket (RFC-1/2). Nothing in `metadata.json` says so; today a backend
is spawned as a plain `QCoreApplication` by construction.

### Proposal

A new optional top-level key, valid only for `type: "ui_qml"` modules with a `main`:

```json
{
  "name": "maps_view",
  "type": "ui_qml",
  "main": "maps_view_plugin",
  "view": "qml/Main.qml",
  "view_surface": true,
  ...
}
```

- **Builder** (`lib/parseMetadata.nix`): parse `view_surface` as a boolean (default
  `false`), throw if set on a non-`ui_qml` module or on a `ui_qml` module without `main`
  (a QML-only view has no process to render in). Not overlay-variable under `platforms`
  (the same rule as `type`, `view`, `main`). Emit it into the resolved `metadata.json`
  that `mkCombined` installs, so the installed record carries it.
  Document in `docs/configuration.md` next to `view` and in the `ui-qml-backend`
  template as a commented-out line.
- **Hosts**: read `view_surface` from `<installDir>/metadata.json` at load time — the same
  file `IntentRegistry` already reads (`logos-basecamp/app/IntentRegistry.cpp:23–46`) —
  and pass `ViewSpawnOptions{gui = true, surfaceSocket = compositor->socketName()}` to
  `ViewModuleHost::spawn` (RFC-2). The package manager module is untouched; it keeps
  forwarding whatever keys it already forwards.
- **Unknown-key behaviour** is preserved: an old builder passes the key through on `_raw`
  and an old host ignores it. The module then runs with `surface.transport: none`.

## Part B — Lifecycle

A surface-bearing view has four states observed by the host and reported to the module
through the generic contract the module already has (its `.rep` replica and the
`Logos.Surface` item):

| State | Host observes | Module observes |
|---|---|---|
| **spawned** | `ViewModuleHost::ready()` with `surfaceToken()` | `initLogos` done; may open its window |
| **ready** | toplevel bound (RFC-4) and configured to item size | first `configure`; frame callbacks flowing |
| **suspended** | tab inactive or item invisible | no frame callbacks; `wl_surface.frame` never fires. The client must not busy-render (Qt's scene graph already blocks on frame callbacks) |
| **closed** | module unload / tab close | `forget(token)` → `xdg_toplevel.close` → `aboutToUnload` → SIGTERM after grace |

Rules:

1. The host MUST stop frame callbacks for invisible surfaces (RFC-4 §3). This is the
   only pacing mechanism; no "pause" RPC is added.
2. Resize is host-driven: `sendConfigure` with the item's logical size; the client acks
   and attaches a buffer of `size × scale`. The host never scales a buffer.
3. On DPR change (window moved between screens) the output `scaleFactor` changes and a
   configure is sent; the client re-renders at the new physical size.
4. Unload order in `UIPluginManager` is: `forget(token)` → remove dock/QML →
   `ViewModuleHost::stop()`; the compositor therefore never sees an unexpected disconnect
   for a module the user closed.
5. If the client exits on its own, `Logos.Surface` reports `error("clientExited")` and
   the host's existing crash handling for `ui-host` applies (`processExited`).

## Compatibility

A `ui_qml` module without `view_surface` is unaffected by every rule above.

## Status

- **Part A** implemented in `logos-module-builder` (`6a3994f`): `lib/parseMetadata.nix`
  validates `view_surface` (boolean, `ui_qml` with `main` only, not a platform overlay)
  and carries it into the resolved metadata; documented in `docs/configuration.md`,
  `templates/ui-qml-backend/metadata.json` shows the key; 8 assertions in
  `tests/test-parse-metadata.nix`.
- **Part B** implemented in `logos-basecamp` at `37c9a57` (branch `rfc/view-surface`, M6c). `UIPluginManager` reads
  `view_surface` from the installed module's `metadata.json`; `PluginLoader` spawns with
  `gui = true` and the compositor socket, registers `expect()` right after `spawn()`
  (the token is known from spawn — RFC-2 deviation), and `stopViewModuleHost` does
  `forget()` before `stop()` on both unload paths (rule 4). Rules 1–3 and 5 are
  exercised by `tests/view_surface_test.cpp`: no frame callbacks while invisible, host-
  driven resize at scale, buffer at DPR 2, `clientExited` on exit. Rule 3's live DPR
  change is implemented (`screenChanged` → output `scaleFactor`) but not covered by a
  test; the offscreen platform has one screen.
- The **spawned** state's "may open its window" is what `maps_view` does when the
  compositor is unavailable; under Basecamp the window is the surface.
