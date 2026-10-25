# RFC-4: `SurfaceCompositor` — a nested Wayland compositor inside the host

**Repository:** `logos-basecamp` · **Status:** draft · **Depends on:** RFC-1, RFC-2 · **Needed by:** RFC-3

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
- hardware integration selected by Qt (`linux-dmabuf-unstable-v1` when the host's EGL
  supports it, else `wl_shm`) — the result is reported as `transport` (RFC-3). No code
  in the host depends on which one is in use.

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

## Tests

`nix/view-surface-test.nix` (check `view-surface-test`): runs Basecamp standalone under
`QT_QPA_PLATFORM=offscreen` with a fixture `view_surface` module (`tests/fixtures/surface-client/`
— a 50-line QML backend that opens a `QQuickWindow` filled with a colour and reports
received touch points over its `.rep`). Asserts: toplevel bound within 2 s; item size
400×300 at DPR 2 → client buffer 800×600; synthetic touch on the item → one touch point
reported by the client; `transport` is `wayland-shm` under offscreen (dmabuf needs a GPU);
a second fixture client with a wrong `app_id` is refused. This check is the acceptance
test for RFC-1..5 (spec §16.6).
