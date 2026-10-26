# RFC-7: The `channel` SurfaceTransport — host-allocated slots over a local socket

**Repositories:** `logos-view-module-runtime` (library `LogosViewSurface`), `logos-basecamp` (`ChannelSurfaceBackend`) · **Status:** implemented for `shm` slots (see §Status) · **Depends on:** RFC-1, RFC-2, RFC-6 · **Needed by:** RFC-8

## Problem

RFC-6 selects one transport family for every operating system that is not Linux, and a
fallback for Linux itself. It must deliver frames with zero or one copy (RFC-6 §1 a),
carry full input (b), be host-authoritative about geometry (c), render on demand only (d)
and keep the host safe from a misbehaving view process (e) — on Windows with Direct3D,
on macOS with Metal, and on any OS with plain shared memory for CI and as the last
resort. Wayland does none of this outside Linux; QtRO is not a frame transport and is
documented as unsafe against malformed peers.

## Proposal

One small binary protocol, one host-owned ring of frame slots whose *storage* is native
to the OS, and a `QQuickRenderControl` producer in the view process. The host never
waits on the view; the view never allocates anything the host has to import.

### 1. Processes and objects

| Where | Object | Role |
|---|---|---|
| host | `SurfaceHostSession` (`LogosViewSurface`) | `QLocalServer` on a per-spawn name; one accepted connection; owns the slot ring, the geometry generation, the credit and the input sender |
| host | `ChannelSurfaceBackend` (`logos-basecamp/app/surface/`) | the `Logos.Surface` backend: imports the ready slot as a `QSGTexture`, paints it, forwards the item's `QEvent`s |
| view | `SurfaceClientSession` (`LogosViewSurface`) | connects, authenticates with the RFC-2 token, attaches slots, receives configure/credits/input |
| view | `SurfaceProducer` (`LogosViewSurface`) | the `QQuickWindow` + `QQuickRenderControl` that renders a granted slot and replays input |
| both | `SurfaceChannel`, `SurfaceProtocol.h` | framing and the message structs below |
| both | `SlotStorage` (`ShmSlotStorage`, `D3D11SlotStorage`, `IOSurfaceSlotStorage`) | allocate / export / import / map one slot kind |

`LogosViewSurface` is a shared library installed by `logos-view-module-runtime`
(`lib/libLogosViewSurface.so|.dylib`, `bin/LogosViewSurface.dll`). Basecamp links it;
`ui-host` loads it with `QLibrary` only when spawned with `--surface-transport channel`,
so a headless backend keeps today's footprint (RFC-1 kept `Qt6::Quick` out of `ui-host`).

### 2. Spawn

```cpp
struct ViewSpawnOptions {
    bool gui = false;
    QString surfaceSocket;      // wayland: WAYLAND_DISPLAY name; channel: QLocalServer name
    QString surfaceTransport;   // "wayland" | "channel" | "" (= none)
};
```

`ui-host --gui --surface-transport channel --surface-socket <name>`: the environment is
left alone (no `QT_QPA_PLATFORM` override — the producer never shows a window, so the
platform plugin only needs to initialise; tests run it under `offscreen`); after the
RFC-2 token lines are read, `ui-host` loads `LogosViewSurface`, calls
`logos_view_surface_init(token, socketName)` and installs the RFC-8 table. The `READY`
line is unchanged.

