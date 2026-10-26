# Platform RFCs: out-of-process view surfaces

These eight proposals are what `docs/architecture.md` §3 asks of the Logos platform so
that a `ui_qml` module can render GPU content in **its own process** and have the host
composite it into a tab — without any module code being loaded into the host, on
Linux, Windows, macOS, iOS and Android. They are written against the repositories and
line numbers in the spec's alias table (§0.2) and are implemented and tested in this
workspace first (branches `rfc/view-surface` in each repository), then offered upstream.

| RFC | Repository | Summary | Status |
|---|---|---|---|
| [RFC-1](RFC-1-view-surface-ui-host.md) | `logos-view-module-runtime` | `ui-host --gui`: a `QGuiApplication` process for modules that declare `view_surface` | implemented at `ee5eefb` |
| [RFC-2](RFC-2-surface-handshake.md) | `logos-view-module-runtime` | Surface handshake: `--surface-socket`, `app_id` token, `READY ... SURFACE <token>`, `ViewModuleHost::surfaceToken()` | implemented at `ee5eefb` |
| [RFC-3](RFC-3-logos-surface-qml.md) | `logos-basecamp` | `Logos.Surface` host QML type available to sandboxed app QML | implemented at `37c9a57` |
| [RFC-4](RFC-4-nested-compositor.md) | `logos-basecamp` | Nested Wayland compositor (`SurfaceCompositor`); rev 2 hardens it: guarded `wl_shm` reads (Qt patch), global allowlist with GPU buffers as policy, bounds, focus hygiene | implemented at `37c9a57`; rev 2 on `rfc/view-surface` (see table below) |
| [RFC-5](RFC-5-lifecycle-and-metadata.md) | `logos-module-builder`, `logos-basecamp` | `view_surface` metadata key; lifecycle; rev 2 adds `SurfacePolicy`, the capability gate (specified) and the isolation record | implemented at `6a3994f` (builder) + `37c9a57` (Basecamp); rev 2 on `rfc/view-surface` |
| [RFC-6](RFC-6-other-transports.md) | platform | **The `SurfaceTransport` contract** (a–e), per-OS selection, the in-process `ViewContainer` for iOS/Android | normative (rev 2.1: `channel` is the default on every platform; `wayland` only with GPU buffers) |
| [RFC-7](RFC-7-channel-transport.md) | `logos-view-module-runtime`, `logos-basecamp` | The `channel` transport: host-allocated slot ring (`shm` / `d3d11` / `iosurface`) over a bounded protocol on a local socket, credits, value-based input | implemented for `shm`; `d3d11`/`iosurface` written, not compiled here |
| [RFC-8](RFC-8-client-api.md) | `logos-view-module-runtime`, `logos-module-builder` | `LogosViewSurface::window()`: the one window a module renders into on every transport | implemented |

Design invariants every RFC preserves (spec M3.1–M3.3):

1. No module QML plugin, C++ or shared library is loaded into the host process. The
   sandbox rules of `QmlSandbox.cpp` and `RestrictedUrlInterceptor.h` are unchanged.
2. The module's existing contract is untouched: a `ui_qml` module without
   `view_surface` builds and runs byte-for-byte as before.
3. Everything added is generic. `Logos.Surface` knows nothing about maps; a video
   player, a 3D viewer or a terminal module uses it identically.

Vocabulary: a **view module** is a `ui_qml` module; its **backend** runs in `ui-host`;
a **surface** is the GPU buffer the backend renders and the host shows; the **transport**
moves it (RFC-7: channel, the default everywhere; RFC-4: Wayland, Linux with GPU buffers); a **slot** is one host-allocated frame buffer
of the channel transport; a **generation** is one host-authoritative geometry
(size, DPR) of a surface.

Assurance claimed by the set: **fault isolation** — a crashing or misbehaving module
cannot crash, block or exhaust the host (RFC-6 §1 e). Not claimed: safety against a
hostile module; what that needs is recorded in RFC-5 rev 2 §Part D.

## Branches and the PRs to open

Each repository has a local branch `rfc/view-surface` at the commit the maps tests
were run against (spec §16.6/§16.7, `view/tests/basecamp-surface.mjs`, `view/tests/logosctl/e2e.sh`):

| Repository | `rfc/view-surface` | Upstream base | RFC commits | PR |
|---|---|---|---|---|
| `logos-view-module-runtime` | `ee5eefb` | `a04b463` (main) | `f8f9424` (RFC-1, RFC-2), `ee5eefb` (RFC-2 amendments) | one PR, two commits |
| `logos-module-builder` | `6a3994f` | `8f56bf8` (main) | `6a3994f` (RFC-5 part A) | one PR; the branch also carries three unrelated local commits (`46f4990`, `8b6d681`, `6e3f7a5`, nim packaging) that sit below it — `6a3994f` cherry-picks cleanly onto `8f56bf8`, so the PR can be opened from a rebased copy |
| `logos-basecamp` | `37c9a57` | `572f722` (main) | `37c9a57` (RFC-3, RFC-4, RFC-5 part B) | one PR; the branch sits on `5454a52` (dev build takes its GL stack from the closure), which is a prerequisite for running the dev launcher with GL at all and should land first or be included — `37c9a57` cherry-picks cleanly onto `572f722` |

Order: runtime first (RFC-1/2 are what Basecamp links against), builder next (RFC-5 A
is independent and tiny), Basecamp last with its `logos-view-module-runtime` input
bumped to the merged runtime commit. Nothing is pushed from this workspace; the
branches are local.

What was verified before offering them (all on Qt 6.9.2 from the SDK's nixpkgs, Linux):

- runtime `unit-tests` (handshake, `--gui`, token), builder `tests/test-parse-metadata.nix`
  (`view_surface`), Basecamp `unit-tests` incl. `tests/view_surface_test.cpp`.
- `maps_view` in Basecamp offscreen (`nix run view#ui-tests`) and live on a Wayland
  desktop: surface bound, `wayland-shm`, frames visible, teardown clean.
- Not verified: `wayland-dmabuf` (software GL only on this host, spec O11), macOS/Windows
  (RFC-6 is informative), live DPR change (RFC-5 rule 3).
