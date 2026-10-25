{
  description = "maps-engine - maplibre-native-qt built against the Logos SDK's Qt, with the Maps patch set";

  inputs = {
    # Same nixpkgs/Qt pin as every Logos module: the Qt scene graph and QtRO
    # wire are Qt-version-sensitive, so the engine must link the one Qt in
    # the closure (docs/architecture.md M14.3; `ws check-qt`). Inside
    # logos-workspace this follows the workspace's logos-nix.
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
  };

  outputs = { self, nixpkgs, logos-nix }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
      });

      # docs/architecture.md §14.1. Order matters only for overlapping hunks;
      # P1/P4/P5/P7/P11/P12 are mandatory (M14.1), the rest are capability-gated and
      # may be dropped from this list without breaking maps_view.
      patchSet = [
        ./patches/p1-query-rendered-features.patch
        ./patches/p4-add-image-metadata.patch
        ./patches/p5-file-source-factory-hook.patch
        ./patches/p7-export-texture-node.patch
        ./patches/p11-texture-node-fbo-restore.patch
        ./patches/p12-shared-context-program-rebind.patch
        ./patches/p9-cache-path-prefetch.patch
        ./patches/p2-feature-state.patch
        ./patches/p6-layer-zoom-visibility.patch
        ./patches/p10-placement-transitions.patch
      ];
      presentPatches = builtins.filter (p: builtins.pathExists p) patchSet;
    in
    {
      packages = forAllSystems ({ pkgs, ... }: rec {
        maplibre-native-qt = pkgs.callPackage ./maplibre-native-qt.nix {
          patches = presentPatches;
        };
        # Unpatched reference build, for bisecting a patch against upstream.
        maplibre-native-qt-upstream = pkgs.callPackage ./maplibre-native-qt.nix { patches = [ ]; };
        default = maplibre-native-qt;
      });

      # What logos-module-builder's `externalLibInputs` consumes: a derivation
      # per variant (docs/external-libraries.md). `portable` is the same
      # build; the view's .lgx bundles the shared libraries beside the plugin.
      lib.externalLib = forAllSystems ({ pkgs, system, ... }: {
        default = self.packages.${system}.maplibre-native-qt;
        portable = self.packages.${system}.maplibre-native-qt;
      });

      devShells = forAllSystems ({ pkgs, system, ... }: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.maplibre-native-qt ];
          packages = [ pkgs.git pkgs.quilt ];
        };
      });
    };
}
