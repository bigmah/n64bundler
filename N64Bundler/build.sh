#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the ModernReality tools and runtime, and assemble N64Bundler.app.
#
#   ./build.sh              build what is missing, then install to ~/Applications
#   ./build.sh --rebuild    force a full rebuild first
#   ./build.sh --no-install leave N64Bundler.app here instead of installing it
#   ./build.sh --tools-only stop after the analyser and the recompiler
#
# The host and the window are not written yet; --tools-only is the whole build
# until they are. See PLAN.md for what is done and what is not.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MR_SRC="$ROOT/ModernReality"
MR_BUILD="$MR_SRC/build"
APP="$HERE/N64Bundler.app"
INSTALL_DIR="$HOME/Applications"

REBUILD=0
INSTALL=1
TOOLS_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --rebuild) REBUILD=1 ;;
    --no-install) INSTALL=0 ;;
    --tools-only) TOOLS_ONLY=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

bold=$'\033[1m'; off=$'\033[0m'
step() { printf '\n%s==> %s%s\n' "$bold" "$*" "$off"; }

for tool in cmake ninja git python3; do
  command -v "$tool" >/dev/null || { echo "$tool is required but not installed" >&2; exit 1; }
done

# Where cmake and ninja live, deduplicated and absolute. toolchain.conf carries
# it to the app, whose PATH when launched from Finder is launchd's four system
# directories - no Homebrew - which is exactly when the pipeline needs them.
TOOLCHAIN_PATH=''
for tool in cmake ninja; do
  d="$(cd "$(dirname "$(command -v "$tool")")" && pwd)"
  case ":$TOOLCHAIN_PATH:" in
    *":$d:"*) ;;
    *) TOOLCHAIN_PATH="${TOOLCHAIN_PATH:+$TOOLCHAIN_PATH:}$d" ;;
  esac
done

step "Checking out the submodules"
if [ ! -f "$MR_SRC/vendor/N64ModernRuntime/CMakeLists.txt" ] || \
   [ ! -f "$MR_SRC/vendor/rt64/CMakeLists.txt" ]; then
  echo "    fetching submodules (RT64 and its externals are a few hundred MB)"
  git -C "$ROOT" submodule update --init --recursive --depth 1
else
  echo "    already present"
fi
for f in "$MR_SRC/vendor/N64ModernRuntime/CMakeLists.txt" \
         "$MR_SRC/vendor/N64ModernRuntime/N64Recomp/CMakeLists.txt" \
         "$MR_SRC/vendor/rt64/CMakeLists.txt"; do
  [ -f "$f" ] || {
    echo "missing $f" >&2
    echo "The submodules did not come down; run: git -C $ROOT submodule update --init --recursive" >&2
    exit 1
  }
done

