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
# OUT OF SCOPE, on purpose: the HLSL text RasterShader::generateShaderText builds
# in C++ also names SV_TARGET1 unconditionally. That text feeds the DXIL and Metal
# backends only, which hardcode the capability true; rt64 asserts
# shaderFormat == SPIRV || capabilities.dualSrcBlend before dispatching to it.
# Policing it here would mean allowlisting a line this guard exists to forbid, so
# the assert covers that path and this script covers the shader sources.
#
# Run from the repo root. Requires the lib/rt64 submodule to be checked out.
set -euo pipefail

RT64_SRC="lib/rt64/src"
ANCHOR_SHADER="${RT64_SRC}/shaders/RasterPS.hlsl"
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
if [ ! -f "${ANCHOR_SHADER}" ]; then
    fail "${ANCHOR_SHADER} not found -- the shader moved, so this guard no longer covers it."
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

# Every shader source, not just RasterPS.hlsl -- a NEW shader declaring a
# secondary output has exactly the same failure mode, so the scan is as wide as
# the C++ half.
shader_files=$(find "${RT64_SRC}" -path "${RT64_SRC}/contrib" -prune -o \
    \( -name '*.hlsl' -o -name '*.hlsli' \) -print | sort)
if [ -z "${shader_files}" ]; then
    fail "no shader sources found under ${RT64_SRC} -- the layout changed and this guard is scanning nothing."
    exit 1
fi

# Both checks emit "UNGATED <where>: <line>" for each violation and one
# "TOTAL <seen> <gated>" per awk invocation. xargs may start awk more than once
# on a long file list, so the totals are summed in the shell rather than trusted
# from a single END block.
sum_totals() { printf '%s\n' "$1" | awk '/^TOTAL/ { t += $2; g += $3 } END { printf "%d %d\n", t, g }'; }

report_ungated() {
    local report="$1" what="$2" hint="$3" line
    while IFS= read -r line; do
        case "${line}" in
            UNGATED*)
                fail "ungated ${what}: ${line#UNGATED }"
                printf '      %s\n' "${hint}" >&2
                ;;
        esac
    done <<< "${report}"
}

# Shared awk prologue. Both checks judge code, not commentary: a comment carrying
# the gate's vocabulary next to an ungated site would otherwise read as the gate
# itself, and a commented-out preprocessor directive would corrupt the nesting
# walk. Block comment state (inblock) is global and reset per file.
AWK_STRIP_COMMENTS='
    function strip_comments(s,   r, p, q) {
        r = ""
        while (length(s) > 0) {
            if (inblock) {
                p = index(s, "*/")
                if (p == 0) { s = ""; break }
                s = substr(s, p + 2)
                inblock = 0
            } else {
                p = index(s, "/*")
                q = index(s, "//")
                if (q > 0 && (p == 0 || q < p)) { r = r substr(s, 1, q - 1); s = ""; break }
                if (p == 0) { r = r s; s = ""; break }
                r = r substr(s, 1, p - 1)
                s = substr(s, p + 2)
                inblock = 1
            }
        }
        return r
    }
'

