{
  description = "maps_view - the Maps view: QML with a process-isolated C++ backend over maps_core";

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
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
