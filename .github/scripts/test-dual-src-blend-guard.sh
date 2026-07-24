#!/usr/bin/env bash
#
# Regression suite for check-dual-src-blend.sh.
#
# The guard has been hardened three times now, and twice a fix for a false
# positive opened a false NEGATIVE -- the direction that matters, because a guard
# that fails open looks exactly like a guard that passes. Round 2 loosened
# "#if !defined" (adjacent) into "mentions the macro and mentions !defined
# somewhere", which accepted a secondary output in the FALLBACK branch; the same
# round widened check 1's window from the line to the statement, which let a
# comment carrying the word dualSrcBlend stand in for the gate.
#
# Both were found by someone probing the script rather than reading it. This file
# makes that probing permanent, so the next person who tightens or loosens a
# regex finds out immediately instead of two reviews later.
#
# Fixtures are synthesized, not copied from lib/rt64, so this runs without the
# submodule and tests the script's logic rather than today's rt64 contents.
set -uo pipefail

GUARD="$(cd "$(dirname "$0")" && pwd)/check-dual-src-blend.sh"
if [ ! -x "${GUARD}" ]; then
    printf 'FAIL: %s not found or not executable\n' "${GUARD}" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "${tmp}"' EXIT
passed=0
failed=0

# The C++ and shader sites the guard polices, with one interesting fragment
# swapped in per case. Everything else is scaffolding.
GOOD_CPP='targetBlend.srcBlend = dualSrcBlend ? RenderBlend::SRC1_ALPHA : RenderBlend::SRC_ALPHA;'
GOOD_HLSL='#if !defined(NO_DUAL_SRC_BLEND)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'

build_fixture() { # $1 = dir, $2 = cpp fragment, $3 = hlsl fragment
    local dir="$1"
    rm -rf "${dir}"
    mkdir -p "${dir}/lib/rt64/src/render" "${dir}/lib/rt64/src/shaders" "${dir}/.github/scripts"
    cp "${GUARD}" "${dir}/.github/scripts/"
    cat > "${dir}/lib/rt64/src/render/rt64_raster_shader.cpp" <<CPP
namespace RT64 {
    std::unique_ptr<RenderPipeline> RasterShader::createPipeline(const PipelineCreation &c) {
        const bool dualSrcBlend = c.device->getCapabilities().dualSrcBlend;
        RenderBlendDesc &targetBlend = pipelineDesc.renderTargetBlend[0];
        if (c.alphaBlend) {
            targetBlend.blendEnabled = true;
$2
        }
        return c.device->createGraphicsPipeline(pipelineDesc);
    }
}
CPP
    cat > "${dir}/lib/rt64/src/shaders/RasterPS.hlsl" <<HLSL
void PSMain(
      in float4 vertexPosition : SV_POSITION
    , [[vk::location(0)]] [[vk::index(0)]] out float4 pixelColor : SV_TARGET0
$3
)
{
    pixelColor = 0.0f;
}
HLSL
}

check() { # $1 = name, $2 = expected exit, $3 = cpp fragment, $4 = hlsl fragment
    local name="$1" expect="$2" dir="${tmp}/$1" out rc
    build_fixture "${dir}" "$3" "$4"
    out=$(cd "${dir}" && ./.github/scripts/check-dual-src-blend.sh 2>&1)
    rc=$?
    if [ "${rc}" -eq "${expect}" ]; then
        passed=$((passed + 1))
        printf '  ok    %-24s (exit %d)\n' "${name}" "${rc}"
    else
        failed=$((failed + 1))
        printf '  FAIL  %-24s (expected exit %d, got %d)\n' "${name}" "${expect}" "${rc}"
        printf '%s\n' "${out}" | sed 's/^/          /'
    fi
}

cpp_case()   { check "$1" "$2" "$3" "${GOOD_HLSL}"; }
hlsl_case()  { check "$1" "$2" "${GOOD_CPP}" "$3"; }

printf 'check 1 -- the SRC1_ blend factor must be gated by dualSrcBlend\n'
cpp_case gated-same-line     0 "${GOOD_CPP}"
cpp_case gated-multiline     0 'targetBlend.srcBlend = dualSrcBlend
                ? RenderBlend::SRC1_ALPHA
                : RenderBlend::SRC_ALPHA;'
cpp_case gated-with-comments 0 '// pick the factor
            targetBlend.srcBlend = dualSrcBlend
                /* fallback below */
                ? RenderBlend::SRC1_ALPHA
                : RenderBlend::SRC_ALPHA;'
cpp_case bare                1 'targetBlend.srcBlend = RenderBlend::SRC1_ALPHA;'
cpp_case gate-in-line-comment 1 '// dualSrcBlend chooses this
            targetBlend.srcBlend = RenderBlend::SRC1_ALPHA;'
