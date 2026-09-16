Shader "Tests/Valid"
{
    Properties
    {
        [MainColor] _BaseColor ("Base Color", Color) = (1, 1, 1, 1)
        [MainTexture] _BaseMap ("Base Map", 2D) = "white" {}
        _Cutoff ("Alpha Cutoff", Range(0.0, 1.0)) = 0.5
        [Toggle(_EMISSION)] _Emission ("Emission", Float) = 0
        [Enum(UnityEngine.Rendering.CullMode)] _Cull ("Cull", Float) = 2
        _Count ("Count", Integer) = 3
        _Extra ("Extra", Vector, 3) = (0, 0, 0, 0)
    }

    HLSLINCLUDE
    #define SCALE() 2.0
    float4 _BaseColor;
    ENDHLSL

    SubShader
    {
        Tags { "RenderType" = "Opaque" "RenderPipeline" = "UniversalPipeline" "Queue" = "Transparent-1" }
        LOD 100
        Cull [_Cull]
        Blend SrcAlpha OneMinusSrcAlpha, One Zero
        BlendOp Add
        ZWrite Off
        ZTest LEqual
        ColorMask RGB 1
        Offset -1, -1

        Pass
        {
            Name "ForwardLit"
            Tags { "LightMode" = "UniversalForward" }
            Stencil
            {
                Ref 2
                Comp equal
                Pass keep
                ZFail decrWrap
            }

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 4.5
            #pragma multi_compile _ _EMISSION
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma only_renderers d3d11 vulkan

            struct Attributes
            {
                float4 positionOS : POSITION;
            };

            Texture2D _BaseMap;
            SamplerState sampler_BaseMap;
            float _Cutoff;

            float4 vert(Attributes input) : SV_POSITION
            {
                return input.positionOS * SCALE();
            }

            float4 frag() : SV_Target
            {
            #ifdef _EMISSION
                return _BaseColor * 2.0;
            #else
                return _BaseColor;
            #endif
            }
            ENDHLSL
        }
    }
    CustomEditor "ExampleShaderGUI"
    Fallback "Hidden/InternalErrorShader"
}
