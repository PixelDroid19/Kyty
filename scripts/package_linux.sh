#!/usr/bin/env bash
# Builds the Linux release archive: the emulator and the Qt launcher with its Qt runtime,
# ready to run after extracting. Qt must be a relocatable build (the official binaries),
# found through CMAKE_PREFIX_PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $# -ne 2 || "$2" != *.tar.gz ]]; then
	echo "usage: $0 <build-dir> <output>.tar.gz" >&2
	exit 1
fi

BUILD_DIR="$(realpath -m "$1")"
ARCHIVE="$(realpath -m "$2")"
PACKAGE_NAME="$(basename "${ARCHIVE}" .tar.gz)"
JOBS="${KYTY_PACKAGE_JOBS:-4}"

cmake -S "${ROOT}/source" -B "${BUILD_DIR}" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DKYTY_PROJECT_NAME=Emulator \
	-DKYTY_WARNINGS_ARE_ERRORS=OFF \
	-DKYTY_BUILD_LAUNCHER=ON \
	-DKYTY_BUILD_DEVTOOLS=OFF \
	-DKYTY_BUILD_UNIT_TESTS=OFF \
	-DKYTY_GENERATE_MAP_CSV=OFF
cmake --build "${BUILD_DIR}" --target fc_script launcher --parallel "${JOBS}"

STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT
PACKAGE_DIR="${STAGE}/${PACKAGE_NAME}"

# Installing only the launcher directory runs its Qt deployment without the vendored libraries' install rules.
cmake --install "${BUILD_DIR}/launcher" --prefix "${PACKAGE_DIR}"
if [[ ! -f "${PACKAGE_DIR}/lib/libQt6Core.so.6" ]]; then
	echo "error: Qt was not bundled; point CMAKE_PREFIX_PATH at a relocatable Qt 6, not a distribution or Nix Qt" >&2
	exit 1
fi

install -m 755 "${BUILD_DIR}/fc_script" "${PACKAGE_DIR}/"
install -m 644 "${ROOT}/scripts/run_guest.lua" "${ROOT}/README.md" "${ROOT}/CHANGELOG.md" "${ROOT}/LICENSE" "${PACKAGE_DIR}/"

mkdir -p "$(dirname "${ARCHIVE}")"
tar -czf "${ARCHIVE}" -C "${STAGE}" "${PACKAGE_NAME}"
echo "${ARCHIVE}"
