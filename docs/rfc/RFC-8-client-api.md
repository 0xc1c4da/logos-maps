# RFC-8: `LogosViewSurface` — the one window a view module renders into

**Repository:** `logos-view-module-runtime` (header `include/logos/view_surface.h`, library `LogosViewSurface`), `logos-module-builder` (include path) · **Status:** implemented (see §Status) · **Depends on:** RFC-1, RFC-2, RFC-6, RFC-7

## Problem

Under RFC-1..4 a view module obtains its surface by creating and showing its own
`QQuickWindow`; Qt's Wayland client turns that into the toplevel the host composites.
That only works for the `wayland` transport. Under `channel` (RFC-7) the window must be
one that a `QQuickRenderControl` drives into a host-granted slot, and in the in-process
container (RFC-6 §4) there is no window of the module's own at all. A module must not
have to know which case it is in.

## Proposal

### 1. The header

`<logos/view_surface.h>` is header-only and links nothing. It resolves a versioned C
function table that `ui-host` (or the in-process container) publishes on the
application object, so an old `ui-host` — which publishes nothing — yields `nullptr`
and the module falls back to opening its own window exactly as today.

```cpp
// include/logos/view_surface.h
struct LogosViewSurfaceV1 {
    uint32_t size;                                 // sizeof(LogosViewSurfaceV1)
    uint32_t version;                              // 1
    QQuickWindow* (*window)(void* ctx);            // the window the host composites
    QQuickItem*   (*rootItem)(void* ctx);          // where the module's content goes
    const char*   (*transport)(void* ctx);         // RFC-6 §3 name
    void*         ctx;
};

namespace LogosViewSurface {
inline const LogosViewSurfaceV1* table();          // qApp->property("logos.viewSurface.v1")
inline QQuickWindow* window();                     // nullptr without a transport
inline QQuickItem*   rootItem();                   // nullptr without a transport
inline QString       transport();                  // "none" without a transport
}
```

The property `logos.viewSurface.v1` on `QCoreApplication::instance()` holds a `void*`
to a table that lives as long as the application. Sandboxed app QML never sees it: the
property is on the C++ application object in the *view* process, not on anything the
host's QML engine can reach.

### 2. Semantics

- `window()` is called on the main thread, from `initLogos()` or later. The first call
  creates the window; later calls return the same pointer. Before the first call the
  module may choose its graphics API with `QQuickWindow::setGraphicsApi()` — the
  surface library does not touch it, so an OpenGL-only renderer keeps working.
- Under **`wayland`** the library creates a plain `QQuickWindow`, shows it, and Qt's
  Wayland client makes it the xdg toplevel whose `app_id` RFC-2 already set; the host
  configures its size (RFC-3). Under **`channel`** the window belongs to the
  `SurfaceProducer` (RFC-7 §1): never shown, sized by `Configure`, rendered on credit.
  In the **in-process container** `window()` is the host's window.
- `rootItem()` is `window()->contentItem()` for both transports and the container-owned
  item in the in-process case. A module adds its content as children of `rootItem()`
  and keeps them sized to it (bind to its `width`/`height`). It never calls `show()`,
  `resize()`, `setScreen()` or `setDevicePixelRatio()` on the window: geometry is the
  host's (RFC-6 §1 c), and under `channel` the window has no screen to speak of.
- `transport()` is the same string `Logos.Surface.transport` reports on the host side;
  a module uses it only for diagnostics and capability reporting (maps: `surface.transport`).
- The window's lifetime is the process's. A module must not delete it.

### 3. Loading QML into it

The pattern the builder's template and `maps_view` use:

```cpp
QQuickWindow* win = LogosViewSurface::window();
if (!win) {                       // no transport: dev launcher, old host, headless
    m_ownWindow = std::make_unique<QQuickWindow>();
    win = m_ownWindow.get();
    win->resize(1024, 768); win->show();
}
auto* engine = new QQmlEngine(win);
QQmlComponent component(engine, QUrl::fromLocalFile(qmlDir + "/MapWindow.qml"));
auto* root = qobject_cast<QQuickItem*>(component.create());
root->setParentItem(LogosViewSurface::rootItem() ? LogosViewSurface::rootItem()
                                                 : win->contentItem());
// size follows the parent item
```

