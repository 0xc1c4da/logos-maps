# RFC-4: `SurfaceCompositor` — a nested Wayland compositor inside the host

**Repository:** `logos-basecamp` · **Status:** implemented, rev 2 (hardening; see §Hardening and §Status) · **Depends on:** RFC-1, RFC-2 · **Needed by:** RFC-3, RFC-6

## Problem

A view module's backend now renders in its own process (RFC-1). Its pixels must reach
the host's window with at most one copy and its input must come back, while the host
stays a single, sandboxed process that never loads module code. On Linux the standard
answer is the one Qt Application Manager uses for multi-process applications: the host
is a **nested Wayland compositor** and each application process is a Wayland client
whose toplevel is shown as an item in the host's scene graph.

## Proposal

### 1. The compositor

`app/surface/SurfaceCompositor.{h,cpp}`, a `QWaylandCompositor` subclass created once per
host process by `UIPluginManager` (lazily, on the first `view_surface` module), with:

- a private socket: `socketName = "logos-basecamp-" + QString::number(pid) + "-" + 8 random hex`,
  created in `XDG_RUNTIME_DIR` by `QWaylandCompositor::setSocketName` (mode 0700 is the
  runtime dir's; the socket is additionally protected by RFC-2 authentication);
- `QWaylandXdgShell` (xdg-shell stable) only — no wl_shell, no layer-shell, no decorations;
- one `QWaylandQuickOutput` whose `window` is the host's main `QQuickWindow`
  (see §4 for the `QQuickWidget` detail), `scaleFactor` bound to the window's DPR;
- `QWaylandSeat` with pointer, keyboard, touch;
- buffer kinds by **policy** (rev 2, §Hardening H2): `wl_shm` always; `linux-dmabuf` /
  `wl_drm` only when GPU buffers are allowed. The kind actually in use is reported as
  `transport` (RFC-3). No code in the host depends on which one is in use.

### 2. Admission

```cpp
void SurfaceCompositor::expect(const QString& token, qint64 pid);   // from PluginLoader after spawn
void SurfaceCompositor::forget(const QString& token);               // on module unload
```

On `QWaylandXdgShell::toplevelCreated`, the compositor reads `toplevel->appId()` and the
client's `processId()`. If `appId == "logos-view-" + token` for an expected token **and**
the pid matches, the toplevel is bound: `surfaceBound(token, toplevel)` is emitted and
`Logos.Surface` items waiting on that token attach (RFC-3). Otherwise the client is
`close()`d and the event logged. A client connection whose pid is not expected at all is
closed on `QWaylandCompositor::clientConnected` — unknown processes cannot even keep a
connection open.

### 3. Lifecycle (with RFC-5)

- Toplevel unmapped or client disconnected → `surfaceLost(token)`; the item shows the
  placeholder and emits `error("clientExited")`.
- `UIPluginManager::unloadUiModuleImpl` (`app/UIPluginManager.cpp:456–567`) calls
  `forget(token)` *before* `ViewModuleHost::stop()` so the compositor does not treat the
  disconnect as an error.
- Frame pacing: the compositor sends frame callbacks only for surfaces whose item is
  visible and whose tab is active (`QWaylandQuickItem::paintEnabled`/`visible` gates the
  `sendFrameCallbacks` the output issues). A hidden module therefore idles at zero FPS.

### 4. Output window

Basecamp shows app QML in a `QQuickWidget` per tab (`app/PluginLoader.cpp:450–462`).
`QWaylandQuickOutput::window` must be a `QQuickWindow`; a `QQuickWidget` owns an
offscreen `QQuickWindow` (`quickWindow()`) that renders into an FBO. Two shapes are
possible and Spike A in this workspace decides which ships:

- **A1 (preferred):** the output's window is the `QQuickWidget`'s `quickWindow()`;
  `Logos.Surface` is an ordinary item in the sandboxed scene. Zero changes to how tabs
  are laid out.
- **A2 (fallback):** a dedicated `QQuickWindow` hosted with
  `QWidget::createWindowContainer` inside the tab's `DockCard`, overlaying the
  `QQuickWidget` where the `Logos.Surface` item reports its geometry.

Both keep module code out of the host; A1 is one textured quad in the existing scene,
A2 adds a native child window. Outcome recorded in `docs/architecture.md` §17 O1.

### 5. Build

- `app/CMakeLists.txt:25`: add `WaylandCompositor` to the Qt components on Linux
  (`if(LINUX)` guard; macOS/Windows compile `SurfaceCompositor` out behind
  `LOGOS_HAVE_WAYLAND_COMPOSITOR`).
- `nix/app.nix`: `qt6.qtwayland` is already a build input (`101–107`) and on the library
  path (`114–160`); the compositor QML plugin (`QtWayland.Compositor`) is **not** needed
  because the compositor is instantiated from C++.
- Runtime: the host must itself run under a Wayland or X11 session; the nested
  compositor works under both (it is a server, independent of the host's own QPA).

### 6. Performance expectations (informative, spec M3.7)

Per frame: client renders to its own buffer; dmabuf import = zero copy, shm = one copy;
host draws one textured quad; one compositor hop of latency (one host frame). No IPC on
the input→render path other than the Wayland socket itself.

## Hardening (rev 2)

Rev 1 trusted Qt's compositor to be safe against a client that merely misbehaves. The
pinned sources say otherwise, so rev 2 adds the following; together they are what lets
RFC-6 claim requirement (e), fault isolation, for this transport. They do **not** make
the host safe against a hostile module (RFC-5 rev 2 §4 says what would).

### H1. Guarded `wl_shm` reads (Qt patch)

`qtwayland` 6.9.2 `src/compositor/wayland_wrapper/qwlclientbuffer.cpp:106–123`:
`SharedMemoryBuffer::image()` returns a `QImage` that *borrows* the client's mapped
pixels, and `toOpenGlTexture()` uploads from it, without `wl_shm_buffer_begin_access` /
`end_access`. A client that truncates its pool (or whose pool was never fully backed)
makes the host's render thread take `SIGBUS`. libwayland's documentation requires the
guard; wlroots (`types/wlr_shm.c`), Mutter (`meta-wayland-buffer.c:438`), Weston
(`gl-renderer.c:1261, 3556`) and KWin (`shmclientbuffer.cpp`, own handler) all have it.

Basecamp therefore builds its `qt6.qtwayland` with
`nix/patches/qtwayland-shm-guarded-access.patch` (also offered upstream):

- `toOpenGlTexture()` wraps the whole read — `image()`'s construction, any
  `convertToFormat`, `glTexImage2D` — in `begin_access`/`end_access`, keeping the single
  upload copy;
- the public `image()` deep-copies inside the guard (`QImage::copy()`), so the
  software/no-GL path (`createTextureFromImage`) and any other caller never hold borrowed
  pixels; the `QWaylandBufferRef::image()` contract ("returns an image") is unchanged.

libwayland's handler is per-thread (`pthread` key), so the render thread's upload is
protected independently of the GUI thread. libwayland 1.24's `F_SEAL_SHRINK` fast path
is a shortcut, not a hole: the guard is still correct for unsealed pools.

### H2. Global allowlist; GPU buffers are policy

`QWaylandCompositorPrivate::loadClientBufferIntegration` (`qwaylandcompositor.cpp:402–442`)
loads `wayland-egl` (hence `wl_drm`/`zwp_linux_dmabuf_v1`) unless told a *valid* other
plugin name; invented values such as `none` fall through to it. Rev 1's "Qt chooses" is
replaced by an explicit policy enforced with `wl_display_set_global_filter` on
`QWaylandCompositor::display()` right after `create()` — the filter governs both
advertisement in `wl_registry` and `wl_registry.bind`, so a client cannot bind a hidden
global by guessing its name:

| Global | Default | Note |
|---|---|---|
| `wl_compositor`, `wl_subcompositor`, `wl_shm`, `wl_seat`, `wl_output`, `xdg_wm_base` | allowed | the protocol a Qt client needs for one toplevel |
| `wp_viewporter`, `wp_fractional_scale_manager_v1` | allowed when the compositor instantiates them (H5) | |
| `zwp_linux_dmabuf_v1`, `wl_drm`, `qt_hardware_integration` | **hidden** unless `SurfaceCompositor::setGpuBuffersAllowed(true)` — set from `LOGOS_SURFACE_GPU_BUFFERS=1` | the dmabuf/EGL import path is unverified (spec O11); when allowed, `transport` reports `wayland-dmabuf` |
| `wl_data_device_manager`, `zwp_text_input_manager_v*`, `qt_text_input_method_manager_v1`, everything else | hidden | clipboard, drag-and-drop and IME are not bridged (RFC-7 §6 lists them as additive); a global that is not needed is not offered |

The policy is a property of the compositor instance (one per host), not an environment
variable the child could influence.

### H3. Bounds

- One toplevel per client (already rev 1: a second toplevel under a bound token is
  refused and the client closed).
- Buffer dimensions ≤ 8192 × 8192 and ≤ 256 MiB, checked on every `bufferSizeChanged`
  before Qt uploads; a larger buffer closes the client.
- `wl_client_set_max_buffer_size(client, 1 MiB)` at the first `surfaceRequested`, so a
  client cannot grow the host's per-connection receive buffer without bound.

### H4. Focus hygiene

Keyboard focus is given to the bound surface when its item gains active focus and
cleared (`QWaylandSeat::setKeyboardFocus(nullptr)`) on detach, unload and client exit,
so key events never flow to a surface that is no longer shown.

### H5. Fractional scale (specified; implemented if the schedule allows)

`wl_output.scale` is integral; rev 1 rounds a fractional DPR up and downsamples in the
host, which is a resampling pass RFC-6 §1 (c) wants to avoid. The fix is the standard
pair the Qt *client* already implements: a `wp_fractional_scale_manager_v1` server
extension (preferred scale in 1/120 units, generated with
`qt_generate_wayland_protocol_server_sources` from `wayland-protocols`) plus
`QWaylandViewporter`, with the item sending the viewport destination in logical pixels
and `buffer_scale 1`. Until it lands, rev 1's behaviour stays and §Status says so.

## Tests

Rev 2 adds to `tests/view_surface_test.cpp` a raw libwayland client (`--raw-client`,
`wayland-client` linked by the test only) that: commits a buffer from an **unsealed
memfd pool and truncates the file** after the commit — the host keeps rendering and the
client is disconnected (H1); attempts `wl_registry.bind` on `zwp_linux_dmabuf_v1` and
`wl_data_device_manager` — refused as protocol errors, and the registry listing shows
neither (H2); commits a 9000 × 9000 buffer — the client is closed (H3). The suite is
also run with `LOGOS_SURFACE_GPU_BUFFERS=1` to show the dmabuf global *is* advertised
then (it is not used under `offscreen`).

`nix/view-surface-test.nix` (check `view-surface-test`): runs Basecamp standalone under
`QT_QPA_PLATFORM=offscreen` with a fixture `view_surface` module (`tests/fixtures/surface-client/`
— a 50-line QML backend that opens a `QQuickWindow` filled with a colour and reports
received touch points over its `.rep`). Asserts: toplevel bound within 2 s; item size
400×300 at DPR 2 → client buffer 800×600; synthetic touch on the item → one touch point
reported by the client; `transport` is `wayland-shm` under offscreen (dmabuf needs a GPU);
a second fixture client with a wrong `app_id` is refused. This check is the acceptance
test for RFC-1..5 (spec §16.6).

## Status

Implemented in `logos-basecamp` at `37c9a57` (branch `rfc/view-surface`, M6c): `app/surface/SurfaceCompositor.{h,cpp}`,
`LOGOS_HAVE_WAYLAND_COMPOSITOR` in `app/CMakeLists.txt` (Linux; `Qt6::WaylandCompositor`
required there, compiled out elsewhere). Spike A settled §4 on **A1**: the output's
window is the tab's `QQuickWidget::quickWindow()` and `Logos.Surface` is an ordinary item
in the sandboxed scene. Deviations and findings against Qt 6.9 `qtwayland`:

- **Admission at `surfaceRequested`.** `QWaylandCompositor` has no `clientConnected`
  signal; the pid gate runs on `surfaceRequested` (the first thing a client does after
  binding globals) and refused clients are remembered so a toplevel they manage to
  create before the deferred `close()` lands is not counted twice. `close()` is always
  deferred (`QTimer::singleShot(0)`) — destroying a client inside its own request
  dispatch is unsafe.
- **Admission at the toplevel** happens when `appIdChanged` fires, falling back to
  `hasContentChanged`, because Qt clients set `app_id` after `get_toplevel`.
- **Default output.** Qt wayland clients will not map a window before they have a
  `wl_output`, and the first `view_surface` load creates the per-window output only after
  the client has connected. The compositor therefore always has a headless default
  `QWaylandOutput` (1920×1080@60, no window) whose `scaleFactor` is the primary screen's
  DPR; per-window `QWaylandQuickOutput`s (`sizeFollowsWindow`, scale = ⌈DPR⌉, updated on
  `screenChanged`) are created by `ensureOutput(window)` when an item attaches, and
  `QWaylandQuickItem::updateWindow` requires one to exist before `setShellSurface`.
- **Frame pacing** is Qt's: `QWaylandQuickOutput` issues frame callbacks from the
  window's `afterRendering`; visibility gating is done by `Logos.Surface` moving its view
  off the output (`QWaylandView::setOutput(nullptr)`) while invisible.
- **`expect(module, token, pid)`** takes the module name too (one surface per module;
  a re-spawn replaces the previous token), and `forget` is routed through
  `PluginLoader::stopViewModuleHost`, which `UIPluginManager` calls on both unload paths.
- **Unavailable, not fatal:** no writable `XDG_RUNTIME_DIR` → `isAvailable() == false`,
  modules are spawned without `--surface-socket` and `Logos.Surface` reports `noCompositor`.
- **Headless.** With `QT_QPA_PLATFORM=offscreen` the host has no EGL; shm buffers go
  through `createTextureFromImage` and work with the software scene graph. The client must
  also use `QT_QUICK_BACKEND=software` in that configuration.
- **Test shape.** §Tests is realised as `tests/view_surface_test.cpp` in the `unit-tests`
  check rather than a separate `view-surface-test` derivation with a fixture module: the
  same binary is the Wayland client (`--surface-client <token>`), so the check needs no
  built module and runs in ~1 s. It covers everything listed above plus client exit,
  unknown pid, visibility gating and clean teardown. The real-app smoke test is
  `maps_view`'s `tests/basecamp-surface.mjs` (`nix run view#ui-tests`, M7): the
  `Logos.Surface` in the maps tab binds, reports `wayland-shm`, and the host window is
  captured with the map in it. Only `wl_shm` has been observed on this host (software
  GL in the nix closure); the dmabuf path is spec O11.

### Rev 2 status

Hardening H1–H4 is implemented on `rfc/view-surface`:

- **H1** — `nix/patches/qtwayland-shm-guarded-access.patch`, applied by
  `nix/qtwayland.nix` (`qt6.qtwayland.overrideAttrs`) and used by `nix/app.nix`,
  `nix/unit-tests.nix` and `nix/coverage.nix`. `SharedMemoryBuffer::image()` deep-copies
  inside `wl_shm_buffer_begin_access`/`end_access`; the former zero-copy wrap is kept as
  `borrowedImage()` for callers that hold the guard themselves, and
  `toOpenGlTexture()` is guarded the same way. The truncation test
  (`rawTruncatedPoolDisconnectsOnlyTheClient`) was run once against unpatched qtwayland to
  confirm it would have taken the host down.
- **H2** — `SurfaceCompositor::isGlobalAllowed` + `wl_display_set_global_filter`,
  `setGpuBuffersAllowed` (initialised from `LOGOS_SURFACE_GPU_BUFFERS`). Under
  `offscreen` the dmabuf global is never created at all, so the "advertised when allowed"
  half is covered by the `isGlobalAllowed` unit assertions rather than a registry listing.
- **H3** — `kMaxBufferSide`/`kMaxBufferBytes` checked on `bufferSizeChanged` of every
  admitted surface (`refuse(client, "oversize")`, `surfaceLost` to the item), and
  `wl_client_set_max_buffer_size(1 MiB)` applied once per expected client.
- **H4** — in `WaylandSurfaceBackend`: keyboard focus on active focus, cleared on detach,
  unload and client exit.
- **H5 is not implemented.** The per-window output still advertises `⌈DPR⌉` and
  fractional scales are downsampled in the host. The design above stands as the
  follow-up (spec O15): a `wp_fractional_scale_manager_v1` server global generated with
  `qt_generate_wayland_protocol_server_sources`, `QWaylandViewporter` enabled, the item
  sending a logical destination size and the client committing `buffer_scale 1`. Both
  globals are already on the H2 allowlist so landing H5 is additive.

**Where this transport now sits (RFC-6 rev 2.1).** The nested compositor is no longer
the Linux default; `channel` is, and `wayland` is selected only when the host allows
GPU buffers *and* the compositor's client buffer integration came up
(`SurfaceCompositor::gpuBuffersAvailable()`, read through
`QWaylandCompositorPrivate::clientBufferIntegrations()` — the one private-API use in the
host, for a boolean Qt otherwise only logs). The reason is a consequence of H2 that rev 2
wrote down without following through: a client of a `wl_shm`-only compositor gets no GPU
context from Mesa and renders in software. On this host the integration cannot come up
at all (the Wayland-QPA host's EGL exports no `EGL_WL_bind_wayland_display`), so the
transport ran on `llvmpipe` in every live test and was ~80× slower than `channel-shm`
under camera motion (numbers in RFC-6 §3). `LOGOS_SURFACE_TRANSPORT=wayland` still
forces it, which is how the suites below exercise the compositor.

The raw-client suite in §Tests is realised as `--raw-client <token> <probe|oversize|truncate>`
in `tests/view_surface_test.cpp`, with `wayland-client`, `wayland-scanner` and
`wayland-protocols` (for `xdg-shell` client glue) added to the `unit-tests` shell only;
the three tests `QSKIP` when `LOGOS_HAVE_RAW_WAYLAND_CLIENT` is not defined. The probe
test asserts that the registry lists exactly the allowlist and that binding
`zwp_linux_dmabuf_v1` or `wl_data_device_manager` by a guessed name fails. The companion
channel-transport suite (`tests/surface_channel_test.cpp`, RFC-7) runs the same
`Logos.Surface` item under `LOGOS_SURFACE_TRANSPORT=shm`.
