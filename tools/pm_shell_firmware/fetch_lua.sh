#!/usr/bin/env bash
# fetch_lua.sh — Download Lua 5.4.7 source and stage into lib/lua/
#
# This script fetches the official Lua tarball from lua.org and unpacks
# the .c and .h source files (minus the CLI tools `lua.c` and `luac.c`)
# into lib/lua/, where PlatformIO will pick them up automatically.
#
# Run once per fresh clone. Idempotent — skips download if tarball already
# present; refuses to overwrite an existing populated lib/lua/ without
# --force.
#
# Usage:
#   ./fetch_lua.sh           # standard install
#   ./fetch_lua.sh --force   # wipe lib/lua/ first
#
# License note: Lua is MIT-licensed. See lua.org/license.html.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

LUA_VERSION="5.4.7"
LUA_URL="https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"
LUA_TARBALL="lua-${LUA_VERSION}.tar.gz"
LUA_SRCDIR="lua-${LUA_VERSION}"
TARGET_DIR="lib/lua"

# ── Argument parsing ────────────────────────────────────
FORCE=0
for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        -h|--help)
            sed -n 's/^# \{0,1\}//p' "$0" | head -n 20
            exit 0
            ;;
        *) echo "Unknown argument: ${arg}" >&2; exit 2 ;;
    esac
done

# ── Sanity checks ───────────────────────────────────────
if [[ -d "${TARGET_DIR}" ]] && [[ -n "$(ls -A "${TARGET_DIR}" 2>/dev/null | grep -E '\.(c|h)$' || true)" ]]; then
    if [[ "${FORCE}" -eq 0 ]]; then
        echo "lib/lua/ already contains Lua sources. Use --force to overwrite."
        echo "Existing files:"
        ls "${TARGET_DIR}" | head -5
        exit 0
    else
        echo "[--force] Removing existing lib/lua/ contents..."
        find "${TARGET_DIR}" -mindepth 1 ! -name 'README.md' -delete
    fi
fi

mkdir -p "${TARGET_DIR}"

# ── Download ────────────────────────────────────────────
if [[ ! -f "${LUA_TARBALL}" ]]; then
    echo "Downloading ${LUA_URL}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --progress-bar -o "${LUA_TARBALL}" "${LUA_URL}"
    elif command -v wget >/dev/null 2>&1; then
        wget --show-progress -O "${LUA_TARBALL}" "${LUA_URL}"
    else
        echo "ERROR: neither curl nor wget found. Install one or download manually:" >&2
        echo "  ${LUA_URL}" >&2
        echo "Save as ${LUA_TARBALL} in this directory, then re-run." >&2
        exit 1
    fi
else
    echo "Tarball already present, skipping download."
fi

# ── Extract ─────────────────────────────────────────────
echo "Extracting..."
tar -xzf "${LUA_TARBALL}"

# ── Stage sources ───────────────────────────────────────
echo "Staging ${LUA_SRCDIR}/src/*.{c,h} → ${TARGET_DIR}/ ..."
cp "${LUA_SRCDIR}/src/"*.c "${TARGET_DIR}/"
cp "${LUA_SRCDIR}/src/"*.h "${TARGET_DIR}/"

# ── Strip CLI entry points ──────────────────────────────
# lua.c contains main() for the standalone interpreter.
# luac.c contains main() for the bytecode compiler.
# Both would conflict with our main.cpp's main(). Drop them.
echo "Removing CLI entrypoints (lua.c, luac.c)..."
rm -f "${TARGET_DIR}/lua.c"
rm -f "${TARGET_DIR}/luac.c"

# ── Cleanup ─────────────────────────────────────────────
rm -rf "${LUA_SRCDIR}"

# ── Verify ──────────────────────────────────────────────
C_COUNT=$(ls -1 "${TARGET_DIR}/"*.c 2>/dev/null | wc -l)
H_COUNT=$(ls -1 "${TARGET_DIR}/"*.h 2>/dev/null | wc -l)

echo ""
echo "─────────────────────────────────────"
echo "  Lua ${LUA_VERSION} installed:"
echo "    ${C_COUNT} .c files"
echo "    ${H_COUNT} .h files"
echo "    in ${TARGET_DIR}/"
echo "─────────────────────────────────────"
echo ""
echo "Next: pio run -e <env_name>"
echo "  envs: tdeck_plus, tlorapager, cardputer_adv, c28p, maxine"
