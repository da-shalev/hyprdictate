{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  hyprwayland-scanner,
  hyprwire,
  hyprutils,
  pipewire,
  wayland,
  wayland-scanner,
  transcribe-cpp,
}:
stdenv.mkDerivation {
  pname = "hyprdictate";
  version = lib.trim (builtins.readFile ../VERSION);

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../VERSION
      ../CMakeLists.txt
      ../src
      ../protocols
    ];
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    hyprwayland-scanner
    # hyprwire-scanner
    hyprwire
    # wayland.xml
    wayland-scanner
  ];

  buildInputs = [
    hyprwire
    hyprutils
    pipewire
    wayland
    transcribe-cpp
  ];

  meta = {
    description = "Streaming dictation into the focused text field, as a Wayland input method";
    license = lib.licenses.mit;
    mainProgram = "hyprdictate";
    platforms = lib.platforms.linux;
  };
}
