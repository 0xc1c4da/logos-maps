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
| [RFC-4](RFC-4-nested-compositor.md) | `logos-basecamp` | Nested Wayland compositor (`SurfaceCompositor`); rev 2 hardens it: guarded `wl_shm` reads (Qt patch), global allowlist with GPU buffers as policy, bounds, focus hygiene | implemented at `37c9a57`; rev 2 at `62c430f` (see table below) |
| [RFC-5](RFC-5-lifecycle-and-metadata.md) | `logos-module-builder`, `logos-basecamp` | `view_surface` metadata key; lifecycle; rev 2 adds `SurfacePolicy`, the capability gate (specified) and the isolation record | implemented at `6a3994f` (builder) + `37c9a57` (Basecamp); rev 2 at `f73b358` (Basecamp `SurfacePolicy`) |
| [RFC-6](RFC-6-other-transports.md) | platform | **The `SurfaceTransport` contract** (a–e), per-OS selection, the in-process `ViewContainer` for iOS/Android | normative (rev 2.1: `channel` is the default on every platform; `wayland` only with GPU buffers) |
| [RFC-7](RFC-7-channel-transport.md) | `logos-view-module-runtime`, `logos-basecamp` | The `channel` transport: host-allocated slot ring (`shm` / `d3d11` / `iosurface`) over a bounded protocol on a local socket, standing credits (§5 rev 2), value-based input | implemented for `shm` at `1232d57`..`01bf4ef` (runtime) and `f73b358` (Basecamp); `d3d11`/`iosurface` written, not compiled here |
| [RFC-8](RFC-8-client-api.md) | `logos-view-module-runtime`, `logos-module-builder` | `LogosViewSurface::window()`: the one window a module renders into on every transport | implemented at `1232d57` (runtime), `249eb33` (builder) |

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

