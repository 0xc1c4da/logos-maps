{
  description = "maps_view - the Maps view: MapLibre Native in a process-isolated C++ backend over maps_core";

  inputs = {
    # Pinned to a full commit so a from-scratch build is reproducible. Inside
    # logos-workspace this input is overridden by `follows`, so the module is
    # built against the workspace's builder/SDK generation like every other.
    logos-module-builder.url = "github:logos-co/logos-module-builder/8f56bf8b769bb35cb54d05aeeb9c5771dba1c9bd";

    # The core this view renders (metadata.json "dependencies"). The input
    # name must equal the module name so the builder generates the typed
    # modules().maps_core client and bundles the core into the dev launcher.
    # Resolvable from any clone (the published core/ of this repo); for a
    # local core edit, build with
    #   nix build --override-input maps_core path:../core
    # In logos-workspace this follows the `maps-core` workspace input.
    maps_core = {
      url = "github:0xc1c4da/logos-maps?dir=core";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };

    # The rendering engine: maplibre-native-qt with the docs/architecture.md
    # section 14 patch set, built against the SDK's Qt (engine/flake.nix).
    # Exposed to the module build as two external libraries, libQMapLibre and
    # libQMapLibreQuickPrivate, both from this one derivation.
    maps-engine = {
      url = "github:0xc1c4da/logos-maps?dir=engine";
    };
  };

  outputs = inputs@{ self, logos-module-builder, maps-engine, ... }:
    let
      base = logos-module-builder.lib.mkLogosQmlModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs = {
          QMapLibre = maps-engine;
          QMapLibreQuickPrivate = maps-engine;
        };
        # The plugin's DT_NEEDED names the versioned sonames; the builder
        # stages the unversioned .so beside the plugin. Add the soname links so
        # $ORIGIN resolution works wherever the lib/ directory is copied to
        # (section 14.5: the engine ships inside the maps_view package).
        postInstall = ''
          for lib in QMapLibre QMapLibreQuickPrivate; do
            if [ -f "$out/lib/lib$lib.so" ]; then
              for v in 4 4.0.0; do
                [ -e "$out/lib/lib$lib.so.$v" ] || ln -s "lib$lib.so" "$out/lib/lib$lib.so.$v"
              done
            fi
          done
        '';
      };

      nixpkgs = logos-module-builder.inputs.nixpkgs;
      lib = nixpkgs.lib;
      linuxSystems = builtins.filter (lib.hasSuffix "-linux") (builtins.attrNames base.packages);

      # section 16.1 / section 16.3 conformance suites: offscreen Qt Quick with
      # llvmpipe, the view's own sources compiled straight into the test
      # binaries (no plugin glue, no IPC).
      viewTests = system:
        import ./nix/view-tests.nix {
          pkgs = import nixpkgs { inherit system; };
          engine = maps-engine.packages.${system}.default;
          src = self;
          # The header-only RFC-8 module API, read from the runtime the
          # builder pins (the plugin build gets the same file through
          # metadata.json view_surface: true). No runtime package is built.
          viewSurfaceInclude = "${logos-module-builder.inputs.logos-view-module-runtime}/include";
        };
    in
      base // {
        checks = base.checks // lib.genAttrs linuxSystems (system: base.checks.${system} // {
          unit-tests = viewTests system;
        });
        packages = base.packages // lib.genAttrs linuxSystems (system: base.packages.${system} // {
          # `nix build .#dev`: the dev launcher (devtools/maps_view_dev), a
          # QGuiApplication that opens MapWindow.qml as a top-level window
          # and drives a maps_core under logosctl (surface.transport: none).
          dev = (viewTests system).passthru.devLauncher;
          view-tests = viewTests system;
        });
      };
}