The platform plugin the child inherits also decides its graphics API: Qt Quick picks the
RHI backend the platform can create a context for, and under `offscreen` that is the
software scene graph unless `QT_QUICK_BACKEND=rhi` and a GL stack are available. Modules
whose content is pure QML render either way; modules that need GL themselves (`maps_view`
and its `MapItem`) only render when the child starts on a platform with a GL context —
the desktop session's, in practice. A host that is itself headless and exports
`QT_QPA_PLATFORM=offscreen` to its children (Basecamp's `dev --test`) therefore gets a
blank surface from such a module, which is a property of that harness, not of the
transport (RFC-4's child is unaffected because `ui-host` sets `QT_QPA_PLATFORM=wayland`
for it).

The server name is `logos-surface-<hostpid>-<8 random hex>`; `QLocalServer` is created
with `UserAccessOption`. The host registers `expect(module, token, pid)` on the
session before the child can connect, exactly as for the compositor (RFC-4 §2).

### 3. Wire protocol

All integers little-endian; all structs fixed layout (`static_assert`ed); every length is
checked against the header *before* anything is parsed; payloads above
`kMaxPayload = 64 KiB` close the connection.

```cpp
struct Header {                     // 32 bytes
    uint32_t magic;                 // 'L' 'V' 'S' '1'
    uint16_t version;               // 1
    uint16_t type;                  // MessageType
    uint32_t length;                // payload bytes
    uint32_t generation;            // geometry generation this message is about (0 = none)
    uint32_t sequence;              // sender's counter, increments by one
    uint32_t flags;                 // per type
    uint32_t reserved[2];           // zero
};
```

| # | Message | Direction | Payload | Invariant |
|---|---|---|---|---|
| 1 | `Hello` | view → host | `uint64 pid; uint32 slotKinds; uint32 graphicsApi; char token[32]` | first message, within 10 s of connect; `pid` equals the socket peer's pid *and* the expected pid; `token` is the expected one; otherwise close |
| 2 | `Welcome` | host → view | `uint32 accepted; uint32 slotKind; uint32 slotCount; uint32 maxPixelW; uint32 maxPixelH; uint64 hostDevice` | `slotKind` ∈ the intersection of what both offer; `hostDevice` is the adapter LUID (`d3d11`) or 0 |
| 3 | `KindRejected` | view → host | `uint32 kind` | the view cannot open this kind on its device (adapter mismatch, API missing); host re-`Welcome`s with `shm` and a new generation |
| 4 | `Configure` | host → view | `uint32 logicalW, logicalH; uint32 pixelW, pixelH; double dpr; uint32 visible; uint32 format` | `generation` increases by one per geometry/DPR change; `format`: 1 = RGBA8 premultiplied, 2 = BGRA8 premultiplied |
| 5 | `Attach` | host → view | `uint32 slot; uint32 kind; uint32 pixelW, pixelH; uint32 stride; uint64 byteLength; uint64 nativeHandle; uint32 keyLength; char key[]` | one per slot per generation; `key`/`nativeHandle` per §4 |
| 6 | `Detach` | host → view | `uint32 slot` | slot of an old generation is withdrawn; the view drops its mapping |
| 7 | `FrameRequest` | host → view | `uint32 slot; uint32 credit; uint32 deadlineUs` | the credit: permission to render exactly one frame into `slot`; at most one outstanding |
| 8 | `FrameWanted` | view → host | — | the scene changed and the view holds no credit; coalesced to one per event-loop turn |
| 9 | `FrameReady` | view → host | `uint32 slot; uint32 credit; uint32 dmgX, dmgY, dmgW, dmgH; uint64 frameIndex` | must name the outstanding credit and its slot in the current generation; anything else closes |
| 10 | `Release` | host → view | `uint32 slot` | the host has finished reading `slot`; it may be granted again |
| 11 | `Input` | host → view | `uint32 kind; …` (§6) | — |
| 12 | `Bye` | either | `uint32 reason` | orderly end |

Unknown message types with `flags & 1` (must-understand) close the connection; without it
they are skipped (forward compatibility).

### 4. Slots and storage

The ring has **three** slots per generation, all allocated by the host before the first
`Attach`, all of the exact pixel size of the current `Configure`. Slot state machine
(host-owned):

```
Free ──FrameRequest──▶ Granted ──FrameReady──▶ Ready ──painted──▶ Displayed ──successor displayed + frameSwapped──▶ Free
```

The host paints the newest `Ready` slot and keeps it `Displayed` until the *next* frame
has been displayed and the window's `frameSwapped` has fired once after that — only
then is `Release` sent. The host therefore always has a complete frame to show, and the
view can never write into memory the host's render thread may still read. A
`FrameReady` for a stale generation is answered with `Release` and dropped.

| Kind | Allocation (host) | Transfer | View side | Host import | Copies |
|---|---|---|---|---|---|
| `shm` | `QSharedMemory` of `stride × pixelH` bytes under a random key, created by the host; `key` is the native key string | the key in `Attach` | `QSharedMemory::attach()`; after rendering, readback into the mapping (software backend: `QQuickRenderControl::grab()`; RHI: `QRhi` texture readback) | `QImage` over the mapping → `QQuickWindow::createTextureFromImage` (upload) | 1 (+ upload) |
| `d3d11` | `ID3D11Texture2D` on the host scene graph's device (`QSGRendererInterface::DeviceResource`), `BindFlags RENDER_TARGET|SHADER_RESOURCE`, `MiscFlags SHARED_NTHANDLE|SHARED_KEYEDMUTEX`, `DXGI_FORMAT_B8G8R8A8_UNORM` | `IDXGIResource1::CreateSharedHandle` then `DuplicateHandle` into the view process; the *duplicated* value is `nativeHandle` — a kernel-transferred capability, not a guessable number; the host closes its own copy of the duplicate | `ID3D11Device1::OpenSharedResource1`, description validated, `IDXGIKeyedMutex` | `QNativeInterface::QSGD3D11Texture::fromNative` | 0 |
| `iosurface` | `IOSurfaceCreate` (`BGRA`, 4 bytes/element) | Mach: the host checks in a receive right under `co.logos.surface.<token>` (`bootstrap_check_in`), the view looks it up and requests slot *n*; the reply moves the `IOSurfaceCreateMachPort` send right; `key` carries the service name, `nativeHandle` the slot index | `IOSurfaceLookupFromMachPort`, `MTLDevice.newTexture(descriptor:iosurface:plane:)` | `MTLTexture` on the host device → `QNativeInterface::QSGMetalTexture::fromNative` | 0 |

Ownership and completion per kind:

- `shm`: the view's readback (`QRhi::endOffscreenFrame`, or `grab()`) completes before
  `FrameReady`; the host's upload runs on the render thread from memory that stays
  mapped until `Release`.
- `d3d11`: keyed mutex key **0 = view may render, 1 = host may read**. View:
  `AcquireSync(0, 2000 ms)` → render → `ReleaseSync(1)` → `FrameReady`. Host (render
  thread): `AcquireSync(1, 0)`; `S_OK` → sample; `WAIT_TIMEOUT` → skip this frame;
  `WAIT_ABANDONED` → reallocate the slot; on `Release`: `ReleaseSync(0)`. The host never
  passes a non-zero timeout.
- `iosurface`: the view calls `QRhi::finish()` after `endFrame()` so the GPU work is
  complete before `FrameReady`; the host's reads are ordered by the `Displayed` rule
  above. An `MTLSharedEvent` timeline is an optimisation left for a device test.

Bounds: `maxPixelW/H = 8192`, slot bytes ≤ 256 MiB, three slots, one outstanding credit,
one connection per session, 64 KiB messages, 1 MiB socket read buffer. A view that
exceeds any of them is disconnected, never accommodated.

### 5. Pacing

A credit (`FrameRequest`) is **permission** to render into one slot, not an order. The
host grants one when **all** hold: the surface is visible (item visible, tab active,
window exposed); no credit is outstanding; a `Free` slot exists; and no `Ready` slot is
waiting to be painted. The grant is made as soon as those hold — right after a
`FrameReady`, right after the host takes a frame for display, right after a slot is
released — so a view with something to draw always already holds the slot for its next
frame while the host paints the current one, and no round trip sits on the critical
path. The view spends a credit only when its scene has changed since its last frame
(`QQuickRenderControl::sceneChanged` / `renderRequested`), or when the generation
changed, or when nothing of this generation has been shown yet; an idle view holds its
credit and renders nothing. `FrameWanted` is informational: the view says so when it has
demand and no credit, which can only mean the ring is full until the host paints.
`deadlineUs` is advisory (the host's refresh interval). `QQuickRenderControl` stays idle
while hidden exactly as a Wayland client does without frame callbacks (RFC-5 rule 1); a
view that is hidden while holding a credit may complete at most that one frame.

Two rules keep the coupling the way the contract wants it — *the view never renders
frames the host cannot show*:

- **One frame per host paint.** While a `Ready` frame waits, no credit is granted. The
  view therefore cannot run ahead of the host and have frames superseded; its rate is
  the host's paint rate (or its own, if slower). Granting on every free slot instead
  gives mailbox behaviour — newest frame wins, one frame less latency — at the cost of
  the view rendering about twice what is shown (measured 180 rendered for 100 shown);
  this RFC chooses the former.
- **Slots return after the paint.** A retired slot goes back to the view only after the
  host has painted its successor (`afterRendering`), because for `shm` the host's
  texture reads the mapping during that paint. A host that does not paint at all (an
  offscreen window nobody grabs) therefore holds the ring at its first frames until it
  does, which is why `surface_channel_test` asserts a steadily rising frame count with
  an animating view behind a painting host, and `test_surface_channel` asserts that an
  idle view holds its credit without rendering.

The ring needs exactly three slots for this: one `Displayed`, one `Granted` to the view,
and one in transit (`Ready` → `Displayed`, or `Retiring` → `Free`).

Revision note: rev 1 of this section required the view's `FrameWanted` before each
grant and granted only after the previous frame had been painted. Measured live, that
serialised host and view — each waited for the other's ~10 ms — and delivered ~30
frames/s from a view that renders in 5. With the standing credit the same view delivers
~90 frames/s, every one of them shown (RFC-7 §8).

### 6. Input

Input is a value protocol built from the host item's `QEvent`s and replayed in the view
with `QCoreApplication::sendEvent(producerWindow, event)`, the delivery
`QQuickRenderControl` documents. Coordinates are item-local logical pixels plus the
host's global position; timestamps are the host's `QEvent::timestamp()`.

| `kind` | Payload | Replayed as |
|---|---|---|
| 1–4 `MouseMove/Press/Release/DoubleClick` | `double x,y,gx,gy; uint32 button, buttons, modifiers; uint64 ts` | `QMouseEvent` (a move without buttons is hover for Qt Quick) |
| 5 `Wheel` | `double x,y,gx,gy; int32 pixelDx,pixelDy, angleDx,angleDy; uint32 buttons, modifiers, phase, inverted; uint64 ts` | `QWheelEvent` with pixel and angle deltas, phase and inversion |
| 6 `Touch` | `uint32 type (Begin/Update/End/Cancel), modifiers; uint64 ts; uint32 count; {int32 id; uint32 state; double x,y,gx,gy, pressure}[count]` | `QTouchEvent` with one `QEventPoint` per contact on a per-session `QPointingDevice` (touchscreen); whole frames only, never one event per finger |
| 7–8 `KeyPress/KeyRelease` | `int32 key; uint32 modifiers, nativeScanCode, nativeVirtualKey, nativeModifiers, autoRepeat; uint16 count, textLen; char16 text[]` | `QKeyEvent` |
| 9 `FocusIn`, 10 `FocusOut` | — | `QFocusEvent` on the window; `FocusOut` also cancels any active touch sequence |
| 11 `Leave` | — | `QEvent::Leave` (clears hover) |

What is preserved and what is not (RFC-6 §1 b): contact identity, state and position
including pressure; wheel pixel deltas and phases; key text and native codes. Not
carried in this revision: touch ellipse and velocity, pen tilt/rotation, native trackpad
gesture events (`QNativeGestureEvent`), IME preedit (`QInputMethodEvent`), clipboard and
drag-and-drop. These are additive message kinds; the matrix in RFC-6 §1 is the record.

### 7. Failure semantics (RFC-6 §1 e)

- The host never blocks on the view: no synchronous reads, no fence waits with a
  timeout other than zero, no allocation on the view's request.
- A protocol violation (bad magic/version/length, unknown slot, stale credit, wrong pid
  or token, second connection) closes the socket; `Logos.Surface` emits
  `error("protocolError")`, keeps the last displayed frame, and waits for a re-spawn under
  the same module like the Wayland backend does after `clientExited`.
- View death: `QLocalSocket::disconnected` → `error("clientExited")`; slots are freed.
- Host death: the view's `ui-host` already dies with its parent (RFC-1 lifecycle).
- Residual, by design of the tier: with `shm` on POSIX the view could truncate a
  POSIX-shm object it can open by name (SysV segments, Qt's default on Linux, cannot
  be resized); a hostile view can draw anything into a slot it holds; a view with GPU
  access can exhaust the GPU. These are addressed by OS confinement (RFC-5 rev 2 §4),
  not by the transport.

### 8. Selection and reporting

`SurfaceTransportSelector` (RFC-6 §3) decides `wayland` vs `channel` per spawn — since
RFC-6 rev 2.1 `channel` is the default on every platform, Linux included; the slot kind
is negotiated in `Hello`/`Welcome`. `Logos.Surface.transport` is `channel-shm`,
`channel-d3d11` or `channel-iosurface`.

Measured cost of `channel-shm` on the view side (`logos.surface.producer.debug=true`
prints one line per frame: scene-graph render, then `finishRender` = GPU→slot
readback): `maps_view` at 2400×1350 on a Radeon 8060S, 2.1 ms + 2.1 ms per frame under
continuous camera motion. Pacing is observable end to end with
`QT_LOGGING_RULES="logos.surface.host.debug=true;logos.surface.client.debug=true;basecamp.surface.channel.item.debug=true"`:
the host logs each grant (time since the last `FrameReady`) and each `FrameReady` (time
since its grant), the view logs credit→render and render time, and the Basecamp item
prints one line a second with frames arrived, frames shown, host swaps and the
arrival→paint wait. With the §5 rules, a 3 s `easeTo` at 1499×836 gave 90 frames/s
arrived, 90/s shown, 91/s host swaps, arrival→paint 5 ms — the view renders only what
the host paints, and the host paints everything the view renders.

## Tests

`logos-view-module-runtime/tests/test_surface_channel.cpp` (two processes, the test
binary re-executed as the view under `QT_QPA_PLATFORM=offscreen`, software Quick):

- handshake: wrong token, wrong pid and a `Hello` after the deadline are refused;
- a 320×200 view filled with a known colour arrives in an `shm` slot and the host's
  texture-side pixels match; a second `Configure` (resize + DPR 2) bumps the generation,
  re-attaches three slots of the new size and the next frame is 640×400;
- exactly one credit outstanding at any time; `FrameWanted` while hidden produces no
  credit, becoming visible produces one; `Release` never precedes the successor's display;
- input: a mouse press/release and a two-finger touch frame reach a `TapHandler` /
  `MultiPointTouchArea` in the view's QML and are reported back over the channel's test
  side-band;
- the view dying mid-frame and a view sending 1 MiB of garbage both end in a closed
  session with the host still painting.

`logos-basecamp/tests/surface_channel_test.cpp` is the item suite under
`LOGOS_SURFACE_TRANSPORT=shm` with a `QQuickWidget` host and the same in-binary client
(bind, `channel-shm` reported, host-driven size at DPR 2, frames keep flowing past the
three slots with an animating view, press in view coordinates, resize to a new
generation, quiet forget, view exit reported, wrong token refused, cross-module refusal).

The runtime suite also runs on the OpenGL path (`LOGOS_SURFACE_TEST_GL=1
LOGOS_SURFACE_TEST_QPA=wayland` on a desktop with a GL stack), which takes
`SurfaceProducer`'s `QQuickRenderControl` + RHI render-target route and
`ShmSlotStorage::finishRender`'s readback instead of `grabWindow()`; all cases pass there
too. `maps_view` over `channel-shm` was verified against a live Basecamp (the map
composited in the tab, frames at the host's rate); see the maps architecture §16.6 note
on what the headless harness can show.

## Status

Implemented in `logos-view-module-runtime` (`surface/`, `include/logos/view_surface.h`)
and `logos-basecamp` (`app/surface/ChannelSurfaceBackend.{h,cpp}`,
`SurfaceTransportSelector.{h,cpp}`) on the `rfc/view-surface` branches; see the RFC
README's PR table for commits. `shm` slots are tested on Linux as described above.
`D3D11SlotStorage` and `IOSurfaceSlotStorage` follow §4 against the public APIs cited
there but **have not been compiled or run** in this workspace (no Windows or macOS
build host); each file says so at the top and the PR text repeats it.

§5 is at rev 2 (standing credit, one frame per host paint) after live measurement of
rev 1; the wire protocol is unchanged — `FrameRequest`, `FrameReady`, `Release` and
`FrameWanted` keep their encoding — only when the host sends a credit and when the view
spends one changed. Mixed revisions still talk: a rev 2 view against a rev 1 host paces
as rev 1 did; a rev 1 view against a rev 2 host spends every credit it is handed, so it
redraws at the host's paint rate while idle (as a Wayland client that redraws on every
frame callback would) until its runtime is updated. Both ends normally come from the
same `LogosViewSurface` library.