# --- Check 1: every SRC1_ blend factor is selected by the capability --------
# Each use must be part of a statement that also names dualSrcBlend, i.e. the
# ternary that picks the fallback factor. A bare RenderBlend::SRC1_ALPHA is the
# regression. The statement, not the line, is the unit: a ternary reformatted
# across several lines keeps the gate on the first of them, and judging that line
# by line would fail closed with a message pointing at the wrong thing.
#
# Comments are stripped first. Widening the window from the line to the statement
# also widens what can sit in it, and a "// dualSrcBlend picks this" above a bare
# SRC1_ factor must not read as the gate -- that would be a WIDER false negative
# than the line-based check this replaced.
src1_report=$(printf '%s\n' "${cpp_files}" | xargs awk "${AWK_STRIP_COMMENTS}"'
    FNR == 1 { stmt = ""; inblock = 0 }
    {
        code = strip_comments($0)
        stmt = stmt " " code
        if (code ~ /SRC1_/) {
            total++
            if (stmt ~ /dualSrcBlend/) { gated++ }
            else { printf "UNGATED %s:%d: %s\n", FILENAME, FNR, $0 }
        }
        # Statement boundary: start accumulating again on the next line.
        if (code ~ /[;{}]/) { stmt = "" }
    }
    END { printf "TOTAL %d %d\n", total, gated }
')
read -r src1_total src1_gated <<< "$(sum_totals "${src1_report}")"
report_ungated "${src1_report}" "dual source blend factor" \
    "every SRC1_ factor must be chosen by the dualSrcBlend capability, in the same statement (a multi-line ternary is fine; an enclosing if() is not)."

if [ "${src1_total}" -eq 0 ]; then
    fail "no SRC1_ blend factors found at all. Either the dual source path was removed (update this guard) or the scan is broken."
fi

# --- Check 2: the secondary shader output stays behind NO_DUAL_SRC_BLEND ----
# Walks the preprocessor nesting and requires every SV_TARGET1 / vk::index(1) to
# sit inside a "#if !defined(NO_DUAL_SRC_BLEND)" (or #ifndef) region. #else drops
# the guard, and so does #elif unless the new condition re-establishes it, so a
# secondary output in a fallback branch is flagged too.
shader_report=$(printf '%s\n' "${shader_files}" | xargs awk "${AWK_STRIP_COMMENTS}"'
    function guarded(   d) {
        for (d = 1; d <= depth; d++) if (guard[d]) return 1
        return 0
    }
    # A guard only counts when the negation applies to NO_DUAL_SRC_BLEND ITSELF.
    # Testing for the macro name and for a "!defined" independently accepts
    # "#if defined(NO_DUAL_SRC_BLEND) && !defined(FOO)", where the negation
    # belongs to some other macro -- that is a secondary output living in the
    # FALLBACK branch, the exact regression this check exists to catch.
    #
    # "&&" is safe: it can only narrow a region that already requires the macro
    # to be undefined. "||" can widen it back into fallback builds, so any
    # condition containing one is not accepted as a guard.
    function is_negated_guard(line) {
        if (line ~ /\|\|/) { return 0 }
        if (line ~ /#[[:space:]]*ifndef[[:space:]]+NO_DUAL_SRC_BLEND([^A-Za-z0-9_]|$)/) { return 1 }
        if (line ~ /![[:space:]]*defined[[:space:]]*\(?[[:space:]]*NO_DUAL_SRC_BLEND([^A-Za-z0-9_]|$)/) { return 1 }
        return 0
    }
    FNR == 1 { depth = 0; inblock = 0 }
    {
        line = strip_comments($0)
        if (line ~ /^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef)/) {
            depth++
            guard[depth] = is_negated_guard(line) ? 1 : 0
            next
        }
        if (line ~ /^[[:space:]]*#[[:space:]]*elif/) {
            if (depth > 0) { guard[depth] = is_negated_guard(line) ? 1 : 0 }
            next
        }
        if (line ~ /^[[:space:]]*#[[:space:]]*else/) { if (depth > 0) { guard[depth] = 0 } next }
        if (line ~ /^[[:space:]]*#[[:space:]]*endif/) { if (depth > 0) { guard[depth] = 0; depth-- } next }
        if (line ~ /SV_TARGET1|vk::index\(1\)/) {
            total++
            if (guarded()) { gated++ } else { printf "UNGATED %s:%d: %s\n", FILENAME, FNR, $0 }
        }
    }
    END { printf "TOTAL %d %d\n", total, gated }
')
read -r shader_total shader_gated <<< "$(sum_totals "${shader_report}")"
report_ungated "${shader_report}" "dual source shader output" \
    "the secondary output must stay inside #if !defined(NO_DUAL_SRC_BLEND) or #ifndef NO_DUAL_SRC_BLEND. A compound condition counts only when the negation applies to NO_DUAL_SRC_BLEND itself and there is no ||."

if [ "${shader_total}" -eq 0 ]; then
    fail "no secondary shader output found in any shader under ${RT64_SRC}. Either the dual source path was removed (update this guard) or the scan is broken."
fi

# --- Result ----------------------------------------------------------------
if [ "${status}" -eq 0 ]; then
    printf 'dual source blending is gated: %d/%d blend factors, %d/%d shader outputs.\n' \
        "${src1_gated}" "${src1_total}" "${shader_gated}" "${shader_total}"
else
    printf '\nSee the issue #15 notes in CLAUDE.md for why this matters.\n' >&2
fi
exit "${status}"
