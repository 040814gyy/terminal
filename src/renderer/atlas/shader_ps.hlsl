// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "dwrite.hlsl"
#include "shader_common.hlsl"

SamplerState backgroundSampler : register(s0);
Texture2D<float4> background : register(t0);
Texture2D<float4> glyphAtlas : register(t1);

struct Output
{
    float4 color;
    float4 weights;
};

// clang-format off
Output main(PSData data) : SV_Target
// clang-format on
{
    float4 color;
    float4 weights;

    switch (data.shadingType)
    {
    case SHADING_TYPE_TEXT_BACKGROUND:
        color = background.Sample(backgroundSampler, data.texcoord);
        weights = float4(1, 1, 1, 1);
        break;
    case SHADING_TYPE_TEXT_GRAYSCALE:
    {
        // These are independent of the glyph texture and could be moved to the vertex shader.
        const float4 foreground = premultiplyColor(data.color);
        const float blendEnhancedContrast = DWrite_ApplyLightOnDarkContrastAdjustment(grayscaleEnhancedContrast, data.color.rgb);
        const float intensity = DWrite_CalcColorIntensity(data.color.rgb);
        // These aren't.
        const float4 glyph = glyphAtlas[data.texcoord];
        const float contrasted = DWrite_EnhanceContrast(glyph.a, blendEnhancedContrast);
        const float alphaCorrected = DWrite_ApplyAlphaCorrection(contrasted, intensity, gammaRatios);
        color = alphaCorrected * foreground;
        weights = color.aaaa;
        break;
    }
    case SHADING_TYPE_TEXT_CLEARTYPE:
    {
        // These are independent of the glyph texture and could be moved to the vertex shader.
        const float blendEnhancedContrast = DWrite_ApplyLightOnDarkContrastAdjustment(cleartypeEnhancedContrast, data.color.rgb);
        // These aren't.
        const float4 glyph = glyphAtlas[data.texcoord];
        const float3 contrasted = DWrite_EnhanceContrast3(glyph.rgb, blendEnhancedContrast);
        const float3 alphaCorrected = DWrite_ApplyAlphaCorrection3(contrasted, data.color.rgb, gammaRatios);
        weights = float4(alphaCorrected * data.color.a, 1);
        color = weights * data.color;
        break;
    }
    case SHADING_TYPE_PASSTHROUGH:
    {
        color = glyphAtlas[data.texcoord];
        weights = color.aaaa;
        break;
    }
    case SHADING_TYPE_PASSTHROUGH_INVERT:
    {
        color = glyphAtlas[data.texcoord];
        color.rgb = color.aaa - color.rgb;
        weights = color.aaaa;
        break;
    }
    case SHADING_TYPE_DASHED_LINE:
    {
        const bool on = frac(data.position.x / dashedLineLength) < 0.333333333f;
        color = on * premultiplyColor(data.color);
        weights = color.aaaa;
        break;
    }
    case SHADING_TYPE_SOLID_FILL:
    default:
    {
        color = premultiplyColor(data.color);
        weights = color.aaaa;
        break;
    }
    }

    Output output;
    output.color = color;
    output.weights = weights;
    return output;
}
