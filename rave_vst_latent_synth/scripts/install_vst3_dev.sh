#!/bin/zsh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
CMAKE_FILE="$PROJECT_DIR/CMakeLists.txt"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-arm64}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
ARCH="${ARCH:-arm64}"
TARGET_NAME="rave-vst_VST3"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"

if [[ ! -f "$CMAKE_FILE" ]]; then
  echo "Missing CMake file: $CMAKE_FILE" >&2
  exit 1
fi

PROJECT_VERSION=$(sed -nE 's/^project\(.* VERSION ([0-9.]+)\)$/\1/p' "$CMAKE_FILE")
PLUGIN_DISPLAY_NAME=$(sed -nE 's/^[[:space:]]*set[[:space:]]*\(plugin_display_name "([^"]+)"\)$/\1/p' "$CMAKE_FILE")

if [[ -z "${PROJECT_VERSION:-}" ]]; then
  echo "Could not read project version from $CMAKE_FILE" >&2
  exit 1
fi

if [[ -z "${PLUGIN_DISPLAY_NAME:-}" ]]; then
  echo "Could not read plugin_display_name from $CMAKE_FILE" >&2
  exit 1
fi

PLUGIN_PRODUCT_NAME="${PLUGIN_DISPLAY_NAME} v${PROJECT_VERSION}"
ARTIFACT="$BUILD_DIR/rave-vst_artefacts/$BUILD_TYPE/VST3/${PLUGIN_PRODUCT_NAME}.vst3"
INSTALLED_BUNDLE="$VST3_DIR/${PLUGIN_PRODUCT_NAME}.vst3"
BINARY="$ARTIFACT/Contents/MacOS/${PLUGIN_PRODUCT_NAME}"
INSTALLED_BINARY="$INSTALLED_BUNDLE/Contents/MacOS/${PLUGIN_PRODUCT_NAME}"

echo "Project dir:        $PROJECT_DIR"
echo "Build dir:          $BUILD_DIR"
echo "Plugin name:        $PLUGIN_PRODUCT_NAME"
echo "Install location:   $INSTALLED_BUNDLE"

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_OSX_ARCHITECTURES="$ARCH"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --target "$TARGET_NAME"

if [[ ! -d "$ARTIFACT" ]]; then
  echo "Built VST3 bundle not found: $ARTIFACT" >&2
  exit 1
fi

mkdir -p "$VST3_DIR"

# Remove known legacy bundle names as well as the current versioned install.
rm -rf \
  "$VST3_DIR/latentSynthV2.vst3" \
  "$VST3_DIR/Latent Synth.vst3" \
  "$INSTALLED_BUNDLE"

if [[ -f "$BINARY" ]]; then
  # Keep only the portable rpath inside the bundle.
  while IFS= read -r rpath; do
    if [[ "$rpath" == /Users/* || "$rpath" == "$PROJECT_DIR"* || "$rpath" == "$BUILD_DIR"* ]]; then
      install_name_tool -delete_rpath "$rpath" "$BINARY"
    fi
  done < <(otool -l "$BINARY" | awk '
    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
    in_rpath && $1 == "path" { print $2; in_rpath = 0 }
  ')
fi

codesign --force --deep -s - "$ARTIFACT"
cp -R "$ARTIFACT" "$INSTALLED_BUNDLE"

echo "Installed rpath:"
otool -l "$INSTALLED_BINARY" | grep -A2 LC_RPATH || true
codesign --verify --deep --strict --verbose=2 "$INSTALLED_BUNDLE"

echo "Install complete: $INSTALLED_BUNDLE"
