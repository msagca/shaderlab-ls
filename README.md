# shaderlab-ls

A language server for Unity ShaderLab (`.shader`) and the HLSL inside it, plus `.compute`, `.hlsl` and `.cginc`
files. Windows only, written in C++, and it checks HLSL with **FXC** (`d3dcompiler_47.dll`), the compiler Unity uses
by default for DirectX 11 and as the front end for OpenGL, Metal and Vulkan.

ShaderLab syntax, values and documentation come from the Unity 6.6 Manual's
[ShaderLab language reference](https://docs.unity3d.com/Manual/SL-Reference.html) and the pages it links to. Only
HLSL is supported inside shaders: `GLSLPROGRAM` blocks are skipped.

## Features

- **ShaderLab diagnostics**: block structure, material property declarations (types, default values, attributes),
  render state commands and their values (`Blend`, `BlendOp`, `ColorMask`, `Cull`, `Offset`, `Stencil`, `ZTest`,
  `ZWrite`, `ZClip`, `Conservative`, `AlphaToMask`), tags with closed value sets, `PackageRequirements` placement,
  and commands that aren't in the current reference. `[_Property]` references to undeclared properties are hints.
- **Unity compatibility**: forms that the reference doesn't list but Unity's own shaders use are accepted:
  `Dependency "Key" = "Value"`, `LOD` in a `Pass`, fixed-function commands (`Lighting`, `Fog`, `Color`, `SetTexture`,
  ...), On/Off and True/False for either kind of switch, `ZTest Off`, the `Any` property type, and the `xboxone`,
  `switch2` and `gles` renderer names. All 360 `.shader` files shipped with Unity 6000.6 and its packages produce no
  errors or warnings from these checks.
- **Pragma checks**: unknown `#pragma` directives, `target`, `require`, `only_renderers`/`exclude_renderers` values,
  keyword declarations, missing `vertex`/`fragment` entry points.
- **HLSL diagnostics from FXC** for each shader stage (`vs`/`ps`/`gs`/`hs`/`ds`, `cs` for compute kernels), with
  Unity's include resolution and preprocessing (see below). Errors inside included files are reported on the
  `#include` line that pulls them in.
- **Completion**: context-aware ShaderLab keywords, commands, values, tags, property types and attributes; pragma
  names and values; HLSL keywords, types, intrinsics, material properties and declarations from the shader and
  everything it includes (URP/HDRP packages, `UnityCG.cginc`, ...).
- **Hover**: reference documentation for ShaderLab keywords, commands, values, tags and pragmas; declarations for HLSL
  symbols, including ones from included files.
- **Go to definition**: `[_Property]` and HLSL uses of material properties, `#pragma vertex vert` entry points,
  `#include` paths, HLSL functions/structs/variables/macros across include files.
- **Document symbols**: Shader > Properties / SubShader > Pass > code blocks > HLSL declarations.
- **Formatting** of `.shader` files, over LSP (`textDocument/formatting`) or the command line (`--format`):
  - one statement per line in Shader, SubShader, Pass and Stencil blocks, indented by nesting;
  - braces of multi-line blocks on their own lines; blocks written on one line (`Tags { ... }`, `Fog { Mode Off }`,
    `"white" {}`) stay on one line;
  - consistent spacing (`_Color ("Color", Color) = (1, 1, 1, 1)`, `Blend SrcAlpha OneMinusSrcAlpha, One Zero`), at
    most one blank line in a row;
  - HLSL/CG/GLSL code keeps its own layout and is only shifted so its least indented line lines up with the
    keyword;
  - comments, strings, property attribute text (`[Header(Some  text)]`), keyword casing, line endings and a byte
    order mark are preserved. Files with unbalanced braces or unterminated strings, comments or code blocks are
    refused rather than guessed at.

  Formatting all 360 `.shader` files shipped with Unity 6000.6 changes only whitespace, is idempotent, and leaves both
  the tree-sitter-shaderlab syntax tree and this server's diagnostics unchanged.

## Build

Requires Visual Studio with the C++ workload and a Windows 10/11 SDK (for `d3dcompiler.h`), CMake 3.25+ and Ninja.
Tested with Visual Studio 2026 (MSVC 14.51) and SDK 10.0.26100. `nlohmann/json` is fetched at configure time.

```
build.cmd            :: Release build in .\build
build.cmd Debug
```

Run the tests (Python 3):

```
python tests\run_tests.py build\shaderlab-ls.exe
python tests\fuzz.py build\shaderlab-ls.exe 300
```

## Usage

```
shaderlab-ls --stdio                        language server over stdio
shaderlab-ls --check Assets/Foo.shader      print diagnostics and exit (1 if there are errors)
shaderlab-ls --format Assets/Foo.shader [--indent 4] [--tabs]
                                            print the formatted file (use - to read stdin); exit 1 if refused
```

With [conform.nvim](https://github.com/stevearc/conform.nvim), following the buffer's indentation settings:

```lua
require('conform').setup {
  formatters = {
    shaderlab_ls = {
      command = 'shaderlab-ls',
      args = function(_, ctx)
        local bo = vim.bo[ctx.buf]
        local args = { '--format', '-', '--indent', tostring(bo.shiftwidth > 0 and bo.shiftwidth or bo.tabstop) }
        if not bo.expandtab then table.insert(args, '--tabs') end
        return args
      end,
    },
  },
  formatters_by_ft = { shaderlab = { 'shaderlab_ls' } },
}
```

### Neovim

Copy [`editors/nvim/lsp/shaderlab_ls.lua`](editors/nvim/lsp/shaderlab_ls.lua) into a runtime `lsp/` folder, put
`shaderlab-ls.exe` on `PATH` (or use an absolute `cmd`), and:

```lua
vim.filetype.add { extension = { shader = 'shaderlab', compute = 'hlsl', cginc = 'hlsl' } }
vim.lsp.enable 'shaderlab_ls'
```

Neovim maps `.shader` to Godot's `gdshader` by default, hence the `filetype.add`.

### Settings

Pass as `initializationOptions`, or later through `workspace/didChangeConfiguration` (optionally under a `shaderlab`
key):

| Setting | Default | Meaning |
| --- | --- | --- |
| `unityEditorPath` | auto | Unity install to take built-in includes and packages from (`Editor` folder, its `Data` folder, or `Unity.exe`). By default: the version in `ProjectSettings/ProjectVersion.txt` under Unity Hub's install folders, else the newest installed editor. |
| `keywords` | `[]` | Shader keywords to treat as enabled when compiling. |
| `defines` | `[]` | Extra macros for every compile, as `NAME` or `NAME=VALUE`. |
| `diagnostics.fxc` | `true` | Run FXC. |
| `diagnostics.delay` | `400` | Milliseconds to wait after an edit before compiling. |

## How HLSL is checked

For each `HLSLPROGRAM`/`CGPROGRAM` block the server builds the program Unity would compile for Direct3D 11 and runs
FXC on it once per stage:

- The file's `HLSLINCLUDE` (or `CGINCLUDE`) blocks are placed before the program, as Unity does. `#line` directives
  keep FXC's line and column numbers pointing at the `.shader` file.
- `CGPROGRAM` also gets `HLSLSupport.cginc` and `UnityShaderVariables.cginc`, which Unity includes automatically.
- Defines: `SHADER_API_D3D11`, `SHADER_API_DESKTOP`, `SHADER_TARGET` (from `#pragma target`, default 2.5),
  `SHADER_STAGE_*`, `UNITY_VERSION`, and the keywords of the default variant: the first keyword of each
  `multi_compile`/`shader_feature` set unless the set allows "all off" (`_`, or a single `shader_feature` keyword).
  `dynamic_branch` keywords are declared as `uniform bool`. The `keywords` setting overrides the choice.
- Built-in Render Pipeline passes also get `UNITY_PASS_<LIGHTMODE>` (for example `UNITY_PASS_META`), which the
  built-in include files test for.
- Profiles: `*_5_0` for `#pragma target 4.5` and above, for `#pragma require` values that need shader model 5
  (`randomwrite`, `compute`, `tessellation`, ...), and always for hull, domain and compute; `*_4_0` otherwise.
- Includes resolve like the Editor: `Packages/<name>/...` through embedded packages, `file:` packages in
  `packages-lock.json`, `Library/PackageCache/<name>@*` and the Editor's built-in packages; `Assets/...` from the
  project root; other names relative to the including file, then the project root, then the Editor's `CGIncludes`.
- Unity's preprocessor accepts things FXC's doesn't, so sources are rewritten without moving any line or column:
  UTF-8 byte order marks become spaces, Unity `#pragma` lines are blanked, `#pragma once` is honored by the include
  handler, `#include_with_pragmas` and
  `#define_for_platform_compiler` become `#include` and `#define`, and `NAME()` macros with an empty parameter list
  become object-like (definitions and call sites).
- Programs with `#pragma only_renderers` that excludes `d3d11`/`dx11`, or `exclude_renderers` that lists them, are
  skipped. Surface shaders (`#pragma surface`), ray tracing programs (`#pragma raytracing`), programs whose entry
  points come from `#include_with_pragmas`, and standalone `.hlsl`/`.cginc` files are only preprocessed, because
  there is no complete program to compile.

## Limitations

- One variant per program is checked (see keyword selection above).
- FXC reports at most the first error of a function body in many cases.
- `UNITY_VERSION` is computed as `major * 100 + minor * 10` from the editor version (for example 2021.2 -> 202120).
- Declaration scanning is lexical: it does not evaluate `#if` branches or resolve struct member types, so there is no
  member completion after `.`.
- Shaders that opt into DXC with `#pragma use_dxc` are still checked with FXC, so SM6-only features are reported.
- Documents are synchronized in full on every change; references, rename, range formatting and semantic
  highlighting are not implemented. Formatting covers ShaderLab only; HLSL inside code blocks is not reformatted.
