Shader "Tests/Completion"
{
    Properties
    {
        _Tint ("Tint", Color) = (1, 1, 1, 1)
        _Mode ("Mode", Float) = 0
    }
    SubShader
    {
        Pass
        {

            Cull
            ZTest [
            HLSLPROGRAM
            #pragma
            #pragma vertex
            float4 vert(float4 p : POSITION) : SV_POSITION { return p; }
            float4 frag() : SV_Target { return _Tint; }
            ENDHLSL
        }
    }
}
