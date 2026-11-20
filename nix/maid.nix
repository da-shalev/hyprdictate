# nix-maid module: writes hyprdictate.conf and runs the package's user unit in
# the graphical session; puts the hyprdictate CLI on PATH. Bind
# `hyprdictate --toggle` to a key.
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.hyprdictate;
in
{
  options.hyprdictate = {
    enable = lib.mkEnableOption "hyprdictate, streaming dictation into the focused text field";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.callPackage ./package.nix { transcribe-cpp = pkgs.callPackage ./transcribe-cpp.nix { }; };
    };

    model = lib.mkOption {
      type = lib.types.path;
      description = "A streaming transcribe.cpp GGUF, e.g. Nemotron 3.5 ASR Streaming from handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf.";
    };

    language = lib.mkOption {
      type = lib.types.str;
      default = "en-US";
    };

    latency = lib.mkOption {
      type = lib.types.int;
      default = 480;
      description = "How soon words appear after they are spoken, in milliseconds: lower is quicker, higher more accurate. -1 keeps the model's default.";
    };
  };

  config = lib.mkIf cfg.enable {
    packages = [ cfg.package ];

    # The settings, where hyprdictate reads them (format: assets/example.conf).
    file.xdg_config."hypr/hyprdictate.conf".text = ''
      general {
          model = ${cfg.model}
          language = ${cfg.language}
          latency = ${toString cfg.latency}
      }
    '';

    # The package's own unit (systemd/hyprdictate.service.in), started with the
    # graphical session and again when the settings change.
    systemd.packages = [ cfg.package ];
    systemd.services.hyprdictate = {
      wantedBy = [ config.maid.systemdGraphicalTarget ];
      restartTriggers = [ config.file.xdg_config."hypr/hyprdictate.conf".text ];
    };
  };
}
