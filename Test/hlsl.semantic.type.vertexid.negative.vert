// SV_VertexID maps to the SPIR-V VertexIndex built-in, which requires a scalar
// int or uint.  A float declaration has no valid type for the built-in.
struct VSOut {
    float4 pos : SV_Position;
};

VSOut main(float vertexId : SV_VertexID)
{
    VSOut outp;
    outp.pos = float4(vertexId, 0.0, 0.0, 1.0);
    return outp;
}
