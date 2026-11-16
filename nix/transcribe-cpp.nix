# transcribe.cpp as a shared library with its headers, on the GPU through
# ggml's Vulkan backend.
{
  lib,
  stdenv,
  callPackage,
  cmake,
  ninja,
  shaderc,
  spirv-headers,
  vulkan-headers,
  vulkan-loader,
}:
let
  # Pinned by nvfetcher (nvfetcher.toml).
  source = (callPackage ./_sources/generated.nix { }).transcribe-cpp;
in
stdenv.mkDerivation {
  pname = "transcribe-cpp";
  version = lib.removePrefix "v" source.version;
  inherit (source) src;

  nativeBuildInputs = [
    cmake
    ninja
    # glslc, for ggml's Vulkan shaders.
    shaderc
  ];

  buildInputs = [
    spirv-headers
    vulkan-headers
    vulkan-loader
  ];

  cmakeFlags = [
    (lib.cmakeBool "TRANSCRIBE_VULKAN" true)
    (lib.cmakeBool "TRANSCRIBE_BUILD_SHARED" true)
    (lib.cmakeBool "TRANSCRIBE_INSTALL" true)
    (lib.cmakeBool "TRANSCRIBE_BUILD_TESTS" false)
    (lib.cmakeBool "TRANSCRIBE_BUILD_EXAMPLES" false)
    # The CPU only runs the small host-side decoder; the model is on the GPU.
    (lib.cmakeBool "TRANSCRIBE_USE_SYSTEM_BLAS" false)
    # No -march=native: the build machine's CPU isn't the user's.
    (lib.cmakeBool "GGML_NATIVE" false)
  ];

  meta = {
    description = "Speech-to-text inference library on ggml";
    homepage = "https://github.com/handy-computer/transcribe.cpp";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
  };
}
