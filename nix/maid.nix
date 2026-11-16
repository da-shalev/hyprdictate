# nix-maid module: runs the daemon as a user service in the graphical session
# and puts the hyprdictate CLI on PATH. Bind `hyprdictate toggle` to a key.
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

    lookahead = lib.mkOption {
      type = lib.types.int;
      default = 6;
      description = "Encoder lookahead in 80 ms frames (0, 3, 6 or 13): lower is faster, higher more accurate.";
    };
  };

  config = lib.mkIf cfg.enable {
    packages = [ cfg.package ];

    systemd.services.hyprdictate = {
      description = "hyprdictate dictation input method";
      wantedBy = [ config.maid.systemdGraphicalTarget ];
      partOf = [ config.maid.systemdGraphicalTarget ];
      after = [
        config.maid.systemdGraphicalTarget
        "pipewire.service"
      ];
      serviceConfig = {
        ExecStart = "${lib.getExe cfg.package} daemon --model ${cfg.model} --language ${cfg.language} --lookahead ${toString cfg.lookahead}";
        Restart = "on-failure";
        RestartSec = 2;
      };
    };
  };
}