| Repository | `rfc/view-surface` | Upstream base (`master`) | RFC commits | PR |
|---|---|---|---|---|
| `logos-view-module-runtime` | `2839d1a` ([0xc1c4da/logos-view-module-runtime](https://github.com/0xc1c4da/logos-view-module-runtime/tree/rfc/view-surface)) | `a04b463` | `f8f9424` (RFC-1, RFC-2), `ee5eefb` (RFC-2 amendments), `1232d57` (RFC-7, RFC-8: `LogosViewSurface` library, `d3d11`/`iosurface` skeletons), `b5f170a` (RFC-7: `ui-host --surface-transport channel`), `01bf4ef` (tests), `2839d1a` (RFC-7 §5 rev 2 standing credit) | [logos-co/logos-view-module-runtime#45](https://github.com/logos-co/logos-view-module-runtime/pull/45) (draft), six commits; `d3d11`/`iosurface` are marked unverified in the sources and the commit |
| `logos-module-builder` | `9461ab1` ([0xc1c4da/logos-module-builder](https://github.com/0xc1c4da/logos-module-builder/tree/rfc/view-surface)) | `8f56bf8` | `7ddd992` (RFC-5 part A; was `6a3994f`), `9461ab1` (RFC-8: runtime include dir and library for `view_surface` modules; was `249eb33`) | [logos-co/logos-module-builder#279](https://github.com/logos-co/logos-module-builder/pull/279) (draft), two commits, cut clean off `8f56bf8` (the nim-packaging commits that sat below the earlier branch are kept locally as `nim-packaging`) |
| `logos-basecamp` | `0025e6b` ([0xc1c4da/logos-basecamp](https://github.com/0xc1c4da/logos-basecamp/tree/rfc/view-surface)) | `66a747e` | `5d0aa21` (RFC-3, RFC-4, RFC-5 part B), `8c42fd5` (RFC-4 rev 2 hardening H1–H4, qtwayland patch), `1592fda` (RFC-6/RFC-7 backends, `SurfaceChannelHost`, `SurfacePolicy`, `SurfaceTransportSelector`), `725eb48` (tests, `docs/spec.md`), `0025e6b` (pacing debug summary) | [logos-co/logos-basecamp#526](https://github.com/logos-co/logos-basecamp/pull/526) (draft), five commits; the former prerequisite `5454a52` (dev build takes its GL stack from the closure) landed upstream as `66a747e` (#522) and the branch is rebased onto it; its `logos-view-module-runtime` input must be bumped to the merged runtime commit |

The hashes before the rebase (`37c9a57`, `62c430f`, `f73b358`, `6cfae7f`, `baabe03`;
`6a3994f`, `249eb33`) are the ones the RFC texts and `architecture.md` cite for "what was
implemented where"; the trees are identical, only the parents changed.

Order: runtime first (RFC-1/2/7/8 are what Basecamp and the builder link against),
builder next (RFC-5 A and RFC-8's include dir), Basecamp last with its
`logos-view-module-runtime` input bumped to the merged runtime commit. Each branch was
tested at its tip; intermediate commits build on Linux but were not re-tested one by one.
The builder and Basecamp PRs are drafts until the runtime PR merges: their `flake.nix`
input is `github:logos-co/logos-view-module-runtime`, so their locks can only point at
the merged commit. The branches are pushed to the `0xc1c4da` forks listed above; the
RFCs are referenced from the PRs at a fixed commit of this repository.

What was verified before offering them (all on Qt 6.9.2 from the SDK's nixpkgs, Linux,
Wayland session, Radeon 8060S):

- runtime `unit-tests` (handshake, `--gui`, token, `test_surface_channel` 14 cases
  offscreen and on GL via `LOGOS_SURFACE_TEST_GL=1 LOGOS_SURFACE_TEST_QPA=wayland`,
  `test_surface_view`), builder `tests/*` (incl. `view_surface` metadata and
  `view-interface-abi`), Basecamp `unit-tests` incl. `tests/view_surface_test.cpp`
  (item suite on both families, raw-client probe/oversize/truncate) and
  `tests/surface_channel_test.cpp`; Basecamp smoke, sandbox, qml, integration, shutdown,
  symbol-gate and link-gate checks; `ws check-qt`.
- `maps_view` in Basecamp headless (`nix run view#ui-tests`) over the default transport
  (`channel-shm`) and over `LOGOS_SURFACE_TRANSPORT=wayland` (`wayland-shm`), and the
  `logosctl` end-to-end snapshot (`view/tests/logosctl/e2e.sh`), same package for all.
- `maps_view` live on the desktop over both families with the same package: surface
  bound, frames visible, pan/zoom through the inspector, teardown clean. Measured under
  camera motion at 2400×1350: `channel-shm` 2.1 ms render + 2.1 ms readback per frame
  (~31 frames/s delivered under the rev 1 pacing, ~90/s with the standing credit of
  RFC-7 §5 rev 2, every frame shown); `wayland-shm` ~340 ms per frame on `llvmpipe` —
  which is why RFC-6 rev 2.1 makes `channel` the default everywhere.
- Not verified: `channel-d3d11` / `channel-iosurface` (no Windows/macOS build host; the
  code compiles only there), `wayland-dmabuf` (this host's EGL exposes no
  `EGL_WL_bind_wayland_display`, so the compositor's client buffer integration never
  comes up and `gpuBuffersAvailable()` is false — RFC-4 H5 therefore also unexercised),
  the in-process `ViewContainer` (RFC-6 §4, specified only), the capability gate
  (RFC-5 rev 2 Part C, specified with hook points only), live DPR change (RFC-5 rule 3).
- Known follow-ups, not regressions: `channel` releases a slot after the host has
  painted its successor (RFC-7 §5), which is required for `shm` and leaves the native
  kinds a fence-based release to add when they are verified; `wp_fractional_scale_v1` on the compositor is specified (RFC-4 rev 2) and not
  implemented.
