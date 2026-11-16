# Entry point without flakes: `(import ./.)` gives the overlay (hyprdictate,
# transcribe-cpp) and the nix-maid module.
{
  overlays.default = final: prev: {
    transcribe-cpp = final.callPackage ./nix/transcribe-cpp.nix { };
    hyprdictate = final.callPackage ./nix/package.nix { };
  };

  maidModules.default = ./nix/maid.nix;
}
