#!/bin/zsh

if [ -z "${ZSH_VERSION:-}" ]; then
  exec /bin/zsh "$0" "$@"
fi

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
CMAKE_FILE="$PROJECT_DIR/CMakeLists.txt"

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-arm64}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
ARCH="${ARCH:-$(uname -m)}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
VST3_DIR="${VST3_DIR:-$HOME/Library/Audio/Plug-Ins/VST3}"
LEGACY_PLUGIN_NAMES="${LEGACY_PLUGIN_NAMES:-Latent Synth}"
REMOVE_LEGACY_INSTALLS="${REMOVE_LEGACY_INSTALLS:-0}"
EXPECTED_RPATH="@loader_path/../torch/libtorch"
TMP_ROOT=""

die() {
  echo "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

cleanup() {
  if [[ -n "$TMP_ROOT" && -d "$TMP_ROOT" ]]; then
    rm -rf -- "$TMP_ROOT"
  fi
}
trap cleanup EXIT

while (( $# > 0 )); do
  case "$1" in
    --clean|--clean-build)
      CLEAN_BUILD=1
      ;;
    --remove-legacy-installs)
      REMOVE_LEGACY_INSTALLS=1
      ;;
    -h|--help)
      echo "Usage: ${0:t} [--clean|--clean-build] [--remove-legacy-installs]"
      echo
      echo "Default behavior:"
      echo "  - rebuilds the plugin"
      echo "  - replaces the current versioned install"
      echo "  - removes older installs matching '${PLUGIN_DISPLAY_NAME} v*.vst3'"
      echo
      echo "Optional behavior:"
      echo "  --remove-legacy-installs  Also removes older non-versioned/legacy bundle names."
      exit 0
      ;;
    *)
      die "Unknown option: $1"
      ;;
  esac
  shift
done

[[ -f "$CMAKE_FILE" ]] || die "Missing CMake file: $CMAKE_FILE"
[[ "$(uname -s)" == "Darwin" ]] || die "This installer is macOS-only."
require_command cmake
require_command codesign
require_command otool
require_command install_name_tool
require_command ditto
require_command mktemp
require_command uname

PROJECT_NAME=$(sed -nE 's/^[[:space:]]*project[[:space:]]*\([[:space:]]*([^[:space:])]+).*$/\1/p' "$CMAKE_FILE" | head -n 1)
PROJECT_VERSION=$(sed -nE 's/^[[:space:]]*project[[:space:]]*\([^)]*VERSION[[:space:]]+([0-9]+(\.[0-9]+)*).*$/\1/p' "$CMAKE_FILE" | head -n 1)
PLUGIN_DISPLAY_NAME=$(sed -nE 's/^[[:space:]]*set[[:space:]]*\([[:space:]]*plugin_display_name[[:space:]]+"([^"]+)"[[:space:]]*\).*$/\1/p' "$CMAKE_FILE" | head -n 1)

[[ -n "$PROJECT_NAME" ]] || die "Could not read project name from $CMAKE_FILE"
[[ -n "$PROJECT_VERSION" ]] || die "Could not read project version from $CMAKE_FILE"
[[ -n "$PLUGIN_DISPLAY_NAME" ]] || die "Could not read plugin_display_name from $CMAKE_FILE"

BUILD_DIR="${BUILD_DIR%/}"
[[ -n "$BUILD_DIR" && "$BUILD_DIR" != "/" ]] || die "Refusing to use unsafe BUILD_DIR: ${BUILD_DIR:-/}"
BUILD_DIR_PARENT="${BUILD_DIR:h}"
BUILD_DIR_BASENAME="${BUILD_DIR:t}"
mkdir -p "$BUILD_DIR_PARENT"
BUILD_DIR="$(cd "$BUILD_DIR_PARENT" && pwd)/$BUILD_DIR_BASENAME"

TARGET_NAME="${TARGET_NAME:-${PROJECT_NAME}_VST3}"
PLUGIN_PRODUCT_NAME="${PLUGIN_DISPLAY_NAME} v${PROJECT_VERSION}"
ARTIFACT="$BUILD_DIR/${PROJECT_NAME}_artefacts/$BUILD_TYPE/VST3/${PLUGIN_PRODUCT_NAME}.vst3"
ARTIFACT_BINARY="$ARTIFACT/Contents/MacOS/${PLUGIN_PRODUCT_NAME}"
INSTALLED_BUNDLE="$VST3_DIR/${PLUGIN_PRODUCT_NAME}.vst3"
INSTALLED_BINARY="$INSTALLED_BUNDLE/Contents/MacOS/${PLUGIN_PRODUCT_NAME}"

cleanup_names=("$PLUGIN_DISPLAY_NAME")
if [[ -n "$LEGACY_PLUGIN_NAMES" ]]; then
  cleanup_names+=("${(@s:,:)LEGACY_PLUGIN_NAMES}")
