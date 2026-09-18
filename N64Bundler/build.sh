#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the ModernReality tools and runtime, and assemble N64Bundler.app.
#
#   ./build.sh                build what is missing, then install to ~/Applications
#   ./build.sh --rebuild      force a full rebuild first
#   ./build.sh --no-install   leave N64Bundler.app here instead of installing it
#   ./build.sh --tools-only   stop after the analyser and the recompiler
#   ./build.sh --no-window    stop after the host, leaving out the window and the app
#   ./build.sh --no-renderer  build a host that runs games and cannot draw them
#
# --tools-only skips RT64, which is most of the build, and is what to use while
# only the analyser is being worked on. --no-window is everything a program that
# recompiles and drives games itself needs (n64rip, n64b-port, n64b-run --gym),
# and nothing that is only for playing them from the library. See PLAN.md for
# what is done and what is not.
#
# --no-renderer goes further: it leaves RT64 out of the host as well, which is
# the whole of this build's dependency on a GPU, a graphics API and a shader
# compiler. What is left still runs a game and still shares its memory -- it
# just cannot show it -- which is what an environment driving one headless
# uses, and is the difference between a build that works on a machine with no
# GPU and a build that cannot be configured there.
#
# The window and the .app are macOS only, and everything else is not: on Linux
# this stops where --no-window stops, because there is no Dioxus app to build
# and nothing to install into ~/Applications.
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
NO_WINDOW=0
RENDERER=1
for arg in "$@"; do
  case "$arg" in
    --rebuild) REBUILD=1 ;;
    --no-install) INSTALL=0 ;;
    --tools-only) TOOLS_ONLY=1 ;;
    --no-window) NO_WINDOW=1 ;;
    --no-renderer) RENDERER=0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

# What is different about this machine.
#
# Three things, and they are the whole of it: how many processors there are,
# what a shared library is called, and whether there is a window server that
# can be asked for an .app. Everything else below is the same everywhere.
case "$(uname -s)" in
  Darwin)
    PLATFORM=macos
    NPROC="$(sysctl -n hw.ncpu)"
    LIB_EXT=dylib
    # Mach-O puts an underscore in front of every C symbol and ELF does not,
    # which matters exactly once: reading the runtime's libultra coverage.
    LEADING_UNDERSCORE=1
    ;;
  Linux)
    PLATFORM=linux
    NPROC="$(nproc)"
    LIB_EXT=so
    LEADING_UNDERSCORE=0
    # There is no app to assemble and nowhere to install one, so the build
    # stops where --no-window stops whether or not it was asked for.
    NO_WINDOW=1
    ;;
  *)
    echo "build.sh knows macOS and Linux, not $(uname -s)." >&2
    echo "Everything below the window is portable; if you are on a BSD, adding a" >&2
    echo "case here and a processor count is most likely all it wants." >&2
    exit 1
    ;;
esac

# A host that cannot draw has nothing to put in a window, and the app exists to
# play games in one -- so --no-renderer stops where --no-window stops. Building
# the app around such a host would produce something that installs, launches,
# and fails at the first game.
if [ "$RENDERER" -eq 0 ]; then
  NO_WINDOW=1
fi

# --no-renderer and a build that stops before the host do not interact: RT64 is
# already out of a --tools-only build. Saying so is worth it because the two
# flags read as though they might fight.
if [ "$TOOLS_ONLY" -eq 1 ] && [ "$RENDERER" -eq 0 ]; then
  echo "note: --tools-only does not build the host at all, so --no-renderer adds nothing."
fi

bold=$'\033[1m'; off=$'\033[0m'
step() { printf '\n%s==> %s%s\n' "$bold" "$*" "$off"; }

for tool in cmake ninja git python3; do
  command -v "$tool" >/dev/null || { echo "$tool is required but not installed" >&2; exit 1; }
done
if [ "$TOOLS_ONLY" -eq 0 ] && [ "$NO_WINDOW" -eq 0 ]; then
  command -v cargo >/dev/null || { echo "cargo is required to build the window; install Rust or pass --no-window" >&2; exit 1; }
fi

