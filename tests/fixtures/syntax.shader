Shader "Tests/Syntax"
{
    SubShader
    {
        Name "NotHere"
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma banana
            float4 vert(float4 p : POSITION) : SV_POSITION { return p; }
            ENDHLSL
        }
}