# Changes to the upstreams that they do not carry yet. They live as patches
# rather than as forks because there is one of them and it is small; when there
# are enough to be worth pushing back, they become forks and this becomes
# forks.sh, which is the road DolBundler already walked.
step "Applying the patches to N64Recomp"
RECOMP_SRC="$MR_SRC/vendor/N64ModernRuntime/N64Recomp"
for patch in "$HERE"/patches/*.patch; do
  [ -f "$patch" ] || continue
  name="$(basename "$patch")"
  if git -C "$RECOMP_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
    echo "    $name is already applied"
  elif git -C "$RECOMP_SRC" apply --check "$patch" >/dev/null 2>&1; then
    git -C "$RECOMP_SRC" apply "$patch"
    echo "    applied $name"
  else
    echo "$name applies neither way; the submodule pin has moved under it" >&2
    exit 1
  fi
done

step "Configuring ModernReality"
if [ "$REBUILD" -eq 1 ]; then rm -rf "$MR_BUILD"; fi
# Keyed on build.ninja, not CMakeCache.txt: the cache is written before
# generation, so an interrupted configure leaves one behind and reusing it
# would hand ninja a directory with nothing to build.
if [ ! -f "$MR_BUILD/build.ninja" ]; then
  # CMake 4 refuses the pre-3.5 minimums a few of RT64's externals declare.
  HOST_FLAG=()
  [ "$TOOLS_ONLY" -eq 0 ] && HOST_FLAG=(-DMODERNREALITY_HOST=ON)
  cmake -S "$MR_SRC" -B "$MR_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    "${HOST_FLAG[@]}"
else
  echo "    reusing $MR_BUILD"
fi

step "Building the analyser and the recompiler"
cmake --build "$MR_BUILD" -j "$(sysctl -n hw.ncpu)" \
  --target n64rip n64sig modernreality_ultra_gaps N64RecompCLI RSPRecomp

RECOMP_BIN_DIR="$MR_BUILD/vendor/N64ModernRuntime/librecomp/N64Recomp"
for tool in "$MR_BUILD/n64rip" "$MR_BUILD/n64sig" \
            "$RECOMP_BIN_DIR/N64Recomp" "$RECOMP_BIN_DIR/RSPRecomp"; do
  [ -x "$tool" ] || { echo "expected $tool to exist" >&2; exit 1; }
done

# Which libultra functions the runtime can stand in for.
#
# The recompiler reacts to a libultra name it recognises by not emitting that
# function's body, on the understanding that the runtime supplies one. Most of
# the time it does. The exceptions are functions the projects librecomp was
# built for happened to provide themselves, and naming one of those turns a
# working translation into a link error.
#
# So the list is read out of the built libraries rather than written down: it is
# exactly the set of `<name>_recomp` symbols that exist, and it cannot drift.
step "Reading the runtime's libultra coverage"
PROVIDES="$MR_BUILD/runtime-provides.txt"
nm -g "$MR_BUILD/vendor/N64ModernRuntime/librecomp/liblibrecomp.a" \
      "$MR_BUILD/vendor/N64ModernRuntime/ultramodern/libultramodern.a" \
      "$MR_BUILD/libmodernreality_ultra_gaps.a" 2>/dev/null \
  | grep -oE '_[A-Za-z_][A-Za-z0-9_]*_recomp$' \
  | sed 's/^_//; s/_recomp$//' | sort -u > "$PROVIDES"
echo "    $(wc -l < "$PROVIDES" | tr -d ' ') libultra functions the runtime implements"

# The signature database, if there is anything to build one from.
#
# Naming libultra inside a ROM needs libultra's own binaries to compare against,
# and those are Nintendo's, so nothing here ships one -- the same rule the ROMs
# themselves follow. Point N64_LIBULTRA at the `libultra*.a` from a
# decompilation project you have set up and the database is built from it.
# Without one, a game still recompiles; it just does so with libultra's hardware
# routines translated rather than replaced, and does not get far.
step "Building the libultra signature database"
SIGNATURES="$MR_BUILD/libultra.n64sig"
LIBULTRA_FILES=()
for candidate in ${N64_LIBULTRA:-}; do
  [ -f "$candidate" ] && LIBULTRA_FILES+=("$candidate")
done
if [ "${#LIBULTRA_FILES[@]}" -gt 0 ]; then
  "$MR_BUILD/n64sig" build "${LIBULTRA_FILES[@]}" --out "$SIGNATURES" | sed 's/^/    /'
else
  rm -f "$SIGNATURES"
  echo "    no libultra archive given, so nothing in a ROM can be named."
  echo "    Set N64_LIBULTRA to the libultra*.a from a decompilation project:"
  echo "      N64_LIBULTRA=\"/path/to/lib/n64/libultra*.a\" ./N64Bundler/build.sh"
fi

if [ "$TOOLS_ONLY" -eq 1 ]; then
  printf '\n%sTools built.%s Try:\n' "$bold" "$off"
  printf '  %s inspect <rom.z64>\n' "$MR_BUILD/n64rip"
  printf '  %s analyze <rom.z64> --out-dir /tmp/rip \\\n' "$MR_BUILD/n64rip"
  printf '      --runtime-provides %s' "$PROVIDES"
  [ -f "$SIGNATURES" ] && printf ' \\\n      --signatures %s' "$SIGNATURES"
  printf '\n  %s /tmp/rip/<ID>.recomp.toml\n' "$RECOMP_BIN_DIR/N64Recomp"
  exit 0
fi

step "Building the host"
cmake --build "$MR_BUILD" -j "$(sysctl -n hw.ncpu)" --target n64b-run

step "Assembling N64Bundler.app"
rm -rf "$APP"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"
mkdir -p "$MACOS" "$RES"

install -m 755 "$HERE/src/recompn64" "$RES/recompn64"
install -m 644 "$HERE/src/make_game_app.py" "$RES/make_game_app.py"

cat > "$RES/toolchain.conf" <<CONF
# Written by N64Bundler/build.sh. Absolute paths to the ModernReality build
# that analyses, recompiles, and runs ROMs. Re-run build.sh if the checkout
# moves: the generated apps reference these paths directly.
REPO_ROOT=$(printf '%q' "$ROOT")
MR_SRC=$(printf '%q' "$MR_SRC")
MR_BUILD=$(printf '%q' "$MR_BUILD")
RECOMP_BIN_DIR=$(printf '%q' "$RECOMP_BIN_DIR")
RUNTIME_PROVIDES=$(printf '%q' "$PROVIDES")
SIGNATURES=$(printf '%q' "$SIGNATURES")
APPS_DIR=$(printf '%q' "$INSTALL_DIR")
# Where cmake and ninja were found. A Finder-launched app gets launchd's PATH,
# which has no Homebrew on it, so the pipeline puts these back itself.
TOOLCHAIN_PATH=$(printf '%q' "$TOOLCHAIN_PATH")
CONF

if [ "$INSTALL" -eq 1 ]; then
  step "Installing to $INSTALL_DIR"
  mkdir -p "$INSTALL_DIR"
  rm -rf "$INSTALL_DIR/N64Bundler.app"
  cp -R "$APP" "$INSTALL_DIR/N64Bundler.app"
  echo "    $INSTALL_DIR/N64Bundler.app"
fi

printf '\n%sDone.%s\n' "$bold" "$off"