# Where cmake, ninja and the compiler live, deduplicated and absolute.
# toolchain.conf carries it to the app, whose PATH when launched from Finder is
# launchd's four system directories - no Homebrew - which is exactly when the
# pipeline needs them.
TOOLCHAIN_PATH=''
for tool in cmake ninja clang; do
  command -v "$tool" >/dev/null || continue
  d="$(cd "$(dirname "$(command -v "$tool")")" && pwd)"
  case ":$TOOLCHAIN_PATH:" in
    *":$d:"*) ;;
    *) TOOLCHAIN_PATH="${TOOLCHAIN_PATH:+$TOOLCHAIN_PATH:}$d" ;;
  esac
done

# RT64 is 400MB of the renderer and its externals against the runtime's 33MB,
# and only a host that draws is built from it: --tools-only builds no host and
# --no-renderer builds one without it. So it is fetched, checked and patched
# only when it is used, which is most of the download on a machine with no GPU.
NEED_RT64=1
if [ "$TOOLS_ONLY" -eq 1 ] || [ "$RENDERER" -eq 0 ]; then
  NEED_RT64=0
fi

step "Checking out the submodules"
fetched=0
if [ ! -f "$MR_SRC/vendor/N64ModernRuntime/CMakeLists.txt" ] || \
   [ ! -f "$MR_SRC/vendor/N64ModernRuntime/N64Recomp/CMakeLists.txt" ]; then
  echo "    fetching the runtime and the recompiler"
  git -C "$ROOT" submodule update --init --recursive --depth 1 -- ModernReality/vendor/N64ModernRuntime
  fetched=1
fi
if [ "$NEED_RT64" -eq 1 ] && [ ! -f "$MR_SRC/vendor/rt64/CMakeLists.txt" ]; then
  echo "    fetching RT64 (it and its externals are a few hundred MB)"
  git -C "$ROOT" submodule update --init --recursive --depth 1 -- ModernReality/vendor/rt64
  fetched=1
fi
[ "$fetched" -eq 1 ] || echo "    already present"
REQUIRED=("$MR_SRC/vendor/N64ModernRuntime/CMakeLists.txt"
          "$MR_SRC/vendor/N64ModernRuntime/N64Recomp/CMakeLists.txt")
[ "$NEED_RT64" -eq 0 ] || REQUIRED+=("$MR_SRC/vendor/rt64/CMakeLists.txt")
for f in "${REQUIRED[@]}"; do
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
step "Applying the patches to the vendored runtimes"
RECOMP_SRC="$MR_SRC/vendor/N64ModernRuntime/N64Recomp"
RUNTIME_SRC="$MR_SRC/vendor/N64ModernRuntime"
RT64_SRC="$MR_SRC/vendor/rt64"

