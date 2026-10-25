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
| `p5-file-source-factory-hook.patch` | `Settings::setFileSourceFactoryHook(std::function<void(mbgl::FileSourceManager&)>)`, called before a `Map` builds its file sources; installs the header-only `mapbox/*` deps the installed `mbgl` headers need | mandatory |
| `p7-export-texture-node.patch` | installs `QMapLibreQuickPrivate/{export_quick_p,texture_node_base_p,texture_node_opengl_p}.hpp` and adds the directory to the `QMapLibre::QuickPrivate` install interface | mandatory |
| `p9-cache-path-prefetch.patch` | `setCacheDatabasePath("")` → on-disk cache under `QStandardPaths::CacheLocation/QMapLibre/cache.db`; `Map::setPrefetchZoomDelta`, `Map::setDebugOptions` | optional |
| `p2-feature-state.patch` | `Map::setFeatureState / getFeatureState / removeFeatureState` | optional, needs P1 |
| `p6-layer-zoom-visibility.patch` | `Map::setLayerZoomRange`, `setLayerVisibility`, `getLayerJson`, `getSourceJson` | optional, needs P1 |

P3 (parsed GeoJSON / patch update) and P8 (custom geometry source) are
deferred (§14.1, O3).

## Order and independence

`engine/flake.nix` applies them in the order of the table. P1, P4, P5 and P7
each apply on their own against the base; P2 and P6 share P1's renderer
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
- P9 keeps the upstream default (`:memory:`); the on-disk cache is opt-in via
  an empty path so that the change is acceptable upstream.

## Regenerating

The patches are plain `git diff` output over a scratch clone of the base
commit with one commit per patch; keep that shape (one subject line, a short
rationale, `Upstream base:` line, `---`, diffstat, diff) when editing them so
they can be turned into upstream PRs with `git am`-style tooling or by hand.
