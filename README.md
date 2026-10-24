# Maps

A [Logos Basecamp](https://github.com/logos-co/logos-basecamp) app: a QML view
(`src/qml/Main.qml`) with a process-isolated C++ backend (`src/maps_ui_backend.*`),
built with [logos-module-builder](https://github.com/logos-co/logos-module-builder).
Module name: `maps_ui`.

## Layout

| Path | Role |
|---|---|
| `metadata.json` | name, version, type, dependencies, codegen (single source of truth) |
| `src/maps_ui.rep` | view <-> backend contract (Qt Remote Objects); SLOTs are callable from QML, PROPs sync to the view |
| `src/maps_ui_backend.{h,cpp}` | the only hand-written C++; everything else is generated |
| `src/qml/Main.qml` | the view, styled with the Logos design system (`Logos.Theme`, `Logos.Controls`) |
| `src/icons/icon.png` | 256x256 app icon |

## Build

```bash
nix build                # result/: plugins/maps_ui/...
nix build .#lgx-portable # distributable .lgx (linux-amd64)
```

Nix only sees git-tracked files: `git add` new sources and assets before building.

## Run in Basecamp (development)

```bash
nix run .          # opens the module in Basecamp (LOGOS_BASECAMP picks the host flake)
nix run .#ui-dev   # same, with QML hot reload from src/qml
nix run .#ui-tests # tests/*.mjs, headless
```

Inside [logos-workspace](https://github.com/logos-co/logos-workspace) use
`ws build logos-maps` and `nix run $LOGOS_WORKSPACE_ROOT#logos-maps--ui-dev`, which build
against the workspace's shared SDK generation.

## Rules of the road

- Keep `.rep` slots to at most 4 arguments; pass structured data as one JSON string.
- Render peer-supplied text with `textFormat: Text.PlainText`.
- Logic that must also run headless belongs in a separate `type: core` module
  (`maps_core`), listed in `dependencies`; the view stays thin.
