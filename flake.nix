{
  description = "Streaming dictation into the focused text field, as a Wayland input method";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forSystems = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      inherit (import ./.) overlays maidModules;

      packages = forSystems (
        pkgs:
        let
          own = pkgs.extend self.overlays.default;
        in
        {
          inherit (own) hyprdictate transcribe-cpp;
          default = own.hyprdictate;
        }
      );

      devShells = forSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.hyprdictate ];
          packages = [ pkgs.clang-tools ];
        };
      });
    };
}
