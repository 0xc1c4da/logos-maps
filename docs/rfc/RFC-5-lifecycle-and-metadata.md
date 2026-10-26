# RFC-5: `view_surface` metadata key, surface lifecycle, policy and isolation

**Repositories:** `logos-module-builder`, `logos-basecamp` · **Status:** implemented, rev 2 (Parts C and D added; see §Status) · **Depends on:** RFC-1..4; rev 2 also RFC-6, RFC-7

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

Rules 1–5 are stated for the Wayland transport; under the `channel` transport (RFC-7)
the same states exist with these mappings: *ready* = first `FrameReady` painted;
*suspended* = no `FrameRequest` is granted while invisible (RFC-7 §5); rule 2's
`sendConfigure` is RFC-7 `Configure` with a new generation; rule 3's scale change is a
`Configure` carrying the new `dpr` and pixel size; rule 4's `forget(token)` is
`SurfaceHostSession::close()` before `ViewModuleHost::stop()`.

## Part C — Policy: who may have a surface (rev 2)

### Today

`PluginLoader::spawnOptionsFor` grants a transport to every module whose installed
`metadata.json` says `view_surface: true`. That is a *declaration* by the package, not
an *authorisation* by the platform. Rev 2 makes the decision a single, replaceable
function so that the authorisation can be added without touching the loader again:

```cpp
// app/surface/SurfacePolicy.h
struct SurfaceGrant {
    bool allowed = false;          // spawn with a transport at all
    QString transport;             // RFC-6 §3 family, after SurfaceTransportSelector
    QString denialReason;          // for the log and Logos.Surface error("denied")
};
SurfaceGrant SurfacePolicy::decide(const PluginLoadRequest& request);
```

Default behaviour (this revision): `allowed = request.viewSurface`, transport from the
selector. A denied module is spawned with plain `--gui` and its `Logos.Surface` reports
`error("denied")` with `transport: "none"`.

### The capability gate (specified, not implemented)

A view surface is a privilege — a process gets to draw into the host's window and to
receive the user's raw input for that region — and in Logos privileges are
**capabilities** brokered by `capability_module` and the host's consent flow
(`logos-basecamp/docs/spec.md:257` for how an app requests a capability without naming
its provider; `logos-liblogos/docs/spec.md:272` for how `capability_module` refuses to
mint a token for a caller outside an access policy). The gate is specified as follows
and is to be implemented when `capability_module` grows a host-queryable capability
registry:

1. The package declares the capability it needs, `"capabilities": ["view_surface"]`,
   next to `"view_surface": true` (the builder validates that the two agree).
2. At load, `SurfacePolicy::decide` asks the capability broker whether *this* module may
   hold `view_surface` for *this* user. The first time, the host shows the consent it
   shows for other capabilities ("Maps wants to draw into Basecamp"); the answer is
   remembered per module and revocable from the host's module management view.
3. Denial is final for that spawn: no transport, `error("denied")`. Grant is per spawn:
   the token minted in RFC-2 is the proof the compositor/session checks, so revocation
   takes effect at the next load (or immediately via `forget(token)`).
4. The decision is logged with the module's trusted package name (not its self-declared
   `name`; `logos-liblogos/docs/spec.md:163`).

Nothing in RFC-1..4, 6, 7 or 8 changes for the gate: it only decides whether
`ViewSpawnOptions.surfaceTransport` is set.

## Part D — Isolation of the view process (recorded; out of scope here)

RFC-6 §1 claims the fault-isolation tier. A module written to attack the host needs the
view process confined by the OS before module code runs, which no RFC in this set
implements. Recorded so that the confinement work starts from the right place:

| OS | Confinement to add to the `ui-host --gui` launch | Interaction with the transports |
|---|---|---|
| Linux | `bubblewrap` (as Qt Application Manager's optional container does): new mount/PID/user/network namespaces, read-only runtime and module paths, private `XDG_RUNTIME_DIR` holding only the nested socket (RFC-4) or the channel socket (RFC-7), no session bus, no real `WAYLAND_DISPLAY`/`DISPLAY`/X11 sockets, seccomp, optional render-node allowlist | `wayland` keeps working (the socket is bind-mounted); `channel-shm` needs the `QSharedMemory` key reachable — SysV keys cross namespaces, POSIX names need a shared `/dev/shm`; prefer passing a sealed memfd over the socket instead (RFC-7 §7 residual) |
| Windows | restricted token + job object + process mitigations, then AppContainer/LPAC once the D3D11 path is tested under it; duplicate only the slot handles | `channel-d3d11` handles are duplicated by the host, so no extra rights are needed; the named pipe gets an explicit DACL |
| macOS | a signed XPC service with its own minimal App Sandbox entitlements rather than a plain child (a child inherits its parent's policy and cannot be stricter); Hardened Runtime; `disable-library-validation` in the helper only if third-party dylibs require it | `channel-iosurface` moves to XPC-transferred `IOSurface` objects (`IOSurfaceCreateXPCObject`), replacing the bootstrap rendezvous |
| Android / iOS | in-process (RFC-6 §4); stronger: Android `isolatedProcess` bound service + `SurfaceControlViewHost` (API 30, input transfer API 35); iOS 26 Enhanced Security helper extensions for parsing only (no UI) | separate investigation; the module API is unchanged either way |

Also part of that tier, and not here: a bounded replacement for QtRO's `QDataStream`
decoding on the module control channel (Qt documents it as unsafe against malformed
peers), and running a module's *QML* outside the host (Qt documents untrusted QML as
unsupported as a security boundary). The surface transports are designed so none of
this changes them: confinement wraps the process; the protocol and the slots stay.

## Compatibility

A `ui_qml` module without `view_surface` is unaffected by every rule above. Part C's
default policy reproduces rev 1 behaviour exactly.

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
- **Rev 2, Part C** implemented in `logos-basecamp` on `rfc/view-surface`:
  `app/surface/SurfacePolicy.{h,cpp}` is the single decision point
  (`decide(PluginLoadRequest) → SurfaceGrant{allowed, transport, shmOnly, denialReason}`),
  called from `PluginLoader::spawnOptionsFor`; `app/surface/SurfaceTransportSelector`
  picks the family (`channel` by default on every platform, `wayland` when the compositor offers GPU buffers — RFC-6 §3 rev 2.1 —, `LOGOS_SURFACE_TRANSPORT`, per-module record). The
  grant is still "the package declares `view_surface`"; the capability gate
  (`"capabilities": ["view_surface"]` through `capability_module`, consent, revocation)
  is specified here and its hook points are commented in `SurfacePolicy.cpp` — it is
  **not implemented**, because `capability_module` does not yet exist on this stack.
  `Logos.Surface` reports `denied` for a refused grant. Rules 1–5 hold on the `channel`
  transport too (`tests/surface_channel_test.cpp`): host-driven size/DPR via
  `Configure`, no credits while invisible, `clientExited` with the last frame kept,
  `forget()` before stop. Part A gained the RFC-8 include path in the builder
  (`lib/buildCppPlugin.nix`, `LOGOS_VIEW_SURFACE_INCLUDE_DIR`). **Part D** (isolation
  record) is unchanged: documentation only.
