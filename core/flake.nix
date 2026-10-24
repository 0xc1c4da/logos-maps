{
  description = "maps_core - the headless Maps core module (type: core)";

  inputs = {
    # Pinned to a full commit so a from-scratch build is reproducible. Inside
    # logos-workspace this input is overridden by `follows`.
    logos-module-builder.url = "github:logos-co/logos-module-builder/8f56bf8b769bb35cb54d05aeeb9c5771dba1c9bd";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