# A patch names the checkout it belongs to, because there are three of them and
# N64Recomp is a submodule of one: 0001-n64recomp-... applies inside N64Recomp,
# 0002-librecomp-... in the runtime around it, and 0023-rt64-... in the renderer.
patches_for() {
  for patch in "$HERE"/patches/*.patch; do
    [ -f "$patch" ] || continue
    case "$(basename "$patch")" in
      *-n64recomp-*)                 [ "$1" = recomp ]  && printf '%s\n' "$patch" ;;
      *-librecomp-*|*-ultramodern-*) [ "$1" = runtime ] && printf '%s\n' "$patch" ;;
      *-rt64-*)                      [ "$1" = rt64 ]    && printf '%s\n' "$patch" ;;
      *) echo "$(basename "$patch") does not say which checkout it applies to" >&2; exit 1 ;;
    esac
  done
}

# Whether a checkout already carries its patches.
#
# Asking each patch whether it reverse-applies is the obvious way and it is
# wrong, because two patches are allowed to touch the same few lines: once one
# of them has landed the other's context has moved and it will not reverse even
# though it is applied. What is being asked is a question about the series and
# not about any patch in it, so it is answered that way -- build what the
# series says the files should be, out of the pinned commit and a scratch
# directory, and compare. That is exact, it costs a handful of small files, and
# it means the patches are the source of truth for what these checkouts hold.
series_matches() {
  local target="$1" which="$2" scratch files rc=0
  files=$(patches_for "$which" | xargs awk '/^--- a\//{print substr($2, 3)}' | sort -u)
  [ -n "$files" ] || return 0
  scratch="$(mktemp -d)"
  for f in $files; do
    mkdir -p "$scratch/$(dirname "$f")"
    git -C "$target" show "HEAD:$f" > "$scratch/$f" 2>/dev/null || { rm -rf "$scratch"; return 1; }
  done
  for patch in $(patches_for "$which"); do
    git -C "$scratch" apply -p1 "$patch" 2>/dev/null || { rm -rf "$scratch"; return 1; }
  done
  for f in $files; do
    cmp -s "$scratch/$f" "$target/$f" || rc=1
  done
  rm -rf "$scratch"
  return $rc
}

# Only the checkouts this build uses. RT64 left unfetched is an empty directory,
# and `git -C` there is the superproject: asking it for RT64's files, or applying
# RT64's patches, would be asking N64Bundler's own tree.
PATCHED=(recomp runtime)
[ "$NEED_RT64" -eq 0 ] || PATCHED+=(rt64)
for which in "${PATCHED[@]}"; do
  case "$which" in
    recomp)  target="$RECOMP_SRC" ;;
    runtime) target="$RUNTIME_SRC" ;;
    rt64)    target="$RT64_SRC" ;;
  esac
  if series_matches "$target" "$which"; then
    echo "    $(basename "$target") already carries its patches"
    continue
  fi
  for patch in $(patches_for "$which"); do
    name="$(basename "$patch")"
    if git -C "$target" apply "$patch" 2>/dev/null; then
      echo "    applied $name"
    else
      echo "$name does not apply to $(basename "$target"), and the series as a whole" >&2
      echo "does not match what is there either. Either the submodule pin has moved" >&2
      echo "under the patches, or the checkout has been edited by hand -- and if it" >&2
      echo "has, the edit belongs in a patch. git -C $target diff will show it." >&2
      exit 1
    fi
  done
done

step "Configuring ModernReality"
if [ "$REBUILD" -eq 1 ]; then rm -rf "$MR_BUILD"; fi
WANT_HOST=ON
[ "$TOOLS_ONLY" -eq 1 ] && WANT_HOST=OFF
WANT_RENDERER=ON
[ "$RENDERER" -eq 0 ] && WANT_RENDERER=OFF
# Keyed on build.ninja, not CMakeCache.txt: the cache is written before
# generation, so an interrupted configure leaves one behind and reusing it
# would hand ninja a directory with nothing to build. Reconfigured as well when
# the host was left out of an earlier run and is wanted now, which is the usual
# way a --tools-only tree becomes a full one.
HAVE_HOST="$(sed -n 's/^MODERNREALITY_HOST:BOOL=//p' "$MR_BUILD/CMakeCache.txt" 2>/dev/null || true)"
HAVE_RENDERER="$(sed -n 's/^MODERNREALITY_RENDERER:BOOL=//p' "$MR_BUILD/CMakeCache.txt" 2>/dev/null || true)"
if [ ! -f "$MR_BUILD/build.ninja" ] || [ "$HAVE_HOST" != "$WANT_HOST" ] || \
   [ "$HAVE_RENDERER" != "$WANT_RENDERER" ]; then
  # CMake 4 refuses the pre-3.5 minimums a few of RT64's externals declare.
  cmake -S "$MR_SRC" -B "$MR_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DMODERNREALITY_HOST="$WANT_HOST" \
    -DMODERNREALITY_RENDERER="$WANT_RENDERER"
else
  echo "    reusing $MR_BUILD"
fi

step "Building the analyser and the recompiler"
cmake --build "$MR_BUILD" -j "$NPROC" \
  --target n64rip n64sig n64b-port modernreality_ultra_gaps N64RecompCLI RSPRecomp

RECOMP_BIN_DIR="$MR_BUILD/vendor/N64ModernRuntime/librecomp/N64Recomp"
for tool in "$MR_BUILD/n64rip" "$MR_BUILD/n64sig" "$MR_BUILD/n64b-port" \
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
# The symbol name is the last field of an nm line on both platforms, and the
# only difference between them is Mach-O's leading underscore. Taking the field
# and then stripping that -- rather than matching a pattern that starts with an
# underscore -- is what makes this read the same list on both: against an ELF
# archive the old pattern matched `_Thread_recomp` inside `osCreateThread_recomp`
# and reported a libultra function called `Thread`.
nm -g "$MR_BUILD/vendor/N64ModernRuntime/librecomp/liblibrecomp.a" \
      "$MR_BUILD/vendor/N64ModernRuntime/ultramodern/libultramodern.a" \
      "$MR_BUILD/libmodernreality_ultra_gaps.a" 2>/dev/null \
  | awk -v strip="$LEADING_UNDERSCORE" \
        '{ name = $NF; if (strip == 1) sub(/^_/, "", name); print name }' \
  | grep -E '^[A-Za-z_][A-Za-z0-9_]*_recomp$' \
  | sed 's/_recomp$//' | sort -u > "$PROVIDES"
echo "    $(wc -l < "$PROVIDES" | tr -d ' ') libultra functions the runtime implements"

# The signature database, if there is anything to build one from.
#
# Naming libultra inside a ROM needs libultra's own binaries to compare against,
# and those are Nintendo's, so nothing here ships one -- the same rule the ROMs
# themselves follow. Point N64_LIBULTRA at the `libultra*.a` from a
# decompilation project you have set up and the database is built from it.
# Without one, a game still recompiles; it just does so with more of libultra
# stubbed, and does not get far.
step "Building the libultra signature database"
SIGNATURES="$MR_BUILD/libultra.n64sig"
# Sorted, because two archives can hold the same function under the same name
# and the first one in wins. Left to the shell's glob order that is stable, but
# N64_LIBULTRA can also be a hand-written list, and a database that depends on
# what order somebody typed the archives in is a database that changes what a
# ROM recompiles to for no reason anybody can see.
LIBULTRA_FILES=()
while IFS= read -r candidate; do
  [ -n "$candidate" ] && [ -f "$candidate" ] && LIBULTRA_FILES+=("$candidate")
done < <(for candidate in ${N64_LIBULTRA:-}; do printf '%s\n' "$candidate"; done | LC_ALL=C sort)
if [ "${#LIBULTRA_FILES[@]}" -gt 0 ]; then
  "$MR_BUILD/n64sig" build "${LIBULTRA_FILES[@]}" --out "$SIGNATURES" | sed 's/^/    /'
elif [ -f "$SIGNATURES" ]; then
  # Keeping one an earlier run built. Deleting it because the environment
  # variable is not set this time would quietly halve what the next ROM
  # recompiles to, and "build what is missing" is what this script does
  # everywhere else.
  echo "    keeping the database an earlier run built ($SIGNATURES)"
  echo "    Set N64_LIBULTRA to rebuild it, or delete it to go without."
else
  echo "    no libultra archive given, so nothing in a ROM can be named."
  echo "    A ROM still recompiles: every libultra function that drives hardware is"
  echo "    stubbed instead of being replaced by the runtime's own, which is enough to"
  echo "    build and not enough to play -- unless the game's title record names its"
  echo "    libultra itself, as Super Mario 64's does."
  echo "    Set N64_LIBULTRA to the libultra*.a from a decompilation project:"
  echo "      N64_LIBULTRA=\"/path/to/lib/n64/libultra*.a\" ./N64Bundler/build.sh"
fi

if [ "$TOOLS_ONLY" -eq 1 ]; then
  printf '\n%sTools built.%s Try:\n' "$bold" "$off"
  printf '  %s inspect <rom.z64>\n' "$MR_BUILD/n64rip"
  printf '  %s analyze <rom.z64> --out-dir /tmp/rip \\\n' "$MR_BUILD/n64rip"
  printf '      --runtime-provides %s' "$PROVIDES"
  [ -f "$SIGNATURES" ] && printf ' \\\n      --signatures %s' "$SIGNATURES"
  printf ' \\\n      --titles %s' "$HERE/titles"
  printf '\n  %s build --analysis /tmp/rip --rom <rom.z64> --out /tmp/game.%s\n' \
    "$MR_BUILD/n64b-port" "$LIB_EXT"
  exit 0
fi

step "Building the host"
if [ "$RENDERER" -eq 0 ]; then
  echo "    without RT64: this host will run games and not draw them."
else
  echo "    RT64 is a few hundred source files; the first build takes a while."
  if [ "$PLATFORM" = macos ]; then
    # RT64 compiles its shaders with Apple's metal, which Xcode 26 downloads as
    # a component of its own, and asks for it as `xcrun -sdk macosx metal`. On
    # some installs that finds only Xcode's stub, which says the component is
    # missing, while the component itself answers when it is asked for by name
    # -- seen with Xcode 26.2 (17C52) and the component at 17C7003j. So it is
    # asked for by name when that is the only way it answers.
    if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
      if TOOLCHAINS=Metal xcrun -sdk macosx metal --version >/dev/null 2>&1; then
        echo "    xcrun finds metal only when asked for the Metal toolchain by name, so it is"
        export TOOLCHAINS=Metal
      else
        echo "RT64 compiles its shaders with Apple's Metal toolchain, and Xcode does not have it:" >&2
        echo "    xcodebuild -downloadComponent MetalToolchain" >&2
        exit 1
      fi
    fi
  else
    # Elsewhere RT64 draws through Vulkan and compiles its shaders to SPIR-V
    # with the DXC it vendors -- there is nothing to install for the shaders.
    # What is needed is the Vulkan headers and loader, and SDL2 to make the
    # window the renderer draws on.
    #
    # Said here rather than left to cmake because the failure lands a few
    # hundred lines into RT64's own configure, where it reads as RT64 being
    # broken rather than as a package that is not installed.
    DXC="$RT64_SRC/src/contrib/dxc/bin/$(uname -m)/dxc-linux"
    if [ ! -x "$DXC" ]; then
      echo "RT64 vendors a shader compiler per architecture and has none for $(uname -m):" >&2
      echo "    looked for $DXC" >&2
      echo "Pass --no-renderer for a host that runs games without drawing them." >&2
      exit 1
    fi
    if ! { pkg-config --exists vulkan 2>/dev/null || [ -f /usr/include/vulkan/vulkan.h ]; }; then
      echo "RT64 draws through Vulkan here and its headers are not installed." >&2
      echo "    Debian/Ubuntu: apt install libvulkan-dev libsdl2-dev" >&2
      echo "    Fedora:        dnf install vulkan-loader-devel vulkan-headers SDL2-devel" >&2
      echo "Pass --no-renderer for a host that runs games without drawing them." >&2
      exit 1
    fi
  fi
fi
cmake --build "$MR_BUILD" -j "$NPROC" --target n64b-run console_memory_test
[ -x "$MR_BUILD/host/n64b-run" ] || { echo "expected $MR_BUILD/host/n64b-run to exist" >&2; exit 1; }

# That a second view of the console's memory is the same memory, on this
# machine. A host that runs without it runs games that are silently wrong, so it
# is checked every build rather than left for someone to remember: it takes a
# few milliseconds, no ROM and no GPU.
step "Checking the console's memory"
if ! "$MR_BUILD/console_memory_test" >"$MR_BUILD/console_memory_test.log" 2>&1; then
  cat "$MR_BUILD/console_memory_test.log" >&2
  echo "A second view of the console's memory is not the same memory on this machine," >&2
  echo "so a game would run and be wrong. See ModernReality/include/modernreality/console_memory.h." >&2
  exit 1
fi
echo "    one memory through every view"

# Where everything is, for recompn64.
#
# It sits next to the script rather than only inside the app bundle, because
# the script is the pipeline and the bundle is one way of reaching it -- and on
# a platform with no bundle it is the only way. The app gets a copy.
step "Writing the toolchain configuration"
cat > "$HERE/src/toolchain.conf" <<CONF
# Written by N64Bundler/build.sh. Absolute paths to the ModernReality build
# that analyses, recompiles, and runs ROMs. Re-run build.sh if the checkout
# moves: the generated apps reference these paths directly.
REPO_ROOT=$(printf '%q' "$ROOT")
MR_SRC=$(printf '%q' "$MR_SRC")
MR_BUILD=$(printf '%q' "$MR_BUILD")
RECOMP_BIN_DIR=$(printf '%q' "$RECOMP_BIN_DIR")
RUNTIME_PROVIDES=$(printf '%q' "$PROVIDES")
SIGNATURES=$(printf '%q' "$SIGNATURES")
TITLES_DIR=$(printf '%q' "$HERE/titles")
APPS_DIR=$(printf '%q' "$INSTALL_DIR")
# Where cmake, ninja and clang were found. A Finder-launched app gets launchd's
# PATH, which has no Homebrew on it, so the pipeline puts these back itself.
TOOLCHAIN_PATH=$(printf '%q' "$TOOLCHAIN_PATH")
CONF
echo "    $HERE/src/toolchain.conf"

if [ "$NO_WINDOW" -eq 1 ]; then
  printf '\n%sTools and host built.%s Recompile a ROM with:\n' "$bold" "$off"
  printf '  %s analyze <rom.z64> --out-dir <dir> --runtime-provides %s --titles %s\n' \
    "$MR_BUILD/n64rip" "$PROVIDES" "$HERE/titles"
  printf '  %s build --analysis <dir> --rom <rom.z64> --out <game.%s>\n' \
    "$MR_BUILD/n64b-port" "$LIB_EXT"
  printf '\nAnd drive it from a program of your own:\n'
  printf '  %s --module <game.%s> --rom <rom.z64> --gym <name> --headless\n' \
    "$MR_BUILD/host/n64b-run" "$LIB_EXT"
  printf '  (%s is the whole of what passes between the two processes)\n' \
    "$MR_SRC/include/modernreality/gym.h"
  printf '\nOr let the pipeline do all of it and put the game in the library:\n'
  printf '  %s <rom.z64>\n' "$HERE/src/recompn64"
  if [ "$RENDERER" -eq 0 ]; then
    printf '\nThis host has no renderer, so --headless is the only way it will start.\n'
  fi
  exit 0
fi

step "Building the window"
( cd "$HERE/gui" && cargo build --release )
GUI_BIN="$HERE/gui/target/release/N64Bundler"
[ -x "$GUI_BIN" ] || { echo "expected $GUI_BIN to exist" >&2; exit 1; }

step "Assembling N64Bundler.app"
rm -rf "$APP"
MACOS="$APP/Contents/MacOS"
RES="$APP/Contents/Resources"
mkdir -p "$MACOS" "$RES"

install -m 755 "$GUI_BIN" "$MACOS/N64Bundler"
install -m 755 "$HERE/src/recompn64" "$RES/recompn64"
install -m 644 "$HERE/src/make_game_app.py" "$RES/make_game_app.py"

python3 "$HERE/src/make_app_icon.py" --out "$RES/icon.icns" >/dev/null

# The document types are what makes a ROM openable with N64Bundler from
# Finder, and what makes a drop onto the Dock icon start a job.
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>N64Bundler</string>
  <key>CFBundleDisplayName</key><string>N64Bundler</string>
  <key>CFBundleExecutable</key><string>N64Bundler</string>
  <key>CFBundleIdentifier</key><string>n64.n64bundler.app</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Nintendo 64 ROM</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>LSHandlerRank</key><string>Alternate</string>
      <key>CFBundleTypeExtensions</key>
      <array><string>z64</string><string>n64</string><string>v64</string></array>
    </dict>
  </array>
</dict>
</plist>
PLIST

install -m 644 "$HERE/src/toolchain.conf" "$RES/toolchain.conf"

if [ "$INSTALL" -eq 1 ]; then
  step "Installing to $INSTALL_DIR"
  mkdir -p "$INSTALL_DIR"
  rm -rf "$INSTALL_DIR/N64Bundler.app"
  cp -R "$APP" "$INSTALL_DIR/N64Bundler.app"
  # Finder caches bundle metadata aggressively; re-registering makes a rebuilt
  # app pick up its icon and the ROM document types.
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister \
    -f "$INSTALL_DIR/N64Bundler.app" >/dev/null 2>&1 || true
  echo "    $INSTALL_DIR/N64Bundler.app"
fi

printf '\n%sDone.%s Drop a ROM on N64Bundler, or:\n' "$bold" "$off"
printf '  %s <rom.z64>\n' "$RES/recompn64"
