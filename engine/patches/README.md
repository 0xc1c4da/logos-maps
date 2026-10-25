# Patch set against maplibre-native-qt (docs/architecture.md §14.1)

Base: `maplibre/maplibre-native-qt` commit c3485f3 (vendoring maplibre-native
702ebc6). Every patch touches the Qt *binding* only; the core is never patched.
File name = patch id; the engine derivation defines
`MAPS_ENGINE_HAS_<ID>` in `include/QMapLibre/qmaplibre_patchset.h` for every
patch it applied (`P1_QUERY_RENDERED_FEATURES`, `P4_ADD_IMAGE_METADATA`, …),
which is how `maps_view` fills the capability report (§11.1).

| File | Adds | Status |
|---|---|---|
| `p1-query-rendered-features.patch` | `Map::queryRenderedFeatures(QRectF/QPointF, layerIds, filter)`, `Map::querySourceFeatures(sourceId, sourceLayers, filter)` → GeoJSON `FeatureCollection` bytes; features carry `source`, `sourceLayer`, `state` | mandatory (M14.1) |
| `p4-add-image-metadata.patch` | `Map::addImage(id, image, pixelRatio, sdf, stretchX, stretchY, content)` | mandatory |
| `p5-file-source-factory-hook.patch` | `Settings::setFileSourceFactoryHook(std::function<void(mbgl::FileSourceManager&)>)`, called before a `Map` builds its file sources; installs the header-only `mapbox/*` deps the installed `mbgl` headers need; CMake option `MLN_QT_WITH_EXPORTED_CORE` (default OFF, our derivation sets ON) builds the vendored core with default visibility so `mbgl::FileSource` can be implemented outside `libQMapLibre` | mandatory |
| `p7-export-texture-node.patch` | installs `QMapLibreQuickPrivate/{export_quick_p,texture_node_base_p,texture_node_opengl_p}.hpp` and adds the directory to the `QMapLibre::QuickPrivate` install interface | mandatory |
| `p11-texture-node-fbo-restore.patch` | `TextureNodeOpenGL::render()` saves the scene graph's framebuffer binding *before* `updateRenderer()` binds the map's and restores it *before* `endExternalCommands()`; calls `updateRenderer()` only when size, pixel ratio or FBO changed (the backend re-allocates the colour texture on every call); re-wraps the `QSGTexture` only when the texture id or size changed and deletes the previous wrapper | mandatory |
| `p12-shared-context-program-rebind.patch` | `OpenGLRendererBackend::updateAssumedState()` forgets the cached program (`gl::Context::program` value 0) so `DrawableGL::draw()` re-binds the shader every frame of a shared context | mandatory |
| `p9-cache-path-prefetch.patch` | `setCacheDatabasePath("")` → on-disk cache under `QStandardPaths::CacheLocation/QMapLibre/cache.db`; `Map::setPrefetchZoomDelta`, `Map::setDebugOptions` | optional |
| `p2-feature-state.patch` | `Map::setFeatureState / getFeatureState / removeFeatureState` | optional, needs P1 |
| `p6-layer-zoom-visibility.patch` | `Map::setLayerZoomRange`, `setLayerVisibility`, `getLayerJson`, `getSourceJson` | optional, needs P1 |
| `p10-placement-transitions.patch` | `Map::setPlacementTransitionsEnabled(bool)` / `placementTransitionsEnabled()`; exposes `TransitionOptions::enablePlacementTransitions` | optional (idle guarantee) |

P3 (parsed GeoJSON / patch update) and P8 (custom geometry source) are
deferred (§14.1, O3).

## Order and independence

`engine/flake.nix` applies them in the order of the table. P1, P4, P5, P7, P11 and
P12 each apply on their own against the base; P2 and P6 share P1's renderer
forwarding and JSON writer and therefore need P1. Any of the optional patches
can be dropped without breaking the others (verified with `patch -p1` on the
pristine tree for every single-drop subset).

## Behaviour notes (for consumers and for the upstream PRs)

- P1 runs the query on the *calling* thread, holding `MapPrivate`'s renderer
  mutex, which `render()` also holds. That is the same guarantee the Android
  frontend gets from `actor->ask()` (no frame in flight while the query reads
  the orchestrator) without requiring an event loop on the render thread.
  Results reflect the last rendered frame; before `createRenderer()` the
  collection is empty.
- `querySourceFeatures` returns one copy of a feature per loaded tile that
  contains it (core semantics; de-duplicate by id if needed).
- P2 `removeFeatureState` coalesces on the next frame, so `getFeatureState`
  still reports the old value until a frame has been rendered (core semantics).
- P5's hook is useless on an unpatched build: the binding compiles the
  vendored core with `-fvisibility=hidden`, so `mbgl::Response`,
  `mbgl::util::RunLoop`, `mbgl::platform::now`, … are not in
  `libQMapLibre.so`'s export table and an out-of-tree `FileSource` fails to
  link (found while linking `view/tests`). `MLN_QT_WITH_EXPORTED_CORE` mirrors
  the core's own `MLN_CREATE_AMALGAMATION` visibility override and is kept
  opt-in so the default ABI surface upstream is unchanged.
- P10 exists because, with placement transitions on (the core default), the
  orchestrator flags the symbol placement *stale* on every frame it does not
  recompute it and reports that as "needs repaint"; `Map::Impl` then
  schedules the next frame, forever, even with no symbol layers. maps_view
  turns the transitions off after every style load so an idle map renders
  zero frames (M11.2); the trade-off is that symbols pop instead of fading.
  The flag is reset by each style load.
- P9 keeps the upstream default (`:memory:`); the on-disk cache is opt-in via
  an empty path so that the change is acceptable upstream.
- P11 is the texture node's framebuffer discipline: `render()` captured
  `GL_FRAMEBUFFER_BINDING` after `updateRenderer()` had already bound the
  map's FBO, so the "restore" at the end re-bound the map's FBO, and did so
  after `endExternalCommands()`, behind the scene graph's state resync. It
  also stops the per-frame colour-texture re-allocation (`updateRenderer()`
  deletes and re-creates the texture on every call) and the per-frame
  `QSGTexture` wrapper leak. Found while reading the texture back for the
  §16.7 snapshot on Qt 6.9.2 (QQuickWindow on Wayland, and offscreen under
  Xvfb).
- P12 is why the map used to render one correct frame and then go uniformly
  white (raster) or transparent (background) until the next style or tile
  change: `DrawableGL::draw()` compares the shader's program against the
  cached *value* of the program state and assigns only when it differs, so
  the dirty flag `setDirtyState()` raises for a shared context is never
  consulted and `glUseProgram` is skipped; the drawables are rendered with
  whatever program Qt left current. The backend's `updateAssumedState()`
  now forgets the program (value 0) at the start of every frame. The
  core-side fix is a plain assignment in `DrawableGL::draw()`; the patch
  keeps to the binding (§14.1 policy). Diagnosed by dumping
  `GL_CURRENT_PROGRAM` after `Map::render()`: Qt's program on every bad
  frame, the map's on every good one.

## Regenerating

The patches are plain `git diff` output over a scratch clone of the base
commit with one commit per patch; keep that shape (one subject line, a short
rationale, `Upstream base:` line, `---`, diffstat, diff) when editing them so
they can be turned into upstream PRs with `git am`-style tooling or by hand.
