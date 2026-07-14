// Final composite, used only when HDR output is active: the FP16 scene
// target -> the HDR swapchain. The scene holds extended-sRGB-encoded values
// with 1.0 == SDR white; SDR content stays in [0,1], HDR video highlights
// exceed 1.0. SDR presentation doesn't run this pass — a plain blit suffices
// there.
// Resource bindings follow the SDL_GPU HLSL convention for fragment shaders.

Texture2D uTex : register(t0, space2);
SamplerState uSmp : register(s0, space2);

cbuffer CompositeParams : register(b0, space3) {
    int uMode;        // 1 = scRGB (linear FP16), 2 = HDR10 (PQ, BT.2020)
    float uSdrWhite;  // SDR white in scRGB units (SDL_PROP_WINDOW_SDR_WHITE_LEVEL)
    float2 _pad;
};

// Extended sRGB EOTF: the standard piecewise curve, continued monotonically
// past 1.0 (inputs are >= 0). Inverse of the encode in tonemap.frag.hlsl.
float3 srgbDecodeExt(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

// BT.709 -> BT.2020 in linear light (row-major, applied as mul(M, v)).
static const float3x3 bt709_to_2020 = float3x3(
    0.627404, 0.329283, 0.043313,
    0.069097, 0.919540, 0.011362,
    0.016391, 0.088013, 0.895595
);

// SMPTE ST 2084 (PQ) inverse EOTF: linear (1.0 == 10000 nits) -> encoded [0,1].
float3 pqInvEotf(float3 lin) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 y = pow(max(lin, 0.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main(float2 vUV : TEXCOORD0) : SV_Target {
    float3 c = uTex.Sample(uSmp, vUV).rgb;

    // Linear, 709 primaries, in scRGB units (1.0 == 80 nits). uSdrWhite maps
    // our SDR-white-relative values to the display's SDR brightness setting.
    float3 lin = srgbDecodeExt(c) * uSdrWhite;
    if (uMode == 1)
        return float4(lin, 1.0);

    // HDR10: absolute nits -> BT.2020 -> PQ.
    float3 lin2020 = mul(bt709_to_2020, lin * (80.0 / 10000.0));
    return float4(pqInvEotf(lin2020), 1.0);
}
