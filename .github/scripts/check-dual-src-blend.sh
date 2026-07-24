#!/usr/bin/env bash
#
# Guard: dual source blending must stay gated behind the device capability.
#
# RT64 implements N64 alpha blending entirely with dual source blending -- the
# blend factor rides in SV_TARGET1 so SV_TARGET0.a can carry N64 coverage. No
# Mali GPU supports the dualSrcBlend feature, the Mali driver returns VK_SUCCESS
# for the resulting invalid pipeline anyway, and then blends undefined. That was
# issue #15: a white screen with nothing wrong in any log.
#
# The fix keeps two paths. With the capability: SRC1_ALPHA/INV_SRC1_ALPHA and the
# secondary shader output, byte-identical to upstream. Without it: the
# NO_DUAL_SRC_BLEND shader variant, which drops the secondary output and carries
# the factor in the primary output's alpha, blended with SRC_ALPHA/INV_SRC_ALPHA.
#
# Both halves have to stay gated. An ungated SRC1_ factor or an ungated secondary
# output brings the white screen back on hardware that no CI runner and none of
# our own test devices have -- silently, because nothing fails on Adreno. This
# check is the tripwire. It is deliberately source-level and dependency-free so
# it cannot fail open the way a check needing an external tool can.
#
# Run from the repo root. Requires the lib/rt64 submodule to be checked out.
set -euo pipefail

RT64_SRC="lib/rt64/src"
SHADER="${RT64_SRC}/shaders/RasterPS.hlsl"
status=0

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    status=1
}

# --- Preflight -------------------------------------------------------------
# A guard that silently scans nothing passes forever. Prove the inputs are here
# and that each check still sees the site it exists to police before trusting a
# clean result.
if [ ! -d "${RT64_SRC}" ]; then
    fail "${RT64_SRC} not found -- is the lib/rt64 submodule checked out?"
    exit 1
fi
if [ ! -f "${SHADER}" ]; then
    fail "${SHADER} not found -- the shader moved, so this guard no longer covers it."
    exit 1
fi

# Every C++ source RT64 builds, minus vendored third party code (the GL headers
# under src/contrib define unrelated GL_SRC1_* constants).
cpp_files=$(find "${RT64_SRC}" -path "${RT64_SRC}/contrib" -prune -o \
    \( -name '*.cpp' -o -name '*.h' \) -print | sort)
if [ -z "${cpp_files}" ]; then
    fail "no C++ sources found under ${RT64_SRC} -- the layout changed and this guard is scanning nothing."
    exit 1
fi

# --- Check 1: every SRC1_ blend factor is selected by the capability --------
# Each use must sit on a line that also names dualSrcBlend, i.e. the ternary that
# picks the fallback factor. A bare RenderBlend::SRC1_ALPHA is the regression.
src1_hits=$(printf '%s\n' "${cpp_files}" | xargs grep -n 'SRC1_' || true)
src1_total=0
src1_gated=0
while IFS= read -r hit; do
    [ -n "${hit}" ] || continue
    src1_total=$((src1_total + 1))
    if printf '%s' "${hit}" | grep -q 'dualSrcBlend'; then
        src1_gated=$((src1_gated + 1))
    else
        fail "ungated dual source blend factor: ${hit}"
        printf '      every SRC1_ factor must be chosen by the dualSrcBlend capability.\n' >&2
    fi
done <<< "${src1_hits}"

if [ "${src1_total}" -eq 0 ]; then
    fail "no SRC1_ blend factors found at all. Either the dual source path was removed (update this guard) or the scan is broken."
fi

# --- Check 2: the secondary shader output stays behind NO_DUAL_SRC_BLEND ----
# Walks the preprocessor nesting and requires every SV_TARGET1 / vk::index(1) to
# sit inside a "#if !defined(NO_DUAL_SRC_BLEND)" (or #ifndef) region. An #else
# drops the guard, so a secondary output in the fallback branch is flagged too.
shader_report=$(awk '
    function guarded(   d) {
        for (d = 1; d <= depth; d++) if (guard[d]) return 1
        return 0
    }
    /^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef)/ {
        depth++
        guard[depth] = 0
        if ($0 ~ /NO_DUAL_SRC_BLEND/ && ($0 ~ /#[[:space:]]*if[[:space:]]*![[:space:]]*defined/ || $0 ~ /#[[:space:]]*ifndef/)) {
            guard[depth] = 1
        }
        next
    }
    /^[[:space:]]*#[[:space:]]*else/ { if (depth > 0) guard[depth] = 0; next }
    /^[[:space:]]*#[[:space:]]*endif/ { if (depth > 0) { guard[depth] = 0; depth-- } next }
    /SV_TARGET1|vk::index\(1\)/ {
        total++
        if (guarded()) { gated++ } else { printf "UNGATED %d: %s\n", FNR, $0 }
    }
    END { printf "TOTAL %d %d\n", total, gated }
' "${SHADER}")

shader_total=$(printf '%s\n' "${shader_report}" | awk '/^TOTAL/ { print $2 }')
shader_gated=$(printf '%s\n' "${shader_report}" | awk '/^TOTAL/ { print $3 }')
while IFS= read -r line; do
    case "${line}" in
        UNGATED*)
            fail "ungated dual source shader output in ${SHADER}, line ${line#UNGATED }"
            printf '      the secondary output must stay inside #if !defined(NO_DUAL_SRC_BLEND).\n' >&2
            ;;
    esac
done <<< "${shader_report}"

if [ "${shader_total}" -eq 0 ]; then
    fail "no secondary shader output found in ${SHADER}. Either the dual source path was removed (update this guard) or the scan is broken."
fi

# --- Result ----------------------------------------------------------------
if [ "${status}" -eq 0 ]; then
    printf 'dual source blending is gated: %d/%d blend factors, %d/%d shader outputs.\n' \
        "${src1_gated}" "${src1_total}" "${shader_gated}" "${shader_total}"
else
    printf '\nSee the issue #15 notes in CLAUDE.md for why this matters.\n' >&2
fi
exit "${status}"
