# RFC-6: Other `SurfaceTransport`s (informative)

**Status:** informative · **Depends on:** RFC-1..5 define the Linux transport; this note records how the same contract is met elsewhere so that nothing in RFC-1..5 paints the platform into a corner.

The contract a transport fulfils (spec M3.4): (a) zero- or single-copy frame delivery,
(b) full input forwarding, (c) size/DPR/visibility propagation host → view, (d) frame
pacing. `Logos.Surface` (RFC-3) and the module contract are transport-agnostic; only the
host-side item implementation and the `ui-host` window creation differ.

| Platform | Transport | Delivery | Input | Notes |
|---|---|---|---|---|
| Linux (Wayland or X11 session) | **nested Wayland compositor** (RFC-4) | dmabuf (0 copies) or shm (1 copy) | native Wayland protocols | ships first |
| macOS | `IOSurface` + `CAMetalLayer`/`QRhi` texture import | 0 copies | host converts `QEvent`s to a private QtRO stream the module replays into its `QQuickWindow` via `QWindowSystemInterface::handle*Event` | the `ui-host --gui` process uses the `cocoa` QPA with an offscreen window; `--surface-socket` names a Mach port |
| Windows | DXGI shared handle (`ID3D11Texture2D` keyed mutex) | 0 copies | same QtRO input stream | `ui-host --gui` with `windows` QPA, hidden window |
| iOS / Android | **in-process `ViewContainer`** | none needed — the module's `QQuickItem` lives in the host scene | native | liblogos has no subprocesses on iOS (`logos-liblogos/docs/spec.md:231`); the platform already runs *modules* in-process there, so views follow. The same `maps_view` sources compile as an in-process plugin; only the container differs. The sandbox guarantee is provided by the OS app sandbox instead of process isolation, as it is for core modules on those platforms. |
| Headless / CI | `none` | — | — | `ui-host --gui` under `offscreen`; the module opens a top-level window that nobody shows; tests read pixels via the module's own snapshot API |

Design consequences already honoured by RFC-1..5:

- `ViewSpawnOptions.surfaceSocket` is an opaque string, not a path.
- `Logos.Surface.transport` is a reported string, not an enum the module switches on.
- `READY ... SURFACE <token>` and `app_id` matching generalise to "the client proves it
  holds the per-spawn token" on every transport (Mach port name, DXGI handle name).
- Nothing in the module API refers to Wayland; the `maps_view` sources contain Wayland
  only in `metadata.json` link flags for the client QPA plugin dependency.

Not planned: X11 `XEmbed` (dead on Wayland sessions and on Qt 6), `QWindow::fromWinId`
embedding (one native child window per surface; input and DPR handling are platform
quirks each), and pixel streaming over QtRO (a full-frame copy through the IPC layer per
frame — only acceptable as a last-resort debug path).
