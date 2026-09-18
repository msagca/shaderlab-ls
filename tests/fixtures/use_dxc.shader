Shader "Tests/UseDxc"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma use_dxc
            float4 vert(float4 p : POSITION) : SV_POSITION { return p; }
            float4 frag() : SV_Target { return missingInDxc; }
            ENDHLSL
        }
    }
}
