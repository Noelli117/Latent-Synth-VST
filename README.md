# Latent Synth

Latent Synth is a JUCE-based VST3/Standalone audio plugin built on top of a TorchScript-exported [RAVE](https://github.com/acids-ircam/RAVE) model. It takes incoming audio, encodes it into the RAVE latent space, applies controllable latent transformations, and decodes the result back to audio in real time.

## Technical Report

For detailed technical background, system design, and implementation notes, see the included [Latent Synth Technical Report](Latent_Synth_Technical_Report.pdf).

## Introduction Video

For a visual overview and demonstration of the plugin, see the included [Latent Synth Introduction Video](Latent_Synth_Introduction_Video.mp4).

The models used in the video demonstration are:

- [Black Latents](https://forum.ircam.fr/projects/detail/black-latents/)
- [Schizophrenia](https://forum.ircam.fr/projects/detail/schizophrenia/)

## Important

Latent Synth is a host instrument/effect for TorchScript-exported RAVE models. The plugin itself does not ship with pretrained timbres or model files.

To produce sound, you need to import a compatible `.ts` RAVE model into the plugin. The plugin UI does not download official models directly; download a compatible pretrained model outside the plugin first, then import the `.ts` file through `Model Explorer > Import .ts`.

Compatible pretrained models are available from IRCAM's RAVE model collection:

https://forum.ircam.fr/collections/detail/rave-model-challenge/

You can also train and export your own models from the official RAVE repository:

https://github.com/acids-ircam/RAVE

## What It Is

This repository combines two main layers:

- `rave_vst_latent_synth/`: the C++ plugin and audio engine
- `UI_p5js/Latent_Synth_UI/`: the p5.js web UI that is bundled into the plugin

At a high level, this project turns RAVE into a playable, interactive latent-space instrument/effect:

- audio is buffered in the plugin and processed in blocks
- a TorchScript RAVE model is loaded with libtorch
- the input is either encoded into a latent trajectory or replaced by samples from the model prior
- latent values are modified in real time
- the modified latent trajectory is decoded back to audio

The plugin targets `VST3` and `Standalone`, and the current product name in CMake is `Latent Synth v0.1.5`.

## Prebuilt VST3 Install

This repo includes a prebuilt macOS Apple Silicon VST3 package:

```text
releases/macOS-arm64/Latent Synth v0.1.5 macOS-arm64 VST3.zip
```

Unzip it first. The extracted plugin bundle should be:

```text
Latent Synth v0.1.5.vst3
```

### macOS

For a per-user install, copy or drag the extracted `.vst3` bundle into:

```text
~/Library/Audio/Plug-Ins/VST3
```

For an all-users install, copy it into:

```text
/Library/Audio/Plug-Ins/VST3
```

If the `VST3` folder does not exist, create it. Then restart or rescan plugins in your DAW.

The included macOS build is `arm64` and is intended for Apple Silicon Macs. It is ad-hoc codesigned, not notarized.

### Windows

Windows cannot use the macOS `.vst3` bundle. Use a Windows-built `.vst3` package, then copy it into:

```text
C:\Program Files\Common Files\VST3
```

You may need administrator permission to write to that folder. After copying, restart or rescan plugins in your DAW.

## Architecture Origin: Based on RAVE

The core architecture is inherited from the original RAVE plugin approach:

- JUCE handles plugin lifecycle, UI hosting, audio I/O, and parameter management
- libtorch runs the exported TorchScript RAVE model inside the plugin
- the audio engine follows a standard RAVE loop:
  1. collect an input frame
  2. run `encode()` or `sample_prior()`
  3. modify the latent representation
  4. run `decode()`
  5. stream the result back to the host

In this repo, that base architecture lives mainly in:

- `rave_vst_latent_synth/source/Rave.h`
- `rave_vst_latent_synth/source/PluginProcessor.cpp`
- `rave_vst_latent_synth/source/PluginProcessorProcessing.cpp`

## What This Repo Adds on Top of RAVE

This is not just a direct RAVE wrapper. The main additions in this repo are:

### 1. Flow-field-driven latent control

The largest change is the `LatentFlowController`, which generates an 8D latent control signal from a particle flow simulation rather than relying only on static knob offsets.

It derives latent movement from:

- particle position statistics
- particle spread across the control canvas
- local 2D noise sampling
- local noise variance
- average flow direction
- motion energy and a dynamically breathing latent radius
- temporal jitter, per-dimension LFO motion, and smoothed stochastic motion

That controller feeds a global/external latent mode in the audio engine, where the first 8 latent dimensions are driven directly by the evolving flow state.

Relevant files:

- `rave_vst_latent_synth/source/LatentFlowController.h`
- `rave_vst_latent_synth/source/LatentFlowController.cpp`
- `rave_vst_latent_synth/source/PluginProcessorProcessing.cpp`

### 2. Hybrid latent editing modes

The plugin supports two distinct ways of working in latent space:

- `native` mode: scale, bias, jitter, width, and other classic plugin-side latent edits
- `global/external` mode: latent values are overridden by the flow controller / web UI state

This makes the plugin usable both as:

- a conventional RAVE effect with direct latent parameter shaping
- a performative latent instrument driven by a dynamic generative controller

### 3. Embedded p5.js web UI inside JUCE

Instead of building the controller entirely as a native JUCE UI, this repo bundles a p5.js interface into the plugin binary and hosts it through `juce::WebBrowserComponent`.

The plugin:

- packages the web assets as binary resources during CMake build
- writes them to a temp directory at runtime
- loads the page inside the plugin editor
- synchronizes host/plugin state through a custom URL bridge

Relevant files:

- `UI_p5js/Latent_Synth_UI/`
- `rave_vst_latent_synth/source/PluginEditor.cpp`
- `rave_vst_latent_synth/source/PluginEditorNetworking.cpp`

### 4. Native-mode mold visualizer

The p5.js UI also includes a mold/particle visualizer under native/timbre transform mode that responds to the current latent scale and bias controls. Recent tuning makes this background lighter-weight and more center-focused:

- fewer active mold agents for lower CPU/GPU load
- agents spawn near the center and respawn there after leaving the canvas
- sensor and fade ranges are tuned for slower, more persistent trails
- latent controls still shape speed, turning, density, radius, anisotropy, and stereo spread

### 5. Model management and persistent latent state

Compared with a minimal RAVE plugin, this repo also adds:

- local model discovery/import through the p5.js `Model Explorer`
- persistent external latent state
- persistent web-UI flow parameters
- safer model/runtime handling around buffer updates and latent history writes

The current `Model Explorer` panel is intentionally local-only. It exposes:

- `Local Models`
- `Load Selected`
- `Import .ts`
- `Refresh`

Official model browsing/downloading is not exposed in the plugin UI.

### 6. Packaging work for libtorch on macOS

The build system bundles libtorch into the plugin and sets runtime paths so the plugin can find the shipped libraries from inside the VST3 bundle. The development install script also repairs rpaths, re-signs the bundle, and installs it into the user VST3 folder.

## Dependencies

The plugin depends on:

- CMake 3.15+
- a C++17 compiler
- Xcode Command Line Tools on macOS
- LLVM/Clang
- libomp on macOS
- JUCE 6.1.6
- libtorch (downloaded automatically on first configure unless you provide it manually)

On macOS with Homebrew, a practical setup is:

```bash
brew install cmake llvm libomp
xcode-select --install
```

## Dependency Resolution

### JUCE

The build supports two JUCE flows:

- if `rave_vst_latent_synth/juce/` exists locally, CMake uses it
- otherwise CMake fetches JUCE `6.1.6` automatically from GitHub during configure

This makes the repo buildable from a clean clone without requiring JUCE to be committed into the repository.

### Compiler detection

On macOS, the CMake setup tries to find Homebrew LLVM in both of the common locations:

- `/opt/homebrew/opt/llvm/bin`
- `/usr/local/opt/llvm/bin`

If neither exists, CMake falls back to the toolchain already configured in the environment or selected by the system.

## Installing Dependencies

### Option A: Let CMake download JUCE and libtorch automatically

The default CMake flow can:

- fetch JUCE if no local checkout is present
- download a CPU libtorch build into the build tree if it is not already present

That means the first configure step needs internet access.

### Option B: Use a local JUCE checkout and/or an existing local libtorch

If you already have a local JUCE checkout, place it at:

```text
rave_vst_latent_synth/juce
```

If you already have libtorch, pass its root directory through `LATENT_SYNTH_TORCH_DIR`.

Example:

```bash
cd rave_vst_latent_synth
cmake -S . -B build-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DLATENT_SYNTH_TORCH_DIR=/path/to/libtorch
```

`LATENT_SYNTH_TORCH_DIR` should point to the libtorch root containing `lib/` and `share/cmake/Torch/`.

## How to Build

### Manual CMake build

```bash
cd rave_vst_latent_synth
cmake -S . -B build-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64

cmake --build build-arm64 --config Release --target rave-vst_VST3
```

Expected VST3 artifact:

```text
rave_vst_latent_synth/build-arm64/rave-vst_artefacts/Release/VST3/Latent Synth v0.1.5.vst3
```

### Build and install on macOS with the helper script

The repo includes a development installer that:

- configures CMake
- builds the VST3 target
- stages the bundle
- fixes/removes unwanted rpaths
- codesigns the result
- installs it to `~/Library/Audio/Plug-Ins/VST3`
- replaces older installed bundles matching `Latent Synth v*.vst3`
- removes the unversioned `Latent Synth.vst3` bundle if it exists

This script is intended for developers building from source on macOS. It is not a one-click end-user installer.

Run:

```bash
cd rave_vst_latent_synth
./scripts/install_vst3_dev.sh
```

For a clean rebuild:

```bash
cd rave_vst_latent_synth
./scripts/install_vst3_dev.sh --clean
```

By default, the installer removes prior versioned installs matching `Latent Synth v*.vst3` and the unversioned `Latent Synth.vst3` bundle. To also remove older legacy bundle names such as `latentSynthV2.vst3` or names listed in `LEGACY_PLUGIN_NAMES`, run:

```bash
cd rave_vst_latent_synth
./scripts/install_vst3_dev.sh --clean --remove-legacy-installs
```

## Runtime Notes

- Models are expected as TorchScript `.ts` files.
- The plugin creates a model directory under the user application data path and can import models there from the editor.
- The p5.js `Model Explorer` panel only manages local models: load an already imported model, import a `.ts` file, or refresh the local model list.
- `Use Prior` is now disabled by default, so new plugin instances encode the audio input instead of generating from `sample_prior()`.
- To use the model prior instead, enable `Use Prior` from the p5.js `Audio Params > Model Parameters` section.
- The p5.js `Audio Params` panel currently exposes only `Output Parameters`, `Model Parameters`, and `Window Size`.
- In web-only mode, the p5.js latent controller becomes the primary interface.
- The first 8 latent dimensions are the dimensions actively driven by the external flow controller.

## Repo Structure

```text
EP-491/
├── README.md
├── releases/            # prebuilt distributable plugin packages
├── UI_p5js/
│   └── Latent_Synth_UI/
└── rave_vst_latent_synth/
    ├── CMakeLists.txt
    ├── juce/                # optional local JUCE checkout
    ├── scripts/
    └── source/
```

## License

The source code in this repository is released under the MIT License. See [LICENSE](/Users/likejie/Documents/GitHub/EP-491/LICENSE).

Third-party dependencies such as JUCE, libtorch, p5.js, and any external model assets remain subject to their own licenses and terms.
