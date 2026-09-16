// Header comment
Shader "Tests/Format" {
  Properties {
      [Header(A  group, of things)] [HDR]_Color("Color",Color)=(1,1,1,1)
  _MainTex ( "Texture" , 2D ) = "white"{}
        _Cutoff("Cutoff",Range (0,1))=0.5 // trailing comment


        [Enum(One,1,SrcAlpha,5)]_Blend("Blend",Float)=1
  }
  CGINCLUDE
        #include "UnityCG.cginc"
  ENDCG
  SubShader {
    Tags{"RenderType"="Opaque"   "Queue"="Geometry+1"}
    LOD   100
    ZTest Always ZWrite Off Cull [_Cull]
    Blend 1 SrcAlpha OneMinusSrcAlpha,One Zero
    Pass {

      Name "Main"
      Stencil { Ref 2 Comp Always }
      Stencil {
        Ref [_Ref] Comp equal
        Pass Replace
      }
      /* block
         comment */
      Fog {Mode Off}
      PackageRequirements { "com.unity.render-pipelines.universal":"[12.0,13.0)" }
      HLSLPROGRAM // same-line comment
        #pragma vertex vert
        #pragma fragment frag
        #define LONG_MACRO(x) \
            (x * 2)

        float4 vert(float4 p : POSITION) : SV_POSITION
        {
            return p;
        }
        float4 frag() : SV_Target { return 1; }
      ENDHLSL

    }
  }
  Fallback Off
}
