# RFC-3: `Logos.Surface` — a host QML type that shows a module's surface

**Repository:** `logos-basecamp` (and the design system's vetted module list) · **Status:** draft · **Depends on:** RFC-2, RFC-4

## Problem

A `ui_qml` module's view QML runs sandboxed in the host (`app/PluginLoader.cpp:450–527`)
and can import only `Logos.Theme`, `Logos.Controls`, `Logos.Icons`
(`app/restricted/QmlSandbox.cpp:60–67`, `SharedLogosModules.h:17–24`) plus the `logos`
and `isActiveTab` context properties. It has no way to say "put my backend's rendered
surface here".

## Proposal

A fourth vetted host module, `Logos.Surface`, installed beside the others under
`$APPDIR/lib/Logos/Surface` and provided by Basecamp itself (C++ `QQuickItem`, compiled
into the host; **no module code involved**). Sandboxed QML uses it as:

```qml
import Logos.Surface

Logos.Surface {
    anchors.fill: parent
    module: "maps_view"          // the ui_qml module whose backend renders
    session: "basecamp/main"     // host-chosen; becomes the viewId prefix the module reports
    onReadyChanged: if (ready) console.log("surface up", viewId)
    onError: (code) => console.warn("surface error", code)
}
```

### Properties and signals

| Member | Type | Semantics |
|---|---|---|
| `module` | `string` | Required. Module name; must equal the enclosing app (the sandbox's own module name) — a view cannot embed another module's surface. Enforced by the host. |
| `session` | `string` | Opaque; forwarded to the backend as `LOGOS_VIEW_SESSION` through the `.rep` replica (`setSession(QString)` slot if the backend exposes one) so one backend can serve several surfaces. |
| `viewId` | `string` (read-only) | `<module>/<session>/<n>`; set when the surface binds. |
| `ready` | `bool` (read-only) | True once a toplevel with the module's token (RFC-2) has mapped and been configured to this item's size. |
| `transport` | `string` (read-only) | `wayland-dmabuf`, `wayland-shm` or `none` (spec §11.1 `surface.transport`). |
| `error(string code)` | signal | `noCompositor`, `notGuiModule`, `tokenMismatch`, `clientExited`. |

### Behaviour

- **Binding.** On completion the item asks the host's `SurfaceCompositor` (RFC-4) for the
  toplevel registered under the token that `LogosQmlBridge::viewModuleSurfaceToken(module)`
  returns (RFC-2 §6). If the toplevel has not mapped yet it waits (`ready = false`).
- **Size and scale.** Item geometry changes call `QWaylandXdgToplevel::sendConfigure(size)`
  in logical pixels; the output's `scaleFactor` follows the host window's
  `devicePixelRatio`, so the client renders at physical resolution. The buffer the client
  attaches is presented 1:1 — no scaling pass in the host.
- **Input.** The item is a `QWaylandQuickShellSurfaceItem`; Qt forwards pointer, wheel,
  touch (`QWaylandTouch::sendFullTouchEvent`) and keyboard to the client when the item has
  focus. `inputEventsEnabled` is true; the host never interprets gestures.
- **Visibility.** `isActiveTab == false` or `visible == false` stops frame callbacks to
  the client (RFC-5), so a background tab does not render.
- **Fallback.** If the compositor is unavailable (`transport == none`), the item shows a
  themed placeholder and emits `error("noCompositor")`; the module's backend is still
  spawned with `--gui` and may open an ordinary top-level window instead (what `maps_view`
  does in development).

### Where the code lives

- `app/surface/LogosSurfaceItem.{h,cpp}` — the item (`QML_NAMED_ELEMENT(Surface)`).
- `app/surface/qml/qmldir` + `qt_add_qml_module(URI Logos.Surface)` in `app/CMakeLists.txt`,
  installed to `lib/Logos/Surface` by `nix/app.nix` beside the design-system modules
  (`nix/app.nix:904–910`).
- `SharedLogosModules.h`: add `"Surface"` to the vetted list so `QmlSandbox` whitelists
  the import path and `RestrictedUrlInterceptor` redirects a plugin-tree probe for
  `Logos/Surface` to the host copy (`app/restricted/RestrictedUrlInterceptor.cpp:138–153`).

### Sandbox invariants

`Logos.Surface` is host code, like `Logos.Controls`. The interceptor still rejects native
`plugin` lines in any `qmldir` under the module's roots; a module cannot ship its own
`Logos.Surface`. The only new capability a sandboxed view gains is *displaying* a buffer
its own backend process produced.

## Tests

- `tests/sandbox/`: extend the evil-app fixture to attempt `module: "other_module"`;
  assert `error("tokenMismatch")` and no surface.
- RFC-4's smoke check (`view-surface-test`) exercises the happy path end to end.

## Status

Implemented in `logos-basecamp` (workspace branch, M6c): `app/surface/LogosSurfaceItem.{h,cpp}`,
`qt_add_qml_module(LogosBasecamp URI Logos.Surface ...)` in `app/CMakeLists.txt`,
`"Surface"` in `SharedLogosModules.h`. Deviations from the draft, all recorded for the
upstream version:

- **Registration.** Basecamp's `Logos.*` modules are static `qt_add_qml_module` targets
  registered process-wide from the executable, not installed under `lib/Logos/<Name>`;
  `Logos.Surface` is registered the same way (there is no `qmldir`/plugin dir to ship,
  and the interceptor's `Logos/Surface` redirect is a no-op that only has to be allowed).
- **Module binding.** The module an item may bind is pinned per QML engine
  (`LogosSurfaceItem::bindEngine(engine, module)` from `PluginLoader::createSandboxedView`,
  stored as a dynamic property on the engine, unreachable from QML) rather than looked up
  through `LogosQmlBridge`; `module:` naming anything else is `tokenMismatch`.
- **`session`** is forwarded by the app itself through its own `.rep` contract; the host
  does not set `LOGOS_VIEW_SESSION`. It still prefixes `viewId`.
- **`placeholderColor`** (default `#1e1e1e`) added: the colour painted until the first
  frame and after `clientExited`.
- **Re-attach.** After `clientExited` the item keeps waiting on its module; if the same
  token binds again (a host restart under the same spawn) it re-attaches.
- **`wl_output.enter`.** Qt only sends `enter` from the first frame-callback round after
  the surface has content, and marks it done even if the client has not yet bound the
  per-window output global — so a client can miss it and stay at the default output's
  scale. The item therefore (a) relies on RFC-4's default output already advertising the
  primary screen's scale, and (b) for up to 3 s after attaching re-adds its view to the
  window output and requests a frame until `bufferScale == scaleFactor`.
- The evil-app sandbox fixture is not extended; the cross-module refusal is covered by
  `tests/view_surface_test.cpp` (`itemRefusesOtherModules`, `itemExposesNoTokenToQml`).
