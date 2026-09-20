#include "shaderlab/reference.h"
#include "common/util.h"
namespace sls::ref {
namespace {
  constexpr Entry kShaderBlock[] = {
    {"Properties", "Properties { <Material property declaration> ... }",
      "Saves the given properties as part of the material asset, and uses the values stored in the material asset "
      "during rendering. A `Properties` block can contain any number of material property declarations.",
      "Properties\n{\n\t$0\n}"},
    {"SubShader", "SubShader { <optional: LOD> <optional: tags> <optional: commands> <One or more Pass definitions> }", "Defines a SubShader. You can define as many Passes as you like within a SubShader.", "SubShader\n{\n\t$0\n}"},
    {"CustomEditor", "CustomEditor \"[custom editor class name]\"",
      "Unity uses the custom editor defined in the named class, unless this is overridden by a "
      "`CustomEditorForRenderPipeline` block.",
      "CustomEditor \"$1\""},
    {"CustomEditorForRenderPipeline",
      "CustomEditorForRenderPipeline \"[custom editor class name]\" \"[render pipeline asset class name]\"",
      "When the active render pipeline asset is the named type, Unity uses the custom editor defined in the named class.",
      "CustomEditorForRenderPipeline \"$1\" \"$2\""},
    {"Fallback", "Fallback \"<name>\" | Fallback Off",
      "`Fallback \"<name>\"`: if no compatible SubShaders are found, use the named Shader object.\n\n`Fallback Off`: do "
      "not use a fallback Shader object in place of this one. If no compatible SubShaders are found, display the error "
      "shader."},
    {"HLSLINCLUDE", "HLSLINCLUDE [code that you want to share] ENDHLSL",
      "Creates a shader include block. Unity includes this code in all shader programs that are defined in "
      "`HLSLPROGRAM` blocks, anywhere in this source file.",
      "HLSLINCLUDE\n$0\nENDHLSL"},
    {"CGINCLUDE", "CGINCLUDE [code that you want to share] ENDCG",
      "Creates a shader include block. Unity includes this code in all shader programs that are defined in `CGPROGRAM` "
      "blocks, anywhere in this source file. Compatible only with the Built-In Render Pipeline.",
      "CGINCLUDE\n$0\nENDCG"},
  };
  constexpr Entry kSubShaderBlock[] = {
    {"PackageRequirements", "PackageRequirements { [requirement definition] }",
      "Defines the package requirements for the Pass or SubShader. If you provide a `PackageRequirements` block, it "
      "must come before all other declarations inside the SubShader or Pass.",
      "PackageRequirements\n{\n\t\"$1\"\n}"},
    {"LOD", "LOD [value]", "Assigns the given LOD value to the SubShader.", "LOD ${1:100}"},
    {"Tags", "Tags { \"[name1]\" = \"[value1]\" \"[name2]\" = \"[value2]\" }", "Applies the given tags to the SubShader. You can define as many tags as you like.", "Tags { \"$1\" = \"$2\" }"},
    {"Pass", "Pass { <optional: name> <optional: tags> <optional: commands> <optional: shader code> }", "Defines a Pass.", "Pass\n{\n\t$0\n}"},
    {"UsePass", "UsePass \"Shader object name/PASS NAME IN UPPERCASE\"",
      "Inserts the named Pass from the named Shader object. If Unity does not find a matching Pass, it shows the error "
      "shader.",
      "UsePass \"$1\""},
    {"GrabPass", "GrabPass { } | GrabPass { \"ExampleTextureName\" }",
      "Grabs the frame buffer contents into a texture that you can use in subsequent Passes. Built-in Render Pipeline "
      "only. This command can significantly increase both CPU and GPU frame times.",
      "GrabPass { \"$1\" }"},
    {"HLSLPROGRAM", "HLSLPROGRAM [source code for shader programs] ENDHLSL",
      "Creates a shader program block. Unity adds the HLSL shader program to the pass that includes this shader program "
      "block.",
      "HLSLPROGRAM\n$0\nENDHLSL"},
    {"HLSLINCLUDE", "HLSLINCLUDE [code that you want to share] ENDHLSL",
      "Creates a shader include block. Unity includes this code in all shader programs that are defined in "
      "`HLSLPROGRAM` blocks, anywhere in this source file.",
      "HLSLINCLUDE\n$0\nENDHLSL"},
    {"CGPROGRAM", "CGPROGRAM [source code for shader programs] ENDCG",
      "Creates a shader program block. Compatible only with the Built-In Render Pipeline. If you use `CGPROGRAM`, Unity "
      "includes several of Unity's built-in shader include files by default.",
      "CGPROGRAM\n$0\nENDCG"},
    {"CGINCLUDE", "CGINCLUDE [code that you want to share] ENDCG",
      "Creates a shader include block. Unity includes this code in all shader programs that are defined in `CGPROGRAM` "
      "blocks, anywhere in this source file.",
      "CGINCLUDE\n$0\nENDCG"},
  };
  constexpr Entry kPassBlock[] = {
    {"PackageRequirements", "PackageRequirements { [requirement definition] }",
      "Defines the package requirements for the Pass or SubShader. If you provide a `PackageRequirements` block, it "
      "must come before all other declarations inside the SubShader or Pass.",
      "PackageRequirements\n{\n\t\"$1\"\n}"},
    {"Name", "Name \"<name>\"", "Sets the name of the Pass.", "Name \"$1\""},
    {"Tags", "Tags { \"<name1>\" = \"<value1>\" \"<name2>\" = \"<value2>\" }", "Applies the given tags to the Pass. You can define as many tags as you like.", "Tags { \"$1\" = \"$2\" }"},
    {"HLSLPROGRAM", "HLSLPROGRAM [source code for shader programs] ENDHLSL",
      "Creates a shader program block. Unity adds the HLSL shader program to the pass that includes this shader program "
      "block.",
      "HLSLPROGRAM\n$0\nENDHLSL"},
    {"HLSLINCLUDE", "HLSLINCLUDE [code that you want to share] ENDHLSL",
      "Creates a shader include block. Unity includes this code in all shader programs that are defined in "
      "`HLSLPROGRAM` blocks, anywhere in this source file.",
      "HLSLINCLUDE\n$0\nENDHLSL"},
    {"CGPROGRAM", "CGPROGRAM [source code for shader programs] ENDCG",
      "Creates a shader program block. Compatible only with the Built-In Render Pipeline. If you use `CGPROGRAM`, Unity "
      "includes several of Unity's built-in shader include files by default.",
      "CGPROGRAM\n$0\nENDCG"},
    {"CGINCLUDE", "CGINCLUDE [code that you want to share] ENDCG",
      "Creates a shader include block. Unity includes this code in all shader programs that are defined in `CGPROGRAM` "
      "blocks, anywhere in this source file.",
      "CGINCLUDE\n$0\nENDCG"},
  };
  constexpr Entry kCommands[] = {
    {"AlphaToMask", "AlphaToMask <state>", "Enables or disables alpha-to-coverage mode on the GPU."},
    {"Blend",
      "Blend <state>\nBlend <render target> <state>\nBlend <source factor> <destination factor>\n"
      "Blend <render target> <source factor> <destination factor>\n"
      "Blend <source factor RGB> <destination factor RGB>, <source factor alpha> <destination factor alpha>\n"
      "Blend <render target> <source factor RGB> <destination factor RGB>, <source factor alpha> <destination factor alpha>",
      "Determines how the GPU combines the output of the fragment shader with the render target. Enabling blending "
      "disables some optimizations on the GPU (mostly hidden surface removal/Early-Z)."},
    {"BlendOp", "BlendOp <operation>",
      "Specifies the blending operation used by the Blend command. For this command to have any effect, there must also "
      "be a Blend command in the same Pass block or SubShader block."},
    {"ColorMask", "ColorMask <channels>\nColorMask <channels> <render target>", "Sets the color channel writing mask, which prevents the GPU from writing to channels in the render target."},
    {"Conservative", "Conservative <enabled>", "Enables or disables conservative rasterization. Requires DX 11.3+, or GL_NV_conservative_raster."},
    {"Cull", "Cull <state>", "Sets which polygons the GPU should cull, based on the direction that they are facing relative to the camera."},
    {"Offset", "Offset <factor>, <units>",
      "Sets the depth bias on the GPU. `factor` scales the maximum Z slope; `units` scales the minimum resolvable depth "
      "buffer value. A negative value draws the polygon closer to the camera."},
    {"Stencil",
      "Stencil { Ref <ref> ReadMask <readMask> WriteMask <writeMask> Comp <comparisonOperation> Pass <passOperation> "
      "Fail <failOperation> ZFail <zFailOperation> ... }",
      "Configures settings relating to the stencil buffer on the GPU. All parameters are optional.",
      "Stencil\n{\n\tRef ${1:1}\n\tComp ${2:Always}\n\tPass ${3:Replace}\n}"},
    {"ZClip", "ZClip [enabled]",
      "Sets the GPU's depth clip mode, which determines how the GPU handles fragments that are outside of the near and "
      "far planes."},
    {"ZTest", "ZTest [operation]", "Sets the conditions under which geometry passes or fails depth testing."},
    {"ZWrite", "ZWrite [state]",
      "Sets whether the depth buffer contents are updated during rendering. Normally, ZWrite is enabled for opaque "
      "objects and disabled for semi-transparent ones."},
  };
  constexpr Entry kStencilFields[] = {
    {"Ref", "Ref <ref>", "An integer, 0 through 255 (default 0). The reference value the GPU compares the stencil buffer against."},
    {"ReadMask", "ReadMask <readMask>", "An integer, 0 through 255 (default 255). Mask used when performing the stencil test."},
    {"WriteMask", "WriteMask <writeMask>", "An integer, 0 through 255 (default 255). Mask used when writing to the stencil buffer."},
    {"Comp", "Comp <comparisonOperation>", "The stencil test operation for all pixels, regardless of facing. Default is Always."},
    {"Pass", "Pass <passOperation>", "Operation when a pixel passes both the stencil test and the depth test. Default is Keep."},
    {"Fail", "Fail <failOperation>", "Operation when a pixel fails the stencil test. Default is Keep."},
    {"ZFail", "ZFail <zFailOperation>", "Operation when a pixel passes the stencil test but fails the depth test. Default is Keep."},
    {"CompBack", "CompBack <comparisonOperationBack>", "The stencil test operation for back-facing pixels only."},
    {"PassBack", "PassBack <passOperationBack>", "Pass operation for back-facing pixels only."},
    {"FailBack", "FailBack <failOperationBack>", "Fail operation for back-facing pixels only."},
    {"ZFailBack", "ZFailBack <zFailOperationBack>", "ZFail operation for back-facing pixels only."},
    {"CompFront", "CompFront <comparisonOperationFront>", "The stencil test operation for front-facing pixels only."},
    {"PassFront", "PassFront <passOperationFront>", "Pass operation for front-facing pixels only."},
    {"FailFront", "FailFront <failOperationFront>", "Fail operation for front-facing pixels only."},
    {"ZFailFront", "ZFailFront <zFailOperationFront>", "ZFail operation for front-facing pixels only."},
  };
  constexpr Entry kBlendFactors[] = {
    {"One", "factor", "The value of this input is one."},
    {"Zero", "factor", "The value of this input is zero."},
    {"SrcColor", "factor", "The GPU multiplies the value of this input by the source color value."},
    {"SrcAlpha", "factor", "The GPU multiplies the value of this input by the source alpha value."},
    {"SrcAlphaSaturate", "factor", "The GPU multiplies the value of this input by the minimum value of `source alpha` and `(1 - destination alpha)`."},
    {"DstColor", "factor", "The GPU multiplies the value of this input by the frame buffer source color value."},
    {"DstAlpha", "factor", "The GPU multiplies the value of this input by the frame buffer source alpha value."},
    {"OneMinusSrcColor", "factor", "The GPU multiplies the value of this input by (1 - source color)."},
    {"OneMinusSrcAlpha", "factor", "The GPU multiplies the value of this input by (1 - source alpha)."},
    {"OneMinusDstColor", "factor", "The GPU multiplies the value of this input by (1 - destination color)."},
    {"OneMinusDstAlpha", "factor", "The GPU multiplies the value of this input by (1 - destination alpha)."},
  };
  constexpr Entry kBlendOps[] = {
    {"Add", "operation", "Add source and destination together."},
    {"Sub", "operation", "Subtract destination from source."},
    {"RevSub", "operation", "Subtract source from destination."},
    {"Min", "operation", "Use the smaller of source and destination."},
    {"Max", "operation", "Use the larger of source and destination."},
    {"LogicalClear", "logical operation", "Clear (0). Requires DX 11.1+ or Vulkan."},
    {"LogicalSet", "logical operation", "Set (1). Requires DX 11.1+ or Vulkan."},
    {"LogicalCopy", "logical operation", "Copy (s). Requires DX 11.1+ or Vulkan."},
    {"LogicalCopyInverted", "logical operation", "Copy inverted (!s). Requires DX 11.1+ or Vulkan."},
    {"LogicalNoop", "logical operation", "Noop (d). Requires DX 11.1+ or Vulkan."},
    {"LogicalInvert", "logical operation", "Invert (!d). Requires DX 11.1+ or Vulkan."},
    {"LogicalAnd", "logical operation", "And (s & d). Requires DX 11.1+ or Vulkan."},
    {"LogicalNand", "logical operation", "Nand !(s & d). Requires DX 11.1+ or Vulkan."},
    {"LogicalOr", "logical operation", "Or (s | d). Requires DX 11.1+ or Vulkan."},
    {"LogicalNor", "logical operation", "Nor !(s | d). Requires DX 11.1+ or Vulkan."},
    {"LogicalXor", "logical operation", "Xor (s ^ d). Requires DX 11.1+ or Vulkan."},
    {"LogicalEquiv", "logical operation", "Equivalence !(s ^ d). Requires DX 11.1+ or Vulkan."},
    {"LogicalAndReverse", "logical operation", "Reverse And (s & !d). Requires DX 11.1+ or Vulkan."},
    {"LogicalAndInverted", "logical operation", "Inverted And (!s & d). Requires DX 11.1+ or Vulkan."},
    {"LogicalOrReverse", "logical operation", "Reverse Or (s | !d). Requires DX 11.1+ or Vulkan."},
    {"LogicalOrInverted", "logical operation", "Inverted Or (!s | d). Requires DX 11.1+ or Vulkan."},
    {"Multiply", "advanced OpenGL blending operation", "Multiply. Requires GLES3.1 AEP+, GL_KHR_blend_equation_advanced, or GL_NV_blend_equation_advanced."},
    {"Screen", "advanced OpenGL blending operation", "Screen."},
    {"Overlay", "advanced OpenGL blending operation", "Overlay."},
    {"Darken", "advanced OpenGL blending operation", "Darken."},
    {"Lighten", "advanced OpenGL blending operation", "Lighten."},
    {"ColorDodge", "advanced OpenGL blending operation", "ColorDodge."},
    {"ColorBurn", "advanced OpenGL blending operation", "ColorBurn."},
    {"HardLight", "advanced OpenGL blending operation", "HardLight."},
    {"SoftLight", "advanced OpenGL blending operation", "SoftLight."},
    {"Difference", "advanced OpenGL blending operation", "Difference."},
    {"Exclusion", "advanced OpenGL blending operation", "Exclusion."},
    {"HSLHue", "advanced OpenGL blending operation", "HSLHue."},
    {"HSLSaturation", "advanced OpenGL blending operation", "HSLSaturation."},
    {"HSLColor", "advanced OpenGL blending operation", "HSLColor."},
    {"HSLLuminosity", "advanced OpenGL blending operation", "HSLLuminosity."},
  };
  constexpr Entry kCull[] = {
    {"Back", "state", "Cull polygons that face away from the camera. This is called back-face culling. This is the default value."},
    {"Front", "state", "Cull polygons that face towards the camera. This is called front-face culling."},
    {"Off", "state", "Do not cull polygons based on the direction that they face."},
  };
  constexpr Entry kZTest[] = {
    {"Disabled", "operation", "Disable the depth test."},
    {"Never", "operation", "Draw no geometry, regardless of distance."},
    {"Less", "operation", "Draw geometry that is in front of existing geometry."},
    {"Equal", "operation", "Draw geometry that is at the same distance as existing geometry."},
    {"LEqual", "operation", "Draw geometry that is in front of or at the same distance as existing geometry. This is the default value."},
    {"Greater", "operation", "Draw geometry that is behind existing geometry."},
    {"NotEqual", "operation", "Draw geometry that is not at the same distance as existing geometry."},
    {"GEqual", "operation", "Draw geometry that is behind or at the same distance as existing geometry."},
    {"Always", "operation", "No depth testing occurs. Draw all geometry, regardless of distance."},
  };
  constexpr Entry kStencilCompare[] = {
    {"Never", "comparison (1)", "Never render pixels."},
    {"Less", "comparison (2)", "Render pixels when their reference value is less than the current value in the stencil buffer."},
    {"Equal", "comparison (3)", "Render pixels when their reference value is equal to the current value in the stencil buffer."},
    {"LEqual", "comparison (4)", "Render pixels when their reference value is less than or equal to the current value in the stencil buffer."},
    {"Greater", "comparison (5)", "Render pixels when their reference value is greater than the current value in the stencil buffer."},
    {"NotEqual", "comparison (6)", "Render pixels when their reference value differs from the current value in the stencil buffer."},
    {"GEqual", "comparison (7)", "Render pixels when their reference value is greater than or equal to the current value in the stencil buffer."},
    {"Always", "comparison (8)", "Always render pixels."},
  };
  constexpr Entry kStencilOps[] = {
    {"Keep", "stencil operation (0)", "Keep the current contents of the stencil buffer."},
    {"Zero", "stencil operation (1)", "Write 0 into the stencil buffer."},
    {"Replace", "stencil operation (2)", "Write the reference value into the buffer."},
    {"IncrSat", "stencil operation (3)", "Increment the current value in the buffer. If the value is 255 already, it stays at 255."},
    {"DecrSat", "stencil operation (4)", "Decrement the current value in the buffer. If the value is 0 already, it stays at 0."},
    {"Invert", "stencil operation (5)", "Negate all the bits of the current value in the buffer."},
    {"IncrWrap", "stencil operation (6)", "Increment the current value in the buffer. If the value is 255 already, it becomes 0."},
    {"DecrWrap", "stencil operation (7)", "Decrement the current value in the buffer. If the value is 0 already, it becomes 255."},
  };
  constexpr Entry kOnOff[] = {
    {"On", "state", "Enables the feature."},
    {"Off", "state", "Disables the feature."},
  };
  constexpr Entry kTrueFalse[] = {
    {"True", "enabled", "Enables the feature."},
    {"False", "enabled", "Disables the feature."},
  };
  constexpr Entry kColorMask[] = {
    {"RGBA", "channels", "Enables color writes to the red, green, blue and alpha channels."},
    {"RGB", "channels", "Enables color writes to the red, green and blue channels."},
    {"R", "channels", "Enables color writes to the red channel."},
    {"G", "channels", "Enables color writes to the green channel."},
    {"B", "channels", "Enables color writes to the blue channel."},
    {"A", "channels", "Enables color writes to the alpha channel."},
    {"0", "channels", "Disables color writes to the R, G, B, and A channels."},
  };
  constexpr Entry kPropertyTypes[] = {
    {"Integer", "_ExampleName (\"Integer display name\", Integer) = 1", "Backed by a real integer (unlike the legacy `Int` type, which is backed by a float)."},
    {"Int", "_ExampleName (\"Int display name\", Int) = 1", "Legacy type backed by a float. Supported for backwards compatibility only; use `Integer` instead."},
    {"Float", "_ExampleName (\"Float display name\", Float) = 0.5", "A float value."},
    {"Range", "_ExampleName (\"Float with range\", Range(0.0, 1.0)) = 0.5", "A float with a slider. The minimum and maximum values for the range slider are inclusive.", "Range(${1:0.0}, ${2:1.0})"},
    {"2D", "_ExampleName (\"Texture2D display name\", 2D) = \"\" {}",
      "Texture2D. Default value strings for built-in textures: \"white\", \"black\", \"gray\", \"bump\", \"red\". An "
      "empty or invalid string defaults to \"gray\"."},
    {"2DArray", "_ExampleName (\"Texture2DArray display name\", 2DArray) = \"\" {}", "Texture2DArray."},
    {"3D", "_ExampleName (\"Texture3D\", 3D) = \"\" {}", "Texture3D. The default value is a \"gray\" texture."},
    {"Cube", "_ExampleName (\"Cubemap\", Cube) = \"\" {}", "Cubemap. The default value is a \"gray\" texture."},
    {"CubeArray", "_ExampleName (\"CubemapArray\", CubeArray) = \"\" {}", "CubemapArray."},
    {"Any", "_ExampleName (\"Texture\", Any) = \"\" {}", "A texture of any dimension. Not in the current reference; used by Unity's built-in blit shaders."},
    {"Color", "_ExampleName(\"Example color\", Color) = (.25, .5, .5, 1)", "Maps to a float4 in your shader code. The Material Inspector displays a color picker."},
    {"Vector", "_ExampleName (\"Example vector\", Vector) = (.25, .5, .5, 1)",
      "Maps to a float4 in your shader code. The Material Inspector displays four float fields; add `2`, `3`, or `4` "
      "after `Vector` to display fewer. You must still define all four components."},
  };
  constexpr Entry kPropertyAttributes[] = {
    {"Gamma", "[Gamma]", "Indicates that a float or vector property uses sRGB values."},
    {"HDR", "[HDR]", "Indicates that a texture or color property uses high dynamic range (HDR) values."},
    {"HideInInspector", "[HideInInspector]", "Tells the Unity Editor to hide this property in the Inspector."},
    {"MainTexture", "[MainTexture]", "Sets the main texture for a Material, which you can access using Material.mainTexture."},
    {"MainColor", "[MainColor]", "Sets the main color for a Material, which you can access using Material.color."},
    {"NoScaleOffset", "[NoScaleOffset]", "Tells the Unity Editor to hide tiling and offset fields for this texture property."},
    {"Normal", "[Normal]", "Indicates that a texture property expects a normal map."},
    {"PerRendererData", "[PerRendererData]", "Indicates that a texture property will be coming from per-renderer data in the form of a MaterialPropertyBlock."},
    // Built-in MaterialPropertyDrawers (ScriptReference/MaterialPropertyDrawer).
    {"Toggle", "[Toggle] / [Toggle(KEYWORD)]", "MaterialPropertyDrawer: shows a float as a toggle and sets a shader keyword.", "Toggle(${1:KEYWORD})"},
    {"ToggleOff", "[ToggleOff] / [ToggleOff(KEYWORD)]", "MaterialPropertyDrawer: toggle that enables the keyword when off.", "ToggleOff(${1:KEYWORD})"},
    {"KeywordEnum", "[KeywordEnum(None, Add, Multiply)]", "MaterialPropertyDrawer: popup that sets one of several keywords.", "KeywordEnum(${1:A}, ${2:B})"},
    {"Enum", "[Enum(UnityEngine.Rendering.BlendMode)] / [Enum(One,1,SrcAlpha,5)]", "MaterialPropertyDrawer: popup of enum values.", "Enum(${1:UnityEngine.Rendering.BlendMode})"},
    {"PowerSlider", "[PowerSlider(3.0)]", "MaterialPropertyDrawer: slider with a non-linear response for Range properties.", "PowerSlider(${1:3.0})"},
    {"IntRange", "[IntRange]", "MaterialPropertyDrawer: integer slider for Range properties."},
    {"Space", "[Space] / [Space(50)]", "MaterialPropertyDrawer decorator: vertical space before the property."},
    {"Header", "[Header(A group of things)]", "MaterialPropertyDrawer decorator: header text before the property.", "Header(${1:Header})"},
  };
  constexpr Entry kSubShaderTags[] = {
    {"RenderPipeline", "\"RenderPipeline\" = \"[name]\"", "Tells Unity whether this SubShader is compatible with URP or HDRP."},
    {"Queue", "\"Queue\" = \"[queue name]\" | \"[queue name] + [offset]\"", "Use the named render queue, or an unnamed queue at a given offset from the named queue."},
    {"RenderType", "\"RenderType\" = \"[renderType]\"", "Set the RenderType value for this SubShader. There are no set values for this parameter."},
    {"ForceNoShadowCasting", "\"ForceNoShadowCasting\" = \"[state]\"", "Whether to prevent shadow casting (and sometimes receiving) for all geometry that uses this SubShader."},
    {"IgnoreProjector", "\"IgnoreProjector\" = \"[state]\"", "Whether Unity ignores Projectors when rendering this geometry. Built-In Render Pipeline only."},
    {"PreviewType", "\"PreviewType\" = \"[shape]\"", "Which shape the Unity Editor uses to display a preview of a material that uses this SubShader."},
  };
  constexpr Entry kPassTags[] = {
    {"LightMode", "\"LightMode\" = \"[value]\"", "Determines when Unity executes the pass. Valid values depend on the render pipeline."},
    {"PassFlags", "\"PassFlags\" = \"OnlyDirectional\"", "Built-in Render Pipeline: specifies what data Unity provides to the Pass."},
    {"RequireOptions", "\"RequireOptions\" = \"SoftVegetation\"", "Built-in Render Pipeline: enables or disables a Pass based on project settings."},
    {"UniversalMaterialType", "\"UniversalMaterialType\" = \"[value]\"", "URP: used in the Deferred Rendering Path. Defaults to Lit."},
  };
  constexpr Entry kRenderPipelineValues[] = {
    {"UniversalPipeline", "URP", "This SubShader is compatible with URP only."},
    {"HDRenderPipeline", "HDRP", "This SubShader is compatible with HDRP only."},
  };
  constexpr Entry kQueueValues[] = {
    {"Background", "queue", "Specifies the Background render queue."},
    {"Geometry", "queue", "Specifies the Geometry render queue."},
    {"AlphaTest", "queue", "Specifies the AlphaTest render queue."},
    {"Transparent", "queue", "Specifies the Transparent render queue."},
    {"Overlay", "queue", "Specifies the Overlay render queue."},
  };
  constexpr Entry kShadowValues[] = {
    {"True", "state", "Enabled."},
    {"False", "state", "Disabled. This is the default value."},
  };
  constexpr Entry kPreviewTypeValues[] = {
    {"Sphere", "shape", "Display the material on a sphere. This is the default value."},
    {"Plane", "shape", "Display the material on a plane."},
    {"Skybox", "shape", "Display the material on a skybox."},
  };
  constexpr Entry kRenderTypeValues[] = {
    {"Opaque", "renderType", "Common RenderType value."},
    {"Transparent", "renderType", "Common RenderType value."},
    {"TransparentCutout", "renderType", "Common RenderType value."},
    {"Background", "renderType", "Common RenderType value."},
    {"Overlay", "renderType", "Common RenderType value."},
  };
  constexpr Entry kLightModeValues[] = {
    // Universal Render Pipeline
    {"UniversalForward", "URP", "Renders object geometry and evaluates all light contributions. Used in the Forward Rendering Path."},
    {"UniversalGBuffer", "URP", "Renders object geometry without evaluating any light contribution. Used in the Deferred Rendering Path."},
    {"UniversalForwardOnly", "URP", "Like UniversalForward, but URP can use the Pass for both the Forward and the Deferred Rendering Paths."},
    {"DepthNormalsOnly", "URP", "Use in combination with UniversalForwardOnly in the Deferred Rendering Path, for the depth and normal prepass."},
    {"Universal2D", "URP", "Renders objects and evaluates 2D light contributions. Used in the 2D Renderer."},
    {"DepthOnly", "URP", "Renders only depth information from the perspective of a Camera into a depth texture."},
    {"SRPDefaultUnlit", "URP", "Draws an extra Pass when rendering objects. The default when a Pass has no LightMode tag."},
    {"MotionVectors", "URP / Built-in", "Adds motion vector support to your shader."},
    {"ShadowCaster", "URP / Built-in", "Renders object depth into the shadow map or a depth texture."},
    {"Meta", "URP / Built-in", "Used only for lightmap baking; stripped from Player builds."},
    // Built-In Render Pipeline
    {"Always", "Built-in", "Always rendered; does not apply any lighting. This is the default value."},
    {"ForwardBase", "Built-in", "Used in Forward rendering; applies ambient, main directional light, vertex/SH lights and lightmaps."},
    {"ForwardAdd", "Built-in", "Used in Forward rendering; applies additive per-pixel lights, one Pass per light."},
    {"Deferred", "Built-in", "Used in Deferred Shading; renders G-buffer."},
    {"Vertex", "Built-in", "Legacy Vertex Lit rendering when the object is not lightmapped."},
    {"VertexLMRGBM", "Built-in", "Legacy Vertex Lit rendering when the object is lightmapped, RGBM encoded lightmaps."},
    {"VertexLM", "Built-in", "Legacy Vertex Lit rendering when the object is lightmapped, double-LDR encoded lightmaps."},
  };
  constexpr Entry kPassFlagsValues[] = {
    {"OnlyDirectional", "Built-in", "Unity provides only the main directional light and ambient/light probe data to this Pass."},
  };
  constexpr Entry kRequireOptionsValues[] = {
    {"SoftVegetation", "Built-in", "Render this Pass only if QualitySettings-softVegetation is enabled."},
  };
  constexpr Entry kUniversalMaterialTypeValues[] = {
    {"Lit", "URP", "The shader type is Lit (PBR specular model). Default."},
    {"SimpleLit", "URP", "The shader type is SimpleLit (Blinn-Phong specular model)."},
  };
  constexpr Entry kPragmas[] = {
    {"vertex", "#pragma vertex <name>", "Compile the function with the given name as the vertex shader. Required in regular graphics shaders.", "vertex ${1:vert}"},
    {"fragment", "#pragma fragment <name>", "Compile the function with the given name as the fragment shader. Required in regular graphics shaders.", "fragment ${1:frag}"},
    {"geometry", "#pragma geometry <name>", "Compile the function with the given name as the geometry shader. Automatically turns on `#pragma require geometry`."},
    {"hull", "#pragma hull <name>", "Compile the function with the given name as the DirectX 11 hull shader. Automatically adds `#pragma require tessellation`."},
    {"domain", "#pragma domain <name>", "Compile the function with the given name as the DirectX 11 domain shader. Automatically adds `#pragma require tessellation`."},
    {"kernel", "#pragma kernel <name>", "Compute shaders: compile the function with the given name as a compute kernel."},
    {"multi_compile", "#pragma multi_compile <keywords>", "Declares a collection of keywords. The compiler includes all of the keywords in the build. Suffixes such as `_local` set additional options."},
    {"shader_feature", "#pragma shader_feature <keywords>", "Declares a collection of keywords. The compiler excludes unused keywords from the build. Suffixes such as `_local` set additional options."},
    {"dynamic_branch", "#pragma dynamic_branch <keywords>", "Declares keywords that keep branching code in one compiled shader program."},
    {"hardware_tier_variants", "#pragma hardware_tier_variants <values>", "Built-in Render Pipeline only: add keywords for graphics tiers when compiling for a given graphics API."},
    {"skip_variants", "#pragma skip_variants <list of keywords>", "Strip specified keywords."},
    {"target", "#pragma target <value>", "The minimum shader model that this shader program is compatible with."},
    {"require", "#pragma require <value>", "The minimum GPU features that this shader is compatible with. Multiple values are separated by a space."},
    {"only_renderers", "#pragma only_renderers <value>", "Compile this shader program only for given graphics APIs (space-delimited)."},
    {"exclude_renderers", "#pragma exclude_renderers <value>", "Do not compile this shader program for given graphics APIs (space-delimited)."},
    {"disable_fastmath", "#pragma disable_fastmath", "Enable precise IEEE 754 rules involving NaN handling. This currently only affects the Metal platform."},
    {"editor_sync_compilation", "#pragma editor_sync_compilation", "Force synchronous compilation. This affects the Unity Editor only."},
    {"enable_cbuffer", "#pragma enable_cbuffer", "Emit `cbuffer(name)` when using `CBUFFER_START(name)` and `CBUFFER_END` macros from HLSLSupport even if the current platform does not support constant buffers."},
    {"enable_debug_symbols", "#pragma enable_debug_symbols", "Generates shader debug symbols for supported graphics APIs, and disables optimizations for all graphics APIs."},
    {"hlslcc_bytecode_disassembly", "#pragma hlslcc_bytecode_disassembly", "Embed disassembled HLSLcc bytecode into a translated shader."},
    {"instancing_options", "#pragma instancing_options <options>", "Enable GPU instancing in this shader, with given options."},
    {"never_use_dxc", "#pragma never_use_dxc", "Compile the shader into DXBC using the Microsoft FXC compiler, overriding the build profile setting."},
    {"use_dxc", "#pragma use_dxc", "Compile the shader into DXIL using the Microsoft DXC compiler, overriding the build profile setting."},
    {"once", "#pragma once", "Include the file only once in a shader program. Requires the Caching Shader Preprocessor."},
    {"rendertarget_format_hint", "#pragma rendertarget_format_hint MRT<MrtID> <list of graphics formats>", "Hint to platform-specific shader compilers about which GraphicsFormat may be used for each render target."},
    {"skip_optimizations", "#pragma skip_optimizations <value>", "Forces optimizations off for given graphics APIs."},
    {"surface", "#pragma surface <surface function> <lighting model> <optional parameters>", "Built-in Render Pipeline Surface Shaders: compile the function with the given name as the surface shader."},
    {"multi_compile_fog", "#pragma multi_compile_fog", "Adds FOG_LINEAR, FOG_EXP, FOG_EXP2 keywords (plus a variant with all keywords off)."},
    {"multi_compile_fwdadd", "#pragma multi_compile_fwdadd", "Adds POINT DIRECTIONAL SPOT POINT_COOKIE DIRECTIONAL_COOKIE keywords for PassType.ForwardAdd."},
    {"multi_compile_fwdadd_fullshadows", "#pragma multi_compile_fwdadd_fullshadows", "Like multi_compile_fwdadd, plus shadow keywords, so lights can have real-time shadows."},
    {"multi_compile_fwdbase", "#pragma multi_compile_fwdbase", "Adds keywords for PassType.ForwardBase."},
    {"multi_compile_fwdbasealpha", "#pragma multi_compile_fwdbasealpha", "Adds keywords for PassType.ForwardBase with vertex lights."},
    {"multi_compile_instancing", "#pragma multi_compile_instancing", "Adds INSTANCING_ON (and PROCEDURAL_ON for procedural instancing)."},
    {"multi_compile_lightpass", "#pragma multi_compile_lightpass", "Adds keywords for all passes that draw real-time light and shadows, except Light Probes."},
    {"multi_compile_particles", "#pragma multi_compile_particles", "Adds SOFTPARTICLES_ON for Particle System passes."},
    {"multi_compile_prepassfinal", "#pragma multi_compile_prepassfinal", "Adds keywords for PassType.Deferred."},
    {"multi_compile_shadowcaster", "#pragma multi_compile_shadowcaster", "Adds SHADOWS_DEPTH, SHADOWS_CUBE for PassType.ShadowCaster."},
    {"multi_compile_shadowcollector", "#pragma multi_compile_shadowcollector", "Adds SHADOWS_SPLIT_SPHERES, SHADOWS_SINGLE_CASCADE for screen-space shadows."},
    // Not in the current pragma reference, but used by Unity's own shaders.
    {"raytracing", "#pragma raytracing <name>", "Declares a ray tracing shader program (used by HDRP ray tracing passes). Not in the current pragma reference."},
    {"prefer_hlslcc", "#pragma prefer_hlslcc <graphics APIs>", "Used by Unity's own shaders. Not in the current pragma reference."},
    {"extended_structured_buffer_bindings", "#pragma extended_structured_buffer_bindings", "Used by Unity's own shaders. Not in the current pragma reference."},
    {"pack_matrix", "#pragma pack_matrix(row_major | column_major)", "Standard HLSL: sets the matrix packing order."},
    {"warning", "#pragma warning(...)", "Standard HLSL: modifies compiler warning behavior."},
    {"def", "#pragma def(...)", "Standard HLSL: defines a constant register value."},
  };
  constexpr Entry kTargets[] = {
    {"2.0", "shader model", "Equivalent to DirectX shader model 2.0. Works on all platforms supported by Unity."},
    {"2.5", "shader model", "Almost the same as 3.0, but with only 8 interpolators, and no explicit LOD texture sampling."},
    {"3.0", "shader model", "Equivalent to DirectX shader model 3.0."},
    {"3.5", "shader model", "Equivalent to OpenGL ES 3.0."},
    {"4.0", "shader model", "Equivalent to DirectX shader model 4.0, but without the requirement to support 8 MRTs."},
    {"gl4.1", "shader model", "Equivalent to OpenGL 4.1."},
    {"4.5", "shader model", "Equivalent to OpenGL ES 3.1."},
    {"4.6", "shader model", "Equivalent to OpenGL 4.1. This is the highest OpenGL level supported on a Mac."},
    {"5.0", "shader model", "Equivalent to DirectX shader model 5.0, but without the requirement to support 32 interpolators or cubemap arrays."},
  };
  constexpr Entry kRequires[] = {
    {"interpolators10", "requirement", "At least 10 vertex-to-fragment interpolators are available."},
    {"interpolators15", "requirement", "At least 15 vertex-to-fragment interpolators are available."},
    {"interpolators32", "requirement", "At least 32 vertex-to-fragment interpolators are available."},
    {"integers", "requirement", "Integers are a supported data type, including bit/shift operations."},
    {"mrt4", "requirement", "At least 4 render targets are supported."},
    {"mrt8", "requirement", "At least 8 render targets are supported."},
    {"derivatives", "requirement", "Pixel shader derivative instructions (ddx/ddy) are supported."},
    {"samplelod", "requirement", "Explicit texture LOD sampling (tex2Dlod / SampleLevel) is supported."},
    {"fragcoord", "requirement", "Pixel location input in pixel shader is supported."},
    {"2darray", "requirement", "2D texture arrays are a supported data type."},
    {"cubearray", "requirement", "Cubemap arrays are a supported data type."},
    {"instancing", "requirement", "SV_InstanceID input system value is supported."},
    {"geometry", "requirement", "Geometry shader stages are supported."},
    {"compute", "requirement", "Compute shaders, structured buffers, and atomic operations are supported."},
    {"randomwrite", "requirement", "Random write (UAV) textures are supported."},
    {"uav", "requirement", "Random write (UAV) textures are supported."},
    {"tesshw", "requirement", "Hardware tessellation is supported, but not necessarily tessellation (hull/domain) shader stages."},
    {"tessellation", "requirement", "Tessellation (hull/domain) shader stages are supported."},
    {"msaatex", "requirement", "The ability to access multi-sampled textures (Texture2DMS in HLSL) is supported."},
    {"sparsetex", "requirement", "Sparse textures with residency info are supported."},
    {"framebufferfetch", "requirement", "The ability to get the current framebuffer from GPU memory is supported."},
    {"fbfetch", "requirement", "The ability to get the current framebuffer from GPU memory is supported."},
    {"setrtarrayindexfromanyshader", "requirement", "Setting the render target array index from any shader stage is supported."},
    {"inlineraytracing", "requirement", "Inline ray tracing is supported."},
  };
  constexpr Entry kRenderers[] = {
    {"d3d11", "graphics API", "DirectX 11 feature level 10 and above, and DirectX 12. Provided for compatibility; use dx11 and dx12 instead."},
    {"dx11", "graphics API", "DirectX 11 feature level 10 and above."},
    {"dx12", "graphics API", "DirectX 12."},
    {"glcore", "graphics API", "OpenGL 3.x, OpenGL 4.x"},
    {"gles3", "graphics API", "OpenGL ES 3.x, WebGL 2.0"},
    {"metal", "graphics API", "Metal on iOS or macOS"},
    {"ps4", "platform", "PlayStation 4"},
    {"ps5", "platform", "PlayStation 5"},
    {"playstation", "platform", "PlayStation 4 or PlayStation 5"},
    {"switch", "platform", "Nintendo Switch"},
    {"vulkan", "graphics API", "Vulkan"},
    {"webgpu", "graphics API", "WebGPU"},
    {"xboxseries", "platform", "Xbox Series S|X"},
    // Not in the current table, but listed by Unity's own shaders.
    {"xboxone", "platform", "Xbox One. Not in the current reference table; used by Unity's own shaders."},
    {"switch2", "platform", "Nintendo Switch 2. Not in the current reference table; used by Unity's own shaders."},
    {"gles", "graphics API", "OpenGL ES 2.0. Not in the current reference table; used by Unity's own shaders."},
  };
  constexpr Entry kDirectives[] = {
    {"include", "#include \"file\"", "Standard HLSL include directive."},
    {"include_with_pragmas", "#include_with_pragmas \"path-to-include-file\"", "Unity: include a file and apply the `#pragma` directives it contains."},
    {"define", "#define NAME", "Standard HLSL #define directive."},
    {"define_for_platform_compiler", "#define_for_platform_compiler NAME",
      "Unity: sends a `#define` directive to the platform-specific shader compiler. The Unity preprocessor doesn't use "
      "symbols you define this way."},
    {"pragma", "#pragma ...", "Pass Unity-specific or standard HLSL directives to the shader compiler."},
    {"if", "#if", "Conditional compilation."},
    {"ifdef", "#ifdef", "Conditional compilation."},
    {"ifndef", "#ifndef", "Conditional compilation."},
    {"elif", "#elif", "Conditional compilation."},
    {"else", "#else", "Conditional compilation."},
    {"endif", "#endif", "Conditional compilation."},
    {"undef", "#undef", "Removes a macro definition."},
    {"error", "#error", "Emits a compile error."},
    {"line", "#line", "Sets the line number and file name for diagnostics."},
  };
} // namespace
std::span<const Entry> shaderBlockKeywords() {
  return kShaderBlock;
}
std::span<const Entry> subShaderBlockKeywords() {
  return kSubShaderBlock;
}
std::span<const Entry> passBlockKeywords() {
  return kPassBlock;
}
std::span<const Entry> commands() {
  return kCommands;
}
std::span<const Entry> stencilFields() {
  return kStencilFields;
}
std::span<const Entry> blendFactors() {
  return kBlendFactors;
}
std::span<const Entry> blendOperations() {
  return kBlendOps;
}
std::span<const Entry> cullModes() {
  return kCull;
}
std::span<const Entry> zTestOperations() {
  return kZTest;
}
std::span<const Entry> stencilComparisons() {
  return kStencilCompare;
}
std::span<const Entry> stencilOperations() {
  return kStencilOps;
}
std::span<const Entry> onOff() {
  return kOnOff;
}
std::span<const Entry> trueFalse() {
  return kTrueFalse;
}
std::span<const Entry> colorMaskValues() {
  return kColorMask;
}
std::span<const Entry> propertyTypes() {
  return kPropertyTypes;
}
std::span<const Entry> propertyAttributes() {
  return kPropertyAttributes;
}
std::span<const Entry> subShaderTags() {
  return kSubShaderTags;
}
std::span<const Entry> passTags() {
  return kPassTags;
}
std::span<const Entry> pragmas() {
  return kPragmas;
}
std::span<const Entry> pragmaTargets() {
  return kTargets;
}
std::span<const Entry> pragmaRequires() {
  return kRequires;
}
std::span<const Entry> renderers() {
  return kRenderers;
}
std::span<const Entry> preprocessorDirectives() {
  return kDirectives;
}
std::span<const Entry> tagValues(std::string_view key) {
  if (iequals(key, "RenderPipeline"))
    return kRenderPipelineValues;
  if (iequals(key, "Queue"))
    return kQueueValues;
  if (iequals(key, "ForceNoShadowCasting") || iequals(key, "IgnoreProjector"))
    return kShadowValues;
  if (iequals(key, "PreviewType"))
    return kPreviewTypeValues;
  if (iequals(key, "RenderType"))
    return kRenderTypeValues;
  if (iequals(key, "LightMode"))
    return kLightModeValues;
  if (iequals(key, "PassFlags"))
    return kPassFlagsValues;
  if (iequals(key, "RequireOptions"))
    return kRequireOptionsValues;
  if (iequals(key, "UniversalMaterialType"))
    return kUniversalMaterialTypeValues;
  return {};
}
const Entry *find(std::span<const Entry> table, std::string_view name) {
  for (const Entry &entry : table) {
    if (iequals(entry.name, name))
      return &entry;
  }
  return nullptr;
}
const Entry *findCommand(std::string_view name) {
  return find(kCommands, name);
}
const Entry *findKeywordAnywhere(std::string_view name) {
  for (auto table : {shaderBlockKeywords(), subShaderBlockKeywords(), passBlockKeywords(), commands()}) {
    if (const Entry *entry = find(table, name))
      return entry;
  }
  return nullptr;
}
std::span<const Entry> commandValues(std::string_view command) {
  if (iequals(command, "AlphaToMask") || iequals(command, "ZWrite"))
    return kOnOff;
  if (iequals(command, "Conservative") || iequals(command, "ZClip"))
    return kTrueFalse;
  if (iequals(command, "Cull"))
    return kCull;
  if (iequals(command, "ZTest"))
    return kZTest;
  if (iequals(command, "BlendOp"))
    return kBlendOps;
  return {};
}
std::span<const Entry> stencilFieldValues(std::string_view field) {
  if (istartsWith(field, "Comp"))
    return kStencilCompare;
  if (istartsWith(field, "Pass") || istartsWith(field, "Fail") || istartsWith(field, "ZFail"))
    return kStencilOps;
  return {};
}
bool isScopeKeyword(std::string_view word) {
  for (std::string_view keyword : {"Pass", "Tags", "Name", "LOD", "UsePass", "GrabPass", "PackageRequirements", "SubShader", "Properties", "Fallback", "CustomEditor", "CustomEditorForRenderPipeline", "Dependency"}) {
    if (iequals(word, keyword))
      return true;
  }
  return false;
}
bool isLegacyCommand(std::string_view word) {
  for (std::string_view command : {"AlphaTest", "BindChannels", "Color", "ColorMaterial", "Fog", "Lighting", "Material", "SeparateSpecular", "SetTexture"}) {
    if (iequals(word, command))
      return true;
  }
  return false;
}
bool isStandardHlslPragma(std::string_view name) {
  return name == "pack_matrix" || name == "warning" || name == "def";
}
bool isKeywordPragma(std::string_view name) {
  for (std::string_view base : {"multi_compile", "shader_feature", "dynamic_branch"}) {
    if (name.substr(0, base.size()) != base)
      continue;
    std::string_view rest = name.substr(base.size());
    if (rest.empty())
      return true;
    if (rest.substr(0, 6) == "_local")
      rest.remove_prefix(6);
    if (rest.empty())
      return true;
    for (std::string_view stage : {"_vertex", "_fragment", "_hull", "_domain", "_geometry", "_raytracing"}) {
      if (rest == stage)
        return true;
    }
  }
  return false;
}
bool isShortcutKeywordPragma(std::string_view name) {
  return name.substr(0, 14) == "multi_compile_" && !isKeywordPragma(name) && find(kPragmas, name) != nullptr;
}
} // namespace sls::ref