`QQuickView` is not used: it owns a window and an engine of its own and cannot be
pointed at a foreign window.

### 4. The library's own API (for `ui-host` and tests)

```cpp
// LogosViewSurface shared library, resolved by ui-host with QLibrary
extern "C" {
int  logos_view_surface_init(const char* token, const char* socketName, const char* transport);
void logos_view_surface_shutdown();
}
```

`init` creates the client session for `channel` (RFC-7 §2) or records the Wayland case,
and installs the §1 table. `ui-host` calls it after the token lines and before loading
the plugin; failure to load the library is logged and leaves the table absent (module
falls back). `shutdown` runs in `aboutToUnload` handling before the application exits.

### 5. The renderer gate (informative, carried from the research)

Zero-copy `d3d11`/`iosurface` slots require the module's scene graph to run on D3D11 or
Metal, because `QQuickRenderTarget::fromD3D11Texture/fromMetalTexture` wrap a texture
of that API. MapLibre Native's Qt frontend is OpenGL-only, so `maps_view` negotiates
`shm` on Windows and macOS until one of these lands (none is in this revision):

- macOS: bind the IOSurface as a GL texture in the module (`CGLTexImageIOSurface2D`)
  and render the OpenGL scene graph into it with `fromOpenGLTexture` — a second
  `IOSurfaceSlotStorage` view-side binding, zero copies, no renderer change;
- Windows: `WGL_NV_DX_interop2` to render GL into the shared D3D11 texture (vendor
  dependent), or an ANGLE-on-D3D11 build of the module's Qt;
- MapLibre's Metal/Vulkan renderers through a non-Qt frontend.

## Tests

- `test_ui_host_gui`: spawn with `--surface-transport channel`; the fixture plugin
  journals that `LogosViewSurface::window()` is non-null, is a `QQuickWindow`, and that
  `transport()` is `channel-shm`; a plain `--gui` spawn journals `nullptr` and `none`.
- `test_surface_channel` (RFC-7) is the producer's test.

## Status

Implemented in `logos-view-module-runtime` (`include/logos/view_surface.h`,
`surface/SurfaceClientApi.cpp`), consumed by `maps_view` (`view/src/view/view_window.cpp`)
and documented in `logos-module-builder/docs/configuration.md` under `view_surface`.

Findings against the draft:

- **Include path.** The header is read from the runtime flake's *source* tree by
  `logos-module-builder/lib/buildCppPlugin.nix` when `metadata.json` says
  `view_surface: true` (`-DLOGOS_VIEW_SURFACE_INCLUDE_DIR`, consumed in
  `cmake/LogosModule.cmake`); only `logos/view_surface.h` is exposed, not the runtime's
  host-side headers, and no runtime package is built for a module build. `maps_view`'s
  standalone test build takes the same file from
  `logos-module-builder.inputs.logos-view-module-runtime`.
- **`transport()` under `wayland`** returns the family, not `wayland-shm`/`-dmabuf`: the
  buffer kind is negotiated between the Qt wayland client and the compositor and is
  only observable on the host (`Logos.Surface.transport`). Under `channel` the kind is
  known after `Welcome` and the full name is returned. Spec §11 was aligned.
- **`ui-host` loads the table for the Wayland transport too** (publishing only; the
  window is a plain top-level the Qt wayland client turns into the admitted toplevel),
  so a module written against RFC-8 behaves identically on both families. A `ui-host`
  without the library publishes nothing and the module's fallback window is what the
  compositor admits, as before RFC-8.
- **Consumer shape.** `maps_view`'s `ViewWindow` became a loader (`QQmlEngine` +
  `QQmlComponent` into `contentItem()`) over either the runtime's window or its own;
  `setGraphicsApi(OpenGL)` is kept ahead of the first `window()` call (§2); `show()`,
  `resize()` and `setTitle()` are no-ops on a host window. The module's conformance
  suites run with no table published and exercise the fallback.
- §5 (GL interop for `d3d11`/`iosurface` slots) is not implemented; `maps_view` would get
  `channel-shm` on Windows/macOS, as RFC-7 §Status says.
