Shader "Tests/UnityCompat"
{
    // Forms missing from the current reference but used by Unity's own shaders.
    Properties
    {
        _MainTex ("Texture", Any) = "" {}
        _Color ("Color", Color) = (1, 1, 1, 1)
    }
    SubShader
    {
        Tags { "RenderType" = "Opaque" "UniversalMaterialType" = "Lit" }
        Lighting Off Cull Back ZWrite Off Fog { Mode Off }
        Pass
        {
            Name "Meta"
            Tags { "LightMode" = "Meta" "Queue" = "Transparent" }
            LOD 100
            ZTest Off
            ZClip Off
            Conservative On
            Cull [_CullMode]
            Color [_Color]
            SetTexture [_MainTex] {
                combine primary, texture * primary
            }

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma require randomwrite
            #pragma only_renderers d3d11 xboxone switch2 gles

            #if !defined(UNITY_PASS_META)
            #error "UNITY_PASS_META should be defined for LightMode Meta"
            #endif

            RWTexture2D<float4> _Output : register(u4);

            float4 vert(float4 p : POSITION) : SV_POSITION { return p; }
            float4 frag() : SV_Target { _Output[uint2(0, 0)] = 1; return 1; }
            ENDHLSL
        }
        Pass
        {
            Lighting Off Cull Sideways
            HLSLPROGRAM
            #pragma raytracing surface_shader
            ENDHLSL
        }
    }
    Dependency "BaseMapShader" = "Hidden/BaseMap"
    Fallback Off
}
