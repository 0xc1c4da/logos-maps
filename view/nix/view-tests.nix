# maps_view conformance suites (docs/architecture.md section 16.1 engine
# contract, section 16.3 provider suite) and the dev launcher.
#
# The view's own sources (src/view, src/providers) are compiled straight into
# the test binaries against the engine and the SDK's Qt -- no builder, no
# generated glue, no IPC. Rendering is headless through Mesa's llvmpipe, so
# the suite runs inside the nix sandbox. The recipe that works (all three
# parts are needed, see docs/architecture.md section 16.1 notes):
#   * an Xvfb display with GLX: Qt's `offscreen` QPA only offers OpenGL
#     through its X11 variant (QOffscreenX11Integration), which needs DISPLAY;
#   * `QT_QUICK_BACKEND=rhi`: `offscreen` does not advertise RhiBasedRendering,
#     so Qt Quick would otherwise pick the software scene graph and MapItem
#     would have no GL context;
#   * Mesa reachable without /run/opengl-driver: `__GLX_VENDOR_LIBRARY_NAME`,
#     `__EGL_VENDOR_LIBRARY_DIRS` for libglvnd, `LIBGL_DRIVERS_PATH` for the
#     rasteriser.
#
#   nix build .#view-tests        # builds and runs the suites (checkPhase)
#   nix build .#dev               # the launcher without running the suites
#   nix flake check               # same as the first, as checks.unit-tests
{ pkgs, engine, src }:

let
  inherit (pkgs) lib stdenv;
  qt = pkgs.qt6;
  pmtilesInclude = "${engine.src}/vendor/maplibre-native/vendor/PMTiles/cpp";

  # GL without a desktop: Mesa's GLX/EGL vendor files and DRI drivers must be
  # reachable without /run/opengl-driver, which the sandbox does not have.
  # Qt from nixpkgs logs to journald unless QT_FORCE_STDERR_LOGGING is set.
  glEnv = ''
    export QT_QPA_PLATFORM=offscreen
    export QT_QUICK_BACKEND=rhi
    export QT_FORCE_STDERR_LOGGING=1
    export QT_QPA_FONTDIR=${pkgs.dejavu_fonts}/share/fonts/truetype
    export __GLX_VENDOR_LIBRARY_NAME=mesa
    export __EGL_VENDOR_LIBRARY_DIRS=${pkgs.mesa}/share/glvnd/egl_vendor.d
    export LIBGL_DRIVERS_PATH=${pkgs.mesa}/lib/dri
    export LD_LIBRARY_PATH=${pkgs.mesa}/lib:${pkgs.libglvnd}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    export LIBGL_ALWAYS_SOFTWARE=1
    export GALLIUM_DRIVER=llvmpipe
    export HOME=$TMPDIR
    export XDG_CACHE_HOME=$TMPDIR/cache
    export XDG_RUNTIME_DIR=$TMPDIR/runtime
    mkdir -p $XDG_CACHE_HOME $XDG_RUNTIME_DIR
  '';

  base = stdenv.mkDerivation {
    pname = "maps-view-tests";
    version = "0.1.0";
    inherit src;
    sourceRoot = "source/tests";

    nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.xvfb-run ];
    buildInputs = [
      qt.qtbase
      qt.qtdeclarative
      qt.qtremoteobjects
      engine
      pkgs.sqlite
      pkgs.libglvnd
      pkgs.mesa
    ];
    # qtbase's setup hook wraps nothing here; the binaries are run in place.
    dontWrapQtApps = true;

    cmakeFlags = [
      "-DMAPS_VIEW_PMTILES_INCLUDE=${pmtilesInclude}"
      "-DMAPS_VIEW_BUILD_DEV=ON"
      "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
    ];

    doCheck = true;
    checkPhase = ''
      runHook preCheck
      ${glEnv}
      export MAPS_VIEW_QML_DIR=$NIX_BUILD_TOP/source/src/qml
      xvfb-run -a -s "-screen 0 1280x1024x24 +extension GLX" \
        ctest --output-on-failure --timeout 300
      runHook postCheck
    '';

    # The checks are the point of this derivation; the installed binaries let
    # a developer re-run a suite outside the sandbox (with the same env).
    postInstall = ''
      mkdir -p $out/bin
      cat > $out/bin/maps-view-tests <<EOF
      #!${pkgs.runtimeShell}
      ${glEnv}
      export MAPS_VIEW_QML_DIR=$out/libexec/maps-view-tests/qml
      status=0
      for t in $out/libexec/maps-view-tests/test_*; do
        echo "== \$t"
        ${pkgs.xvfb-run}/bin/xvfb-run -a -s "-screen 0 1280x1024x24 +extension GLX" "\$t" || status=1
      done
      exit \$status
      EOF
      chmod +x $out/bin/maps-view-tests
    '';

    passthru = {
      inherit glEnv pmtilesInclude;
      # The launcher alone: same build, no suites, so a broken test never
      # blocks running the view.
      devLauncher = base.overrideAttrs (_: {
        pname = "maps-view-dev";
        doCheck = false;
        postInstall = ''
          # ViewWindow::qmlDirectory() is <binary dir>/qml; installed beside bin/.
          wrapProgram $out/bin/maps_view_dev \
            --set-default QT_QPA_PLATFORM xcb \
            --prefix QML_IMPORT_PATH : ${qt.qtdeclarative}/${qt.qtbase.qtQmlPrefix} \
            --prefix QT_PLUGIN_PATH : ${qt.qtbase}/${qt.qtbase.qtPluginPrefix}:${qt.qtdeclarative}/${qt.qtbase.qtPluginPrefix}:${qt.qtwayland}/${qt.qtbase.qtPluginPrefix}
        '';
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config pkgs.makeWrapper ];
        buildInputs = [ qt.qtbase qt.qtdeclarative qt.qtremoteobjects qt.qtwayland engine pkgs.sqlite pkgs.libglvnd ];
      });
    };

    meta = with lib; {
      description = "maps_view conformance suites (sections 16.1, 16.3) and dev launcher";
      platforms = platforms.linux;
    };
  };
in
base
