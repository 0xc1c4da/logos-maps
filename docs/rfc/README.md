# Platform RFCs: out-of-process view surfaces

These six proposals are what `docs/architecture.md` §3 asks of the Logos platform so
that a `ui_qml` module can render GPU content in **its own process** and have the host
composite it into a tab — without any module code being loaded into the host. They are
written against the repositories and line numbers in the spec's alias table (§0.2) and
are implemented and tested in this workspace first (branches `rfc/view-surface` in each
repository), then offered upstream.

| RFC | Repository | Summary | Status |
|---|---|---|---|
| [RFC-1](RFC-1-view-surface-ui-host.md) | `logos-view-module-runtime` | `ui-host --gui`: a `QGuiApplication` process for modules that declare `view_surface` | draft |
| [RFC-2](RFC-2-surface-handshake.md) | `logos-view-module-runtime` | Surface handshake: `--surface-socket`, `app_id` token, `READY ... SURFACE <token>`, `ViewModuleHost::surfaceToken()` | draft |
| [RFC-3](RFC-3-logos-surface-qml.md) | `logos-basecamp` | `Logos.Surface` host QML type available to sandboxed app QML | draft |
| [RFC-4](RFC-4-nested-compositor.md) | `logos-basecamp` | Nested Wayland compositor (`SurfaceCompositor`) on a private socket | draft |
| [RFC-5](RFC-5-lifecycle-and-metadata.md) | `logos-module-builder`, `logos-basecamp` | `view_surface` metadata key; surface lifecycle and visibility pacing | draft |
| [RFC-6](RFC-6-other-transports.md) | platform | Other `SurfaceTransport`s: macOS, Windows, in-process `ViewContainer` (informative) | informative |

Design invariants every RFC preserves (spec M3.1–M3.3):

1. No module QML plugin, C++ or shared library is loaded into the host process. The
   sandbox rules of `QmlSandbox.cpp` and `RestrictedUrlInterceptor.h` are unchanged.
2. The module's existing contract is untouched: a `ui_qml` module without
   `view_surface` builds and runs byte-for-byte as before.
3. Everything added is generic. `Logos.Surface` knows nothing about maps; a video
   player, a 3D viewer or a terminal module uses it identically.

Vocabulary: a **view module** is a `ui_qml` module; its **backend** runs in `ui-host`;
a **surface** is the GPU buffer the backend renders and the host shows; the **transport**
moves it (RFC-4: Wayland).
