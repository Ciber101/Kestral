#!/usr/bin/env bash
# ===================================================================
#  Kestral build script for Linux (any distro: Debian/Ubuntu/Mint/Pop,
#  Fedora/RHEL, Arch/Manjaro/EndeavourOS, openSUSE, and others).
#  First run: installs the system packages it needs (asks first),
#  downloads vcpkg and builds the dependencies (OpenCASCADE is the big
#  one, 20 to 40 minutes once). Later builds take seconds.
#  Set KESTRAL_BUILD to use a different build folder.
#  Set KESTRAL_NO_DEPS=1 to skip the system package step.
# ===================================================================
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KESTRAL_BUILD="${KESTRAL_BUILD:-${XDG_CACHE_HOME:-$HOME/.cache}/kestral-build}"
VCPKG_ROOT="$KESTRAL_BUILD/vcpkg"
export VCPKG_DISABLE_METRICS=1
say() { printf '\033[1;33m[Kestral]\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- system packages
install_deps() {
  local pm="" pkgs=""
  if command -v apt-get >/dev/null; then
    pm="apt-get install -y"
    pkgs="build-essential cmake ninja-build git curl zip unzip tar pkg-config autoconf automake autoconf-archive libtool bison python3
          libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxkbcommon-dev libwayland-dev wayland-protocols
          extra-cmake-modules libgl1-mesa-dev libglu1-mesa-dev libvulkan-dev mesa-vulkan-drivers zenity"
  elif command -v dnf >/dev/null; then
    pm="dnf install -y"
    pkgs="gcc-c++ make cmake ninja-build git curl zip unzip tar pkgconf autoconf automake autoconf-archive libtool bison python3 perl
          libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel libxkbcommon-devel wayland-devel
          wayland-protocols-devel extra-cmake-modules mesa-libGL-devel mesa-libGLU-devel vulkan-loader-devel mesa-vulkan-drivers zenity"
  elif command -v pacman >/dev/null; then
    pm="pacman -S --needed --noconfirm"
    pkgs="base-devel cmake ninja git curl zip unzip tar pkgconf autoconf automake autoconf-archive libtool bison python
          libx11 libxrandr libxinerama libxcursor libxi libxext libxkbcommon wayland wayland-protocols extra-cmake-modules
          mesa glu vulkan-icd-loader vulkan-headers zenity"
  elif command -v zypper >/dev/null; then
    pm="zypper install -y"
    pkgs="gcc-c++ make cmake ninja git curl zip unzip tar pkg-config autoconf automake autoconf-archive libtool bison python3
          libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel libxkbcommon-devel wayland-devel
          wayland-protocols-devel extra-cmake-modules Mesa-libGL-devel glu-devel vulkan-devel zenity"
  else
    say "Unknown package manager. Install by hand: a C++20 compiler (GCC 11+ or Clang 14+), cmake, ninja, git, curl, zip,"
    say "unzip, pkg-config, autoconf, automake, libtool, bison, python3, X11 and Wayland dev headers, Vulkan loader + headers,"
    say "and zenity or kdialog (file dialogs). Then run this again with KESTRAL_NO_DEPS=1."
    return
  fi
  local sudo=""
  [ "$(id -u)" -ne 0 ] && sudo="sudo"
  say "Kestral needs these system packages:"
  echo "  $pm $(echo $pkgs)"
  read -r -p "Install them now? [Y/n] " ans || ans="y"
  case "${ans:-y}" in
    [Nn]*) say "Skipped. Install them yourself if the build fails." ;;
    *) $sudo $pm $pkgs ;;
  esac
}
if [ "${KESTRAL_NO_DEPS:-0}" != "1" ] && [ ! -f "$KESTRAL_BUILD/.deps_ok" ]; then
  install_deps
  mkdir -p "$KESTRAL_BUILD" && touch "$KESTRAL_BUILD/.deps_ok"
fi

# ---------------------------------------------------------------- vcpkg
mkdir -p "$KESTRAL_BUILD"
if [ ! -d "$VCPKG_ROOT/.git" ]; then
  say "Downloading vcpkg..."
  git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi
git -C "$VCPKG_ROOT" fetch -q origin >/dev/null 2>&1 || true
if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

case "$(uname -m)" in
  aarch64|arm64) TRIPLET=arm64-linux ;;
  *) TRIPLET=x64-linux ;;
esac

# ---------------------------------------------------------------- build
say "Configuring (the first time builds dependencies, be patient)..."
cmake -S "$SRC" -B "$KESTRAL_BUILD/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET="$TRIPLET" \
  -DVCPKG_INSTALLED_DIR="$KESTRAL_BUILD/vcpkg_installed"

say "Building..."
cmake --build "$KESTRAL_BUILD/build"

mkdir -p "$SRC/bin"
cp -f "$KESTRAL_BUILD/build/Kestral" "$SRC/bin/Kestral"

# ---------------------------------------------------------------- menu entry (this user only)
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
mkdir -p "$APPS" "$ICONS/256x256/apps" "$ICONS/scalable/apps"
cp -f "$SRC/assets/kestral_icon_256.png" "$ICONS/256x256/apps/kestral.png"
cp -f "$SRC/assets/kestral_icon.svg" "$ICONS/scalable/apps/kestral.svg"
ESC="$(printf '%s' "$SRC" | sed 's/[&|\\]/\\&/g')"
sed "s|^Exec=.*|Exec=\"$ESC/bin/Kestral\" %F|" "$SRC/assets/kestral.desktop" > "$APPS/kestral.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database -q "$APPS" || true

echo
say "Done. Run: ./run.sh   (or find Kestral in your app menu)"
