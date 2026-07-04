// Fullscreen triangle from SV_VertexID, no vertex buffer needed.
// SDL_GPU convention: NDC +Y up, texture coordinates top-left origin, so
// uv (0,0) maps to NDC (-1,+1).

struct VSOutput {
    float2 uv : TEXCOORD0;
    float4 pos : SV_Position;
};

VSOutput main(uint vid : SV_VertexID) {
    float2 p = float2((vid == 1) ? 3.0 : -1.0,
                      (vid == 2) ? 3.0 : -1.0);
    VSOutput o;
    o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    o.pos = float4(p, 0.0, 1.0);
    return o;
}