fi

echo "Project dir:      $PROJECT_DIR"
echo "Build dir:        $BUILD_DIR"
echo "Build target:     $TARGET_NAME"
echo "Clean build:      $CLEAN_BUILD"
echo "Arch:             $ARCH"
echo "Plugin name:      $PLUGIN_PRODUCT_NAME"
echo "Cleanup names:    ${(j:, :)cleanup_names}"
echo "Install location: $INSTALLED_BUNDLE"

case "${CLEAN_BUILD:l}" in
  1|true|yes|on)
    if [[ "$BUILD_DIR" == "/" || "$BUILD_DIR" == "$PROJECT_DIR" || "$BUILD_DIR" == "$HOME" ]]; then
      die "Refusing to remove unsafe BUILD_DIR: $BUILD_DIR"
    fi
    echo "Removing build dir: $BUILD_DIR"
    rm -rf -- "$BUILD_DIR"
    ;;
esac

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_OSX_ARCHITECTURES="$ARCH"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --target "$TARGET_NAME"

[[ -d "$ARTIFACT" ]] || die "Built VST3 bundle not found: $ARTIFACT"
[[ -f "$ARTIFACT_BINARY" ]] || die "Built VST3 binary not found: $ARTIFACT_BINARY"

mkdir -p "$VST3_DIR"
TMP_ROOT=$(mktemp -d "$VST3_DIR/.${PROJECT_NAME}.install.XXXXXX")
STAGED_BUNDLE="$TMP_ROOT/${PLUGIN_PRODUCT_NAME}.vst3"
STAGED_BINARY="$STAGED_BUNDLE/Contents/MacOS/${PLUGIN_PRODUCT_NAME}"

ditto "$ARTIFACT" "$STAGED_BUNDLE"
[[ -f "$STAGED_BINARY" ]] || die "Staged VST3 binary not found: $STAGED_BINARY"

found_expected_rpath=0
while IFS= read -r rpath; do
  if [[ "$rpath" == "$EXPECTED_RPATH" ]]; then
    found_expected_rpath=1
  elif [[ "$rpath" == /Users/* || "$rpath" == "$PROJECT_DIR"* || "$rpath" == "$BUILD_DIR"* ]]; then
    install_name_tool -delete_rpath "$rpath" "$STAGED_BINARY"
  fi
done < <(otool -l "$STAGED_BINARY" | awk '
  $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
  in_rpath && $1 == "path" { print $2; in_rpath = 0 }
')

if (( ! found_expected_rpath )); then
  install_name_tool -add_rpath "$EXPECTED_RPATH" "$STAGED_BINARY"
fi

codesign --force --deep --sign - "$STAGED_BUNDLE"

old_installs=()
typeset -U old_installs
old_installs+=(
  "$VST3_DIR/${PLUGIN_DISPLAY_NAME} v"*.vst3(N)
  "$VST3_DIR/${PLUGIN_DISPLAY_NAME}.vst3"(N)
)

case "${REMOVE_LEGACY_INSTALLS:l}" in
  1|true|yes|on)
    for name in "${cleanup_names[@]}"; do
      old_installs+=(
        "$VST3_DIR/${name} v"*.vst3(N)
        "$VST3_DIR/${name}.vst3"(N)
      )
    done
    old_installs+=("$VST3_DIR/latentSynthV2.vst3"(N))
    ;;
esac

if (( ${#old_installs[@]} )); then
  echo "Removing existing installs:"
  printf '  %s\n' "${old_installs[@]}"
  rm -rf -- "${old_installs[@]}"
fi

mv "$STAGED_BUNDLE" "$INSTALLED_BUNDLE"

echo "Installed rpath:"
otool -l "$INSTALLED_BINARY" | grep -A2 LC_RPATH || true
codesign --verify --deep --strict --verbose=2 "$INSTALLED_BUNDLE"

installed_matches=()
typeset -U installed_matches
installed_matches+=(
  "$VST3_DIR/${PLUGIN_DISPLAY_NAME} v"*.vst3(N)
  "$VST3_DIR/${PLUGIN_DISPLAY_NAME}.vst3"(N)
)

case "${REMOVE_LEGACY_INSTALLS:l}" in
  1|true|yes|on)
    for name in "${cleanup_names[@]}"; do
      installed_matches+=(
        "$VST3_DIR/${name} v"*.vst3(N)
        "$VST3_DIR/${name}.vst3"(N)
      )
    done
    installed_matches+=("$VST3_DIR/latentSynthV2.vst3"(N))
    ;;
esac

echo "Installed tracked bundles:"
if (( ${#installed_matches[@]} )); then
  printf '  %s\n' "${installed_matches[@]}"
else
  echo "  (none)"
fi

echo "Install complete: $INSTALLED_BUNDLE"
