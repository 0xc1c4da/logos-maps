# RFC-1: `ui-host --gui` — a GUI-capable host process for `view_surface` modules

**Repository:** `logos-view-module-runtime` · **Status:** draft · **Depends on:** nothing · **Needed by:** RFC-2, RFC-4

## Problem

`ui-host` is a `QCoreApplication` (`ui-host/main.cpp:98`). It has no `QGuiApplication`,
no platform plugin, no scene graph and no GPU context, so a view module's backend cannot
render anything: all pixels a `ui_qml` module shows today are drawn by QML that runs in
the host's `QQuickWidget`. A module whose view *is* a renderer (a map, a video, a 3D scene)
has no legitimate place to run it — the only alternative would be to load its C++ into
the host, which the sandbox forbids (`logos-basecamp/app/restricted/QmlSandbox.cpp:36–44`,
regression F-008) and which `docs/architecture.md` §3 rules out.

## Proposal

Add an opt-in **GUI mode** to `ui-host`. In GUI mode the process is a `QGuiApplication`
and may create `QWindow`s/`QQuickWindow`s; everything else (token handshake, plugin load,
`initLogos`, QtRO remoting, `READY`, unload grace) is unchanged.

### Command line

```
ui-host --name <module> --path <plugin> --socket <qtro socket> [--gui] [--surface-socket <name>]
```

- `--gui`: construct `QGuiApplication` instead of `QCoreApplication`. Without it the
  binary behaves exactly as today (same object, same code path; the branch is a single
  `if` before construction).
- `--surface-socket <name>` (RFC-2): the Wayland socket of the host's nested compositor.
  Implies `--gui`. Before `QGuiApplication` is constructed, `ui-host` sets
  `QT_QPA_PLATFORM=wayland` and `WAYLAND_DISPLAY=<name>` in its own environment and
  clears `DISPLAY`, so the Qt wayland client plugin connects to the host and to nothing
  else. On platforms without Wayland (RFC-6) the option is accepted and ignored.

### Application object

```cpp
std::unique_ptr<QCoreApplication> app;
if (gui) {
    qputenv("QT_QPA_PLATFORM", "wayland");
    qputenv("WAYLAND_DISPLAY", surfaceSocket.toUtf8());
    qunsetenv("DISPLAY");
    app = std::make_unique<QGuiApplication>(argc, argv);
} else {
    app = std::make_unique<QCoreApplication>(argc, argv);
}
```

`QGuiApplication::setDesktopFileName()` is set in RFC-2 (it becomes the xdg `app_id`
the compositor authenticates). `setQuitOnLastWindowClosed(false)` is set so a module
closing its window does not end the process; lifetime remains the parent's (RFC-5).

### Rendering API available to the plugin

Nothing new is injected into the plugin. A `view_surface` plugin that wants to render
creates its own `QQuickWindow` (or `QWindow` + `QOpenGLContext`) in `initLogos()` or
later; because the process is now a GUI application, Qt provides the platform window,
and under `--surface-socket` that window is a Wayland toplevel shown by the host (RFC-4).
The plugin must size the window from the host's configure events (xdg-shell), which Qt
does automatically for a `QQuickWindow`.

### Build

`ui-host` links `Qt6::Gui` in addition to `Qt6::Core` and `Qt6::RemoteObjects`
(`CMakeLists.txt:93`). `Qt6::Quick` is *not* linked by `ui-host`; a plugin that needs it
links it itself (the loader resolves it when the plugin is loaded). The Wayland client
QPA plugin is a runtime dependency supplied by the distribution; Basecamp already ships
`qt6.qtwayland` (`logos-basecamp/nix/app.nix:101–107`).

### Lifecycle

Unchanged: `PR_SET_PDEATHSIG`, parent-pid watchdog, SIGTERM self-pipe (`main.cpp:68–145`),
`runPluginAboutToUnload` grace (`main.cpp:316–322`). A `QGuiApplication` additionally
needs `QGuiApplication::quit()` to be reachable while a window is open; the existing
SIGTERM notifier already calls `app.quit()` on the main thread.

## Compatibility

- Hosts that never pass `--gui` observe no change.
- A `view_surface` module launched by an old host (no `--gui`) loads but has no
  `QGuiApplication`; the plugin should detect `qobject_cast<QGuiApplication*>(qApp) == nullptr`
  and report `surface.transport: none` rather than crash. `maps_view` does this.

## Tests

- `tests/test_ui_host_gui.cpp`: spawns `ui-host --gui` with `QT_QPA_PLATFORM=offscreen`
  forced in the test environment (ui-host only *sets* `wayland` when `--surface-socket`
  is given), loads a fixture plugin (`tests/test_gui_fixture.cpp`) that journals the
  application class it was given and opens a `QQuickWindow` on the software scene graph,
  asserts `READY` is printed, `quitOnLastWindowClosed` is off and the window reaches
  `isExposed()`; a plain spawn journals `QCoreApplication` and opens nothing.
- The existing `test_ui_host_unload` passes unchanged (it never passes `--gui`).

## Status

Implemented in `logos-view-module-runtime` (workspace branch, M6a): `ui-host/main.cpp`
pre-scans `--gui`/`--surface-socket` before constructing the application, links
`Qt6::Gui`; `ViewSpawnOptions` in `include/ViewModuleHost.h`.

## Alternatives considered

- A second binary `ui-host-gui`: duplicates the token/QtRO/unload code; a flag is one line.
- Always `QGuiApplication`: forces a platform plugin on every headless backend and breaks
  CI environments without a display. Opt-in keeps the default lightweight.
