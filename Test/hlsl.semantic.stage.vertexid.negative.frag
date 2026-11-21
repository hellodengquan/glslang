// SV_VertexID maps to the SPIR-V VertexIndex built-in, which is an input of
// the vertex stage only.  Declaring it on a fragment-stage input is illegal.
float4 main(uint vertexId : SV_VertexID) : SV_Target0
{
    return float4(float(vertexId), 0.0, 0.0, 1.0);
}
