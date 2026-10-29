// skate1 - native D3D layer: Resolve (EDRAM render target -> texture).
// Full-screen triangle; the pixel shader loads the source surface at the
// destination pixel + offset and converts it to what the game's texture
// fetches expect:
//   mode 0: color * scale (16_16 EDRAM targets hold -32..32, read back as
//           normalized 16_16 textures: scale 1/32)
//   mode 1: depth packed as D24S8 (depth << 8) in an 8_8_8_8 texture, as a
//           depth resolve read through an 8_8_8_8 fetch sees it.

struct PushConstants
{
    uint SourceIndex;
    uint Mode;
    float Scale;
    int OffsetX;
    int OffsetY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_Push;
Texture2D<float4> g_Textures[] : register(t0, space0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy) + int2(g_Push.OffsetX, g_Push.OffsetY);
    float4 value = g_Textures[g_Push.SourceIndex].Load(int3(p, 0));
    if (g_Push.Mode == 1)
    {
        uint packed = uint(saturate(value.x) * 16777215.0 + 0.5) << 8;
        return float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    }
    return value * g_Push.Scale;
}
