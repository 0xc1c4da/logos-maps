# Maps

A [Logos Basecamp](https://github.com/logos-co/logos-basecamp) app: a programmable
MapLibre map that other modules draw on. Built with
[logos-module-builder](https://github.com/logos-co/logos-module-builder).

The design of record is [`docs/architecture.md`](docs/architecture.md). Platform
changes it needs (an out-of-process GPU surface composited into a Basecamp tab) are
proposed in [`docs/rfc/`](docs/rfc/).

| Directory | Module / artefact | Type | Role |
|---|---|---|---|
| `core/` | `maps_core` | `core` (headless, Qt-free) | Owns the retained scene (sessions, sources, layers, images), the basemap catalogue, the provider registry and the capability report. Any module can `applyBatch` and follow its events. Runs under Basecamp or `logosctl` alike. |
| `view/` | `maps_view` | `ui_qml` + C++ backend, `view_surface` | Renders the scene with MapLibre Native in its own process (`ui-host`), owns gestures and picking, and is composited into the host through `Logos.Surface`. |
| `engine/` | `maplibre-native-qt` | Nix derivation | The engine: MapLibre Native Qt binding plus the patch set in `engine/patches/`. |

The split is what makes the map programmable: a Basecamp view is not addressable by
other modules, so everything another module should be able to call lives in `maps_core`
(spec §4). `maps_view` subscribes to the core's scene revisions and publishes
interaction events through it.

## Module API (`maps_core`)

See spec §12.1. Every method takes at most four arguments; structured payloads are JSON
strings with a top-level `"v": 1`. The only scene mutator is

```
applyBatch(session, batchJson) -> {"revision", "accepted", "rejected"}
```

with ops `putSource`, `replaceGeoJson`, `patchFeatures`, `putImage`, `putLayer`,
`setPaint`, `setLayout`, `setFilter`, `placeLayer`, `removeLayer`, `removeSource`,
`removeImage`, `dropSession` (spec §6.4). Basemaps are JSON descriptors (spec §9.1,
Appendix C); camera intents are addressed to a `viewId` (spec §7).

From another module, declare `"dependencies": ["maps_core"]` and call
`modules().maps_core.applyBatch(session, json)`, or from QML
`logos.callModuleAsync("maps_core", "applyBatch", [session, json], cb)`.

## Build

```bash
nix build ./core                                              # result/: plugins/maps_core/...
nix build ./engine                                            # maplibre-native-qt with patches
nix build ./view --override-input maps_core path:$PWD/core    # the view, against the local core
```

Nix only sees git-tracked files: `git add` new sources and assets before building.

## Test

```bash
nix build ./core#checks.x86_64-linux.unit-tests               # scene replay, catalogue, schema (spec §16.2)
nix build ./view#checks.x86_64-linux.unit-tests               # engine contract, providers, gestures, picking (offscreen)
```

## Run in Basecamp (development)

```bash
cd view
nix run . --override-input maps_core path:$PWD/../core          # opens the view + core in Basecamp
nix run .#ui-dev --override-input maps_core path:$PWD/../core   # same, with QML hot reload
```

Inside [logos-workspace](https://github.com/logos-co/logos-workspace) the modules are
the inputs `maps-core`, `maps-view` and `maps-engine`; overrides are automatic there:

```bash
ws build --group own
nix run "$LOGOS_WORKSPACE_ROOT#maps-view--ui-dev" -- --user-dir /tmp/bc-maps
```

Headless, with `logosctl`: install `maps_core`'s `.lgx`, then
`logosctl call maps_core applyBatch str:demo/pins @batch.json` (see `core/tests/logosctl/`).

## Rules of the road

- Only `bool`, `int64_t`, `std::string` and std containers of them cross the module
  boundary; structured data travels as JSON text. Keep methods and `.rep` slots to
  at most 4 arguments.
- `metadata.json` descriptions: ASCII only, no semicolons (CMake splits on them).
- Never call a module synchronously from inside one of its event callbacks or from
  `onContextReady()`: the call blocks until the timeout and returns a default.
  Defer it to the next event-loop turn (`QTimer::singleShot`), as the view backend does
  (spec M12.9).
- Render peer-supplied text with `textFormat: Text.PlainText`.
- `maps_core` keeps scene state in memory; session owners re-submit on `viewOpened`
  (spec O6).
- `references/` is a private research corpus and is never pushed.