cpp_case gate-in-block-comment 1 '/* dualSrcBlend chooses this */
            targetBlend.srcBlend = RenderBlend::SRC1_ALPHA;'
cpp_case gate-in-multiline-comment 1 '/* the dualSrcBlend
               capability picks this */
            targetBlend.srcBlend = RenderBlend::SRC1_ALPHA;'
cpp_case gate-in-trailing-comment 1 'targetBlend.srcBlend = RenderBlend::SRC1_ALPHA; // dualSrcBlend'
cpp_case gate-in-enclosing-if 1 'if (dualSrcBlend) {
                targetBlend.srcBlend = RenderBlend::SRC1_ALPHA;
            }'
cpp_case no-factor-at-all    1 'targetBlend.srcBlend = RenderBlend::SRC_ALPHA;'

printf 'check 2 -- the secondary output must be gated by NO_DUAL_SRC_BLEND\n'
hlsl_case guarded-if-defined 0 "${GOOD_HLSL}"
hlsl_case guarded-ifndef     0 '#ifndef NO_DUAL_SRC_BLEND
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case guarded-no-parens  0 '#if !defined NO_DUAL_SRC_BLEND
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case guarded-and-narrowed 0 '#if !defined(NO_DUAL_SRC_BLEND) && defined(SOMETHING)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case guarded-elif-reestablished 0 '#if defined(SOMETHING_ELSE)
    , in float4 unrelated : COLOR7
#elif !defined(NO_DUAL_SRC_BLEND)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case ungated            1 '    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1'
hlsl_case in-else-branch     1 '#if !defined(NO_DUAL_SRC_BLEND)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#else
    , [[vk::location(0)]] [[vk::index(1)]] out float4 oops : SV_TARGET1
#endif'
hlsl_case in-elif-branch     1 '#if !defined(NO_DUAL_SRC_BLEND)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#elif defined(SOMETHING_ELSE)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 oops : SV_TARGET1
#endif'
# The negation must apply to NO_DUAL_SRC_BLEND itself: this is the fallback
# branch, so a secondary output here is the original white-screen bug.
hlsl_case negation-on-other-macro 1 '#if defined(NO_DUAL_SRC_BLEND) && !defined(FOO)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case or-widens-region   1 '#if !defined(NO_DUAL_SRC_BLEND) || defined(FOO)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case positive-ifdef     1 '#ifdef NO_DUAL_SRC_BLEND
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case similar-macro-name 1 '#if !defined(NO_DUAL_SRC_BLEND_EXTRA)
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif'
hlsl_case commented-out-endif 1 '#if !defined(NO_DUAL_SRC_BLEND)
//#endif
#endif
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1'
hlsl_case no-output-at-all   1 '    , in float4 unrelated : COLOR7'

printf 'structure -- the guard must not pass vacuously\n'
newshader_dir="${tmp}/new-shader-escapes"
build_fixture "${newshader_dir}" "${GOOD_CPP}" "${GOOD_HLSL}"
printf 'void OtherPS(out float4 c : SV_TARGET0, out float4 a : SV_TARGET1) { c = 1; a = 1; }\n' \
    > "${newshader_dir}/lib/rt64/src/shaders/OtherPS.hlsl"
if (cd "${newshader_dir}" && ./.github/scripts/check-dual-src-blend.sh >/dev/null 2>&1); then
    failed=$((failed + 1)); printf '  FAIL  %-24s (a new shader escaped the scan)\n' "new-shader-escapes"
else
    passed=$((passed + 1)); printf '  ok    %-24s (exit 1)\n' "new-shader-escapes"
fi

missing_dir="${tmp}/missing-submodule"
build_fixture "${missing_dir}" "${GOOD_CPP}" "${GOOD_HLSL}"
rm -rf "${missing_dir}/lib/rt64/src"
if (cd "${missing_dir}" && ./.github/scripts/check-dual-src-blend.sh >/dev/null 2>&1); then
    failed=$((failed + 1)); printf '  FAIL  %-24s (passed with nothing to scan)\n' "missing-submodule"
else
    passed=$((passed + 1)); printf '  ok    %-24s (exit 1)\n' "missing-submodule"
fi

anchor_dir="${tmp}/missing-anchor-shader"
build_fixture "${anchor_dir}" "${GOOD_CPP}" "${GOOD_HLSL}"
rm "${anchor_dir}/lib/rt64/src/shaders/RasterPS.hlsl"
if (cd "${anchor_dir}" && ./.github/scripts/check-dual-src-blend.sh >/dev/null 2>&1); then
    failed=$((failed + 1)); printf '  FAIL  %-24s (passed with the anchor gone)\n' "missing-anchor-shader"
else
    passed=$((passed + 1)); printf '  ok    %-24s (exit 1)\n' "missing-anchor-shader"
fi

printf '\n%d passed, %d failed\n' "${passed}" "${failed}"
[ "${failed}" -eq 0 ]
