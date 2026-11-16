# hyprdictate

Streaming dictation into the focused text field on Wayland: words appear as you speak, transcribed locally on your GPU.

Works on compositors with the `zwp_input_method_v2` protocol: Hyprland and wlroots-based ones such as sway, river and labwc. **It does not work on GNOME or KDE**, which don't support that protocol.

This project is written and maintained with AI assistance.

# Features
 - Types into any text field through the Wayland input-method protocol, without faking key presses
 - Streams: settled words are committed as you speak, words still being decided show as a preedit
 - Local speech recognition on the GPU through Vulkan, with [transcribe.cpp](https://github.com/handy-computer/transcribe.cpp)
 - The microphone is only open while dictating

# Usage

Download a streaming transcribe.cpp model, e.g. `nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf` from
[Nemotron 3.5 ASR Streaming](https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf).

Start the daemon with your session and bind a key to toggle dictation, e.g. in Hyprland:

```ini
exec-once = hyprdictate --model ~/models/nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf --language en-US
bind = SUPER, D, exec, hyprdictate --toggle
```

`hyprdictate --help` lists the options.

Requirements:
 - A compositor with `zwp_input_method_v2` (Hyprland, sway, river, labwc; not GNOME or KDE), and no other input method running
 - Apps with `text_input_v3` (GTK, foot; Chromium with `--enable-wayland-ime`)
 - PipeWire

# Installation

## Nix

`default.nix` and the flake export `overlays.default` and `maidModules.default`, a
[nix-maid](https://github.com/viperML/nix-maid) module that runs the daemon in the graphical session:

```nix
maid.sharedModules = [ (import hyprdictate).maidModules.default ];
# per user:
hyprdictate = {
  enable = true;
  model = ./nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf;
  # also: package, language, lookahead
};
```

## Manual

### Dependencies
 - wayland, wayland-scanner
 - pipewire
 - hyprutils, hyprwire, hyprwayland-scanner
 - [transcribe.cpp](https://github.com/handy-computer/transcribe.cpp), built with `-DTRANSCRIBE_VULKAN=ON -DTRANSCRIBE_BUILD_SHARED=ON` and installed

### Building

Building is done via CMake:

```sh
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr -S . -B ./build
cmake --build ./build --config Release --target hyprdictate -j`nproc 2>/dev/null || getconf _NPROCESSORS_CONF`
```

Install with:

```sh
cmake --install ./build
```
