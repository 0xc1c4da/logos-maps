{
  description = "Maps - a Logos Basecamp app: QML view with a process-isolated C++ backend";

  inputs = {
    # Pinned to a full commit so a from-scratch build is reproducible. Inside
    # logos-workspace this input is overridden by `follows`, so the module is
    # built against the workspace's builder/SDK generation like every other.
    logos-module-builder.url = "github:logos-co/logos-module-builder/8f56bf8b769bb35cb54d05aeeb9c5771dba1c9bd";
    # Core-module dependencies go here, one input per metadata.json "dependencies"
    # entry, e.g.  maps_core.url = "github:0xc1c4da/logos-maps-core";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
