# hyprdictate

Streaming dictation into the focused text field on Wayland.

It runs as the seat's input method (`zwp_input_method_v2`) and streams the
microphone into a streaming speech model ([transcribe.cpp](https://github.com/handy-computer/transcribe.cpp),
on the GPU through Vulkan). Words the model won't revise are committed into the
field as they arrive; the words it may still revise sit in the field's preedit
until they settle. Nothing fakes key presses.

```sh
hyprdictate daemon --model nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf --language en-US --lookahead 6
hyprdictate toggle   # start / stop, e.g. from a Hyprland bind
```

The daemon keeps the model loaded and opens the microphone only while
dictating. `start`, `stop` and `toggle` talk to it over a
[hyprwire](https://github.com/hyprwm/hyprwire) socket at
`$XDG_RUNTIME_DIR/hyprdictate.sock`.

Models: any transcribe.cpp GGUF that supports streaming, e.g.
[Nemotron 3.5 ASR Streaming](https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf).

Limits: only one input method can run per seat, and the app must support
`text_input_v3` (GTK, foot; Chromium with `--enable-wayland-ime`).

## Building

Dependencies: wayland, pipewire, [hyprwire](https://github.com/hyprwm/hyprwire),
[hyprutils](https://github.com/hyprwm/hyprutils),
[hyprwayland-scanner](https://github.com/hyprwm/hyprwayland-scanner), and
[transcribe.cpp](https://github.com/handy-computer/transcribe.cpp) built with
`-DTRANSCRIBE_VULKAN=ON -DTRANSCRIBE_BUILD_SHARED=ON` and installed.

Building is done via CMake:

```sh
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr -S . -B ./build
cmake --build ./build --config Release --target hyprdictate -j`nproc 2>/dev/null || getconf _NPROCESSORS_CONF`
```

Install with:

```sh
cmake --install ./build
```

## Nix

`nix build` builds it with transcribe.cpp. `default.nix` (also the flake's outputs) exports `overlays.default` and
`maidModules.default`, a [nix-maid](https://github.com/viperML/nix-maid)
module that runs the daemon in the graphical session:

```nix
maid.sharedModules = [ (import hyprdictate).maidModules.default ];
# per user:
hyprdictate = {
  enable = true;
  model = ./nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf; # required
  # also: package, language, lookahead
};
```
