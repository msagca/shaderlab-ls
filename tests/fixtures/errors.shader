Shader "Tests/Errors"
{
    Properties
    {
        [HDR] _Color ("Color", Color) = (1, 1, 1, 1)
        _MainTex ("Texture", 2D) = "white" {}
        _Bad ("Bad", Float2) = 1
        _WrongDefault ("Wrong", Color) = 1
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlend ("Src", Float) = 1
    }
    SubShader
    {
        Tags { "RenderType" = "Opaque" "Queue" = "Geometry+10" "PreviewType" = "Cube" }
        Cull Sideways
        Blend [_SrcBlend] OneMinusSrcAlpha
        ZTest [_Missing]
        Pass
        {
            Name "Main"
            Tags { "LightMode" = "UniversalForward" }
            ColorMask RGBX
            Offset 1
            Stencil
            {
                Ref 300
                Comp Sometimes
                Pass Replace
            }
            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 9.0
            float4 _Color;
            float4 vert(float4 p : POSITION) : SV_POSITION { return p; }
            float4 frag() : SV_Target { /* é */ return _Color * undefinedThing; }
            ENDHLSL
        }
    }
    Fallback Off
}
