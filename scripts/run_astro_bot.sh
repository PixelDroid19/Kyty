#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_ROOT="${1:-${KYTY_GUEST_ROOT:-}}"

if [[ -z "${GAME_ROOT}" ]]; then
	echo "error: set KYTY_GUEST_ROOT or pass the guest root as the first argument" >&2
	exit 1
fi

if [[ ! -f "${GAME_ROOT}/eboot.bin" ]]; then
	echo "error: eboot.bin not found under: ${GAME_ROOT}" >&2
	exit 1
fi

exec "${ROOT}/_build_linux/fc_script" "${ROOT}/scripts/run_guest.lua" "${GAME_ROOT}"
