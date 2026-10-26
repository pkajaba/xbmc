# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026-present Pavel Kajaba
#
# UNVERIFIED in a CoreELEC tree: none was available when this was written.
# The offline cargo build of the workspace was checked on a Mac; the toolchain
# variables below follow the LibreELEC/CoreELEC convention and need a first
# build to confirm (rust package name, how its environment is loaded, whether
# the target's std is built).
#
# The source is the harlettizer release tarball: the workspace with its
# crates.io and git dependencies vendored (`cargo vendor`, with the source
# replacement in .cargo/config.toml). PKG_URL and PKG_SHA256 below are
# placeholders until a release exists: put the release tarball's URL and
# sha256 here, or put the tarball into the tree's sources/dtsx2atmos/
# directory and keep only the sha256.

PKG_NAME="dtsx2atmos"
PKG_VERSION="0.1.0"
PKG_SHA256="@SHA256_OF_THE_RELEASE_TARBALL@"
PKG_LICENSE="GPL-3.0-or-later"
PKG_SITE="https://github.com/harletty/harlettizer"
PKG_URL="@HARLETTIZER_RELEASE_TARBALL_URL@/harlettizer-${PKG_VERSION}.tar.xz"
PKG_DEPENDS_TARGET="toolchain rust:host"
PKG_SECTION="audio"
PKG_SHORTDESC="Real-time in-memory DTS to Dolby TrueHD conversion library"
PKG_LONGDESC="In-memory streaming converter (hz-stream, built as the hz-stream-ffi library) turning DTS and DTS:X bitstreams into Dolby TrueHD access units for MAT packing and playback over HDMI eARC."
PKG_TOOLCHAIN="manual"
# The crates need Rust 1.89 or newer (the DTS decoder declares it).
PKG_RUST_MIN="1.89"

configure_package() {
  case "${TARGET_ARCH}" in
    aarch64) RUST_TARGET="aarch64-unknown-linux-gnu" ;;
    arm)     RUST_TARGET="armv7-unknown-linux-gnueabihf" ;;
    *)       die "dtsx2atmos: no rust target for ${TARGET_ARCH}" ;;
  esac
  RUST_TARGET_ENV="$(echo "${RUST_TARGET}" | tr 'a-z-' 'A-Z_')"
}

# Core pinning (runtime, nothing to build): the encoder's worker threads stay on
# the big cores. The library picks the cores that report the highest capacity
# (S922X: 2-5), HZ_STREAM_CORES="2,3,4,5" (or "none") overrides that, and a
# host may call hz_stream_set_cpu_cores before the first context. If the
# kernel reports no capacities, start the player under `taskset -c 2-5`.

pre_make_target() {
  local have
  have="$(rustc --version | cut -d' ' -f2)"
  if [ "$(printf '%s\n%s\n' "${PKG_RUST_MIN}" "${have}" | sort -V | head -1)" != "${PKG_RUST_MIN}" ]; then
    die "dtsx2atmos needs rustc >= ${PKG_RUST_MIN}, found ${have}"
  fi
}

make_target() {
  # The source tree is the unpacked tarball root, the workspace root.
  cd "${PKG_BUILD}"
  export "CARGO_TARGET_${RUST_TARGET_ENV}_LINKER=${TARGET_CC}"
  # Keep what the tree exports and add the tuning for the Cortex-A73 cores.
  export RUSTFLAGS="${RUSTFLAGS} -C target-cpu=cortex-a73"
  cargo build --release --offline --frozen --target "${RUST_TARGET}" -p hz-stream-ffi
}

makeinstall_target() {
  mkdir -p "${INSTALL}/usr/lib" "${INSTALL}/usr/include"
  cp "${PKG_BUILD}/target/${RUST_TARGET}/release/libhz_stream_ffi.so" "${INSTALL}/usr/lib/"
  cp "${PKG_BUILD}/crates/hz-stream-ffi/include/hz_stream.h" "${INSTALL}/usr/include/"
}
