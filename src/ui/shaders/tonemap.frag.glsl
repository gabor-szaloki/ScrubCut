#version 330 core
// HDR->SDR tone-mapping pass: decode the HDR transfer (PQ or HLG), gamut-map
// BT.2020 -> BT.709 in linear light, apply the selected operator, sRGB-encode.
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform int uTransfer;    // 0 = PQ, 1 = HLG
uniform int uPrimaries;   // matches the VideoColorPrimaries enum: 0=709, 1=2020, 2=P3
uniform int uTonemapper;  // matches the Tonemapper enum
uniform float uExposure;

// SMPTE ST 2084 (PQ) EOTF: encoded [0,1] -> linear, 1.0 == 10000 nits.
vec3 pqEotf(vec3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    vec3 ep = pow(max(e, 0.0), vec3(1.0 / m2));
    vec3 num = max(ep - c1, 0.0);
    vec3 den = c2 - c3 * ep;
    return pow(num / den, vec3(1.0 / m1));
}

// HLG inverse-OETF (encoded [0,1] -> scene linear [0,1]).
vec3 hlgInvOetf(vec3 e) {
    const float a = 0.17883277;
    const float b = 0.28466892; // 1 - 4a
    const float c = 0.55991073; // 0.5 - a*ln(4a)
    vec3 lo = (e * e) / 3.0;
    vec3 hi = (exp((e - c) / a) + b) / 12.0;
    return mix(lo, hi, step(vec3(0.5), e));
}

// Gamut -> BT.709 in linear light (column-major columns of the row-major
// matrix, so `M * v` gives the intended result).
const mat3 bt2020_to_709 = mat3(
     1.660491, -0.124550, -0.018151,
    -0.587641,  1.132900, -0.100579,
    -0.072850, -0.008349,  1.118730
);
// Display P3 (D65 white, shared with BT.709, so no chromatic adaptation). Also
// used for DCI-P3, whose different white point is a minor approximation here.
const mat3 p3_to_709 = mat3(
     1.224940, -0.042057, -0.019638,
    -0.224940,  1.042057, -0.078636,
     0.000000,  0.000000,  1.098274
);

// --- Tone-mapping operators. Each maps linear (1.0 == SDR diffuse white) to
// display-linear [0,1]; the sRGB OETF is applied afterwards. ---

// Reinhard tone curve: x / (1 + x). Maps 1.0 to 0.5.
vec3 reinhard(vec3 x) {
    return x / (1.0 + x);
}

// Hable / "Uncharted 2" filmic, matching VLC 3 HDR->SDR shader
// http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 hableCurve(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
vec3 uncharted2(vec3 x) {
    const float W = 11.2;
    const float exposureBias = 2.0;
    return hableCurve(exposureBias * x) / hableCurve(vec3(W));
}

// Narkowicz ACES filmic approximation.
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}

// AgX (Troy Sobotka), minimal approximation by Benjamin Wrensch, using the
// neutral "default" look (no contrast/saturation adjustment). Operates through
// its own input/output matrices and outputs display-encoded values.
// https://iolite-engine.com/blog_posts/minimal_agx_implementation
vec3 agxContrast(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}
vec3 agxInner(vec3 val) {
    const mat3 agx_in = mat3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    val = agx_in * val;
    val = clamp(log2(max(val, 1e-10)), min_ev, max_ev);
    val = (val - min_ev) / (max_ev - min_ev);
    return agxContrast(val);
}
// AgX output transform. The result is display-encoded (no further OETF needed).
vec3 agx(vec3 val) {
    const mat3 agx_out = mat3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);
    return agx_out * agxInner(val);
}

vec3 srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    vec3 lo = 12.92 * c;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, step(c, vec3(0.0031308)));
}

void main() {
    vec3 e = texture(uTex, vUV).rgb;

    vec3 lin; // linear BT.2020, in nits
    if (uTransfer == 1) {
        // HLG: apply the reference OOTF to ~1000-nit display light.
        vec3 s = hlgInvOetf(e);
        float Ys = max(dot(s, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
        lin = s * pow(Ys, 0.2) * 1000.0; // system gamma 1.2
    } else {
        lin = pqEotf(e) * 10000.0;
    }

    // Convert the source gamut to BT.709 in linear light.
    vec3 rgb;
    if (uPrimaries == 0)      rgb = lin;                 // already BT.709
    else if (uPrimaries == 2) rgb = p3_to_709 * lin;     // Display P3 (D65)
    else                      rgb = bt2020_to_709 * lin; // BT.2020 (and default)

    // Scale nits into the operators' working range (1.0 ~= SDR diffuse white).
    rgb = max(rgb, 0.0) / 200.0 * uExposure;

    // AgX outputs display-encoded values; the others output linear and take the
    // sRGB OETF below.
    vec3 mapped;
    bool encoded = false;
    if (uTonemapper == 1)      mapped = reinhard(rgb);
    else if (uTonemapper == 2) mapped = uncharted2(rgb);
    else if (uTonemapper == 3) mapped = aces(rgb);
    else if (uTonemapper == 4) { mapped = agx(rgb); encoded = true; }
    else                       mapped = rgb;  // None (0): linearize + gamut only, hard clip

    if (!encoded)
        mapped = srgb(mapped);
    fragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
