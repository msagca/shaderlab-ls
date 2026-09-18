# shaderlab-ls

[![build](https://github.com/msagca/shaderlab-ls/actions/workflows/build.yml/badge.svg)](https://github.com/msagca/shaderlab-ls/actions/workflows/build.yml)

> [!WARNING]
> This tool was built with the help of AI. It is tested, but it has not had the years of use that catch the rarer
> bugs, so use it with caution. Formatting rewrites your files, so keep them under version control and review the
> changes it makes, and treat its diagnostics as a guide rather than the final word: Unity's own compiler is.

A language server for Unity ShaderLab (`.shader`) and the HLSL inside it, plus `.compute`, `.hlsl` and `.cginc`
files. Written in C++, it compiles the HLSL the way Unity does: with **FXC** (`d3dcompiler_47.dll`), the compiler
Unity uses by default for DirectX 11 and as the front end for OpenGL, Metal and Vulkan, and with **DXC** for shaders
that ask for it with `#pragma use_dxc`.

It builds and runs on Linux too, where FXC does not exist and DXC does all the compiling (see
[Platforms](#platforms)).

ShaderLab syntax, values and documentation come from the Unity 6.6 Manual's
[ShaderLab language reference](https://docs.unity3d.com/Manual/SL-Reference.html) and the pages it links to. Only
HLSL is supported inside shaders: `GLSLPROGRAM` blocks are not analyzed or compiled, only formatted.

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
- **HLSL diagnostics from FXC or DXC** for each shader stage (`vs`/`ps`/`gs`/`hs`/`ds`, `cs` for compute kernels),
  with Unity's include resolution and preprocessing (see below). Errors inside included files are reported on the
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
  - blocks written on one line (`Tags { ... }`, `Fog { Mode Off }`, `"white" {}`) stay on one line, and the braces
    of the rest go where the style puts them;
  - consistent spacing (`_Color ("Color", Color) = (1, 1, 1, 1)`, `Blend SrcAlpha OneMinusSrcAlpha, One Zero`);
  - **the code in every block is formatted with [clang-format](https://clang.llvm.org/docs/ClangFormat.html)** (see
    below) and indented to its block: HLSL, CG and GLSL alike. Without clang-format a block keeps its own layout,
    shifted so its least indented line lines up with the keyword;
  - as many empty lines in a row as `MaxEmptyLinesToKeep` allows (none by default), never at the edge of a block,
    and none at the end of the file: it ends with one newline after the last `}`;
  - comments, strings, property attribute text (`[Header(Some  text)]`), keyword casing, line endings and a byte
    order mark are preserved. Files with unbalanced braces or unterminated strings, comments or code blocks are
    refused rather than guessed at.

  Formatting all 360 `.shader` files shipped with Unity 6000.6 changes only whitespace, is idempotent, and leaves both
  the tree-sitter-shaderlab syntax tree and this server's diagnostics unchanged.

### Formatting style

The layout comes from the `.clang-format` (or `_clang-format`) that applies to the shader, exactly as clang-format
resolves it: the first one found in the shader's folder or any folder above it. `IndentWidth`, `TabWidth`, `UseTab`,
`MaxEmptyLinesToKeep` and `BraceWrapping.AfterStruct` shape the ShaderLab part; the code inside the
`HLSLPROGRAM`/`CGPROGRAM`/`GLSLPROGRAM` (and `HLSLINCLUDE`/`CGINCLUDE`) blocks is handed to clang-format itself, so
the whole file follows the same settings. GLSL is laid out as C++ too, which it is close enough to: Unity's own
`.glslinc` files come back out of clang-format unchanged but for their layout. The editor's own
`tabSize`/`insertSpaces` are not used: they would disagree with the code inside the blocks.

Without a `.clang-format`, Unity's own layout is used:
`{BasedOnStyle: Microsoft, AlignTrailingComments: false, ColumnLimit: 0, MaxEmptyLinesToKeep: 0}` — four spaces,
braces on their own line, no empty lines, and long lines left where they were written. Set `MaxEmptyLinesToKeep` in
a `.clang-format` to keep them.

Two things in HLSL are not C++, and clang-format does not see them: entry point attributes on their own line
(`[numthreads(8, 8, 1)]`) and Unity's `#pragma` directives (`#pragma instancing_options ... maxcount:50`). Both are
hidden from it and put back afterwards. `#include` directives are never sorted, whatever the configuration says: in
HLSL their order is part of the program.

A function's return semantic (`float4 frag(v2f i) : SV_Target`) reads as a constructor initializer list in C++, and
clang-format up to at least 18 breaks it onto a line of its own; it is joined back afterwards. Trailing comments are
left where they land rather than aligned, which the versions disagree about. With those two settled, formatting the
233 shaders shipped with the Unity 6000.6 editor came out byte-identical under clang-format 18 (Linux) and 23
(Windows) for all but four, where the two versions indent a wrapped parameter list or a comment block inside `#if`
differently — the same spread any C++ project sees across clang-format versions, so pin one for a shared project.

clang-format is looked up on `PATH`; set `clangFormatPath` (or pass `--clang-format`) to point at another one. It is
optional: without it, code blocks are left as they were written.

## Platforms

| | Windows | Linux |
| --- | --- | --- |
| ShaderLab diagnostics, completion, hover, definition, symbols | yes | yes |
| Formatting (needs clang-format) | yes | yes |
| HLSL diagnostics from FXC | yes | no: `d3dcompiler_47.dll` is Windows only |
| HLSL diagnostics from DXC | for `#pragma use_dxc` | for every shader |

DXC is loaded when first needed, from the first of: the `dxcPath` setting (`--dxc`), the project's Unity editor
(`Data/Tools/dxcompiler.dll`, the DXC Unity itself compiles with; `Data/Tools/libdxcompiler.so` on Linux), and the
system's library path (`PATH`, `LD_LIBRARY_PATH`). On Linux the
[release archive](https://github.com/microsoft/DirectXShaderCompiler/releases), unpacked with its `lib` folder on
`LD_LIBRARY_PATH`, is enough. The server says at startup which compilers it found; with none it publishes the
ShaderLab diagnostics alone, and `--check` notes it on stderr.

Unity installs are found through Unity Hub on both: `%ProgramFiles%\Unity\Hub\Editor` and
`%APPDATA%\UnityHub\secondaryInstallPath.json` on Windows, `~/Unity/Hub/Editor` and
`$XDG_CONFIG_HOME/UnityHub/secondaryInstallPath.json` on Linux.

## Build

There are no released binaries; the server is built on the machine that runs it. [CI](.github/workflows/build.yml) builds and tests it
on Windows and Linux on every push, and keeps each build as a workflow artifact. CMake 3.25+, Ninja and a C++20
compiler. `nlohmann/json` is fetched at configure time.

On Windows, Visual Studio with the C++ workload and a Windows 10/11 SDK (for `d3dcompiler.h`); tested with Visual
Studio 2026 (MSVC 14.51) and SDK 10.0.26100.

```
build.cmd            :: Release build in .\build
build.cmd Debug
```

On Linux, any recent GCC or Clang; tested with GCC 13 on Ubuntu 24.04.

```
./build.sh           # Release build in ./build
./build.sh Debug
```

Run the tests (Python 3; the formatting ones need clang-format on `PATH`). The checks that compile HLSL use whatever
compiler the server finds, and the `use_dxc` one needs a DXC: set `SHADERLAB_LS_TEST_DXC` to one, or have a Unity
editor installed on Windows:

```
python tests\run_tests.py build\shaderlab-ls.exe
python tests\fuzz.py build\shaderlab-ls.exe 300
```

## Usage

```
shaderlab-ls --stdio                        language server over stdio
shaderlab-ls --check Assets/Foo.shader [--editor <path>] [--dxc <library>] [--compiler auto|fxc|dxc|none]
                                            print diagnostics and exit (1 if there are errors)
shaderlab-ls --format Assets/Foo.shader [--assume-filename <path>] [--clang-format <exe>]
                                            print the formatted file (use - to read stdin, and --assume-filename to
                                            say where it lives); exit 1 if refused
```

### Neovim

This repository is also the plugin: [`lsp/shaderlab_ls.lua`](lsp/shaderlab_ls.lua) is the server config and
[`plugin/shaderlab-ls.lua`](plugin/shaderlab-ls.lua) maps the filetypes — Neovim has no `shaderlab` filetype of its
own, does not detect `.hlsl`, and gives `.shader` to Godot's `gdshader`, so the mapping claims `.shader` unless the
file declares a `shader_type`. Needs Neovim 0.11+, and 0.12+ for `vim.pack`:

```lua
vim.pack.add { 'https://github.com/msagca/shaderlab-ls' }
vim.lsp.enable 'shaderlab_ls'
```

A plugin manager checks the repository out but does not build it, and there are no released binaries (see
[Build](#build)). The config runs the `build/` executable from the checkout when it is there and falls back to
`shaderlab-ls` on `PATH`, so building it in place is enough — on install and on every update:

```lua
local build = vim.fn.has 'win32' == 1 and { 'cmd.exe', '/c', 'build.cmd' } or { 'sh', 'build.sh' }

vim.api.nvim_create_autocmd('PackChanged', {
  callback = function(event)
    local data = event.data
    if data.spec.name ~= 'shaderlab-ls' or data.kind == 'delete' then return end
    vim.system(build, { cwd = data.path }, function(result)
      vim.schedule(function()
        if result.code == 0 then
          vim.notify 'shaderlab-ls built'
        else
          vim.notify('shaderlab-ls build failed:\n' .. result.stderr, vim.log.levels.ERROR)
        end
      end)
    end)
  end,
})
```

The build runs in the background, so a shader opened while it is going gets no server; reopen it afterwards.

By hand, or with another plugin manager: copy `lsp/shaderlab_ls.lua` into a runtime `lsp/` folder, put
`shaderlab-ls.exe` on `PATH`, and map the filetypes yourself:

```lua
vim.filetype.add { extension = { shader = 'shaderlab', hlsl = 'hlsl', compute = 'hlsl', cginc = 'hlsl' } }
vim.lsp.enable 'shaderlab_ls'
```

### Formatting from conform.nvim

[conform.nvim](https://github.com/stevearc/conform.nvim) never uses a language server unless it is told to
(`lsp_format` defaults to `"never"`), so name the formatter for the `shaderlab` filetype:

```lua
require('conform').setup {
  formatters = {
    shaderlab_ls = {
      command = function() return vim.lsp.config.shaderlab_ls.cmd[1] end,  -- the exe the LSP config found
      -- The layout comes from the .clang-format that applies to the file, so tell it where the buffer lives.
      args = function(_, ctx) return { '--format', '-', '--assume-filename', ctx.filename } end,
    },
  },
  formatters_by_ft = { shaderlab = { 'shaderlab_ls' } },
}
```

The other way round works too: leave the filetype out of `formatters_by_ft` and pass `lsp_format = 'fallback'` (in
`format_on_save`, `format_after_save` or `conform.format{}`) to format through the running server. The output is the
same either way — same code, same `.clang-format`.

### Settings

Pass as `initializationOptions`, or later through `workspace/didChangeConfiguration` (optionally under a `shaderlab`
key):

| Setting | Default | Meaning |
| --- | --- | --- |
| `clangFormatPath` | auto | The clang-format to lay out HLSL and read `.clang-format` with. By default it is looked up on `PATH`. |
| `dxcPath` | auto | The DXC library (`dxcompiler.dll`, `libdxcompiler.so`) to try first. See [Platforms](#platforms). |
| `unityEditorPath` | auto | Unity install to take built-in includes and packages from (`Editor` folder, its `Data` folder, or `Unity.exe`). By default: the version in `ProjectSettings/ProjectVersion.txt` under Unity Hub's install folders, else the newest installed editor. |
| `keywords` | `[]` | Shader keywords to treat as enabled when compiling. |
| `defines` | `[]` | Extra macros for every compile, as `NAME` or `NAME=VALUE`. |
| `diagnostics.compiler` | `auto` | `auto`: FXC, and DXC for `#pragma use_dxc` or where there is no FXC. `fxc` or `dxc`: that one for every shader. `none`: no compiling. |
| `diagnostics.delay` | `400` | Milliseconds to wait after an edit before compiling. |

## How HLSL is checked

For each `HLSLPROGRAM`/`CGPROGRAM` block the server builds the program Unity would compile for Direct3D 11 and runs
FXC on it once per stage. For a program with `#pragma use_dxc` (with no API list, or one naming `d3d11` or `d3d12`)
and no `never_use_dxc`, it builds the one Unity compiles for Direct3D 12 and runs DXC instead:

- The file's `HLSLINCLUDE` (or `CGINCLUDE`) blocks are placed before the program, as Unity does. `#line` directives
  keep the compiler's line and column numbers pointing at the `.shader` file.
- `CGPROGRAM` also gets `HLSLSupport.cginc` and `UnityShaderVariables.cginc`, which Unity includes automatically.
- Defines: `SHADER_API_D3D11`, `SHADER_API_DESKTOP`, `SHADER_TARGET` (from `#pragma target`, default 2.5),
  `SHADER_STAGE_*`, `UNITY_VERSION`, and the keywords of the default variant: the first keyword of each
  `multi_compile`/`shader_feature` set unless the set allows "all off" (`_`, or a single `shader_feature` keyword).
  `dynamic_branch` keywords are declared as `uniform bool`. The `keywords` setting overrides the choice. DXC also
  gets `UNITY_COMPILER_DXC`, as in Unity, which makes `HLSLSupport.cginc` stand in for the DX9-style `sampler2D` and
  `tex2D` that DXC dropped.
- Built-in Render Pipeline passes also get `UNITY_PASS_<LIGHTMODE>` (for example `UNITY_PASS_META`), which the
  built-in include files test for.
- Profiles: `*_5_0` for `#pragma target 4.5` and above, for `#pragma require` values that need shader model 5
  (`randomwrite`, `compute`, `tessellation`, ...), and always for hull, domain and compute; `*_4_0` otherwise. DXC
  starts at `*_6_0`, and `#pragma require inlineraytracing` takes it to `*_6_5`. It compiles HLSL 2018 (`-HV 2018`),
  the language Unity's shader code is written in; 2021 rejects much of it.
- Includes resolve like the Editor: `Packages/<name>/...` through embedded packages, `file:` packages in
  `packages-lock.json`, `Library/PackageCache/<name>@*` and the Editor's built-in packages; `Assets/...` from the
  project root; other names relative to the including file, then the project root, then the Editor's `CGIncludes`.
- Unity's preprocessor accepts things the compilers don't, so sources are rewritten without moving any line or
  column:
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
- On Linux every shader compiles with DXC, for shader model 6, while Unity compiles most shaders for Direct3D 11
  with FXC. The two mostly agree (all but 3 of the 233 shaders shipped with the Unity 6000.6 editor compile cleanly
  under both), but SM6 rules apply: the pixel shader output semantic `COLOR`, which FXC accepts for `SV_Target`, is
  an error, for example.
- DXC runs without the DXIL validator (`-Vd`), which would need `dxil.dll` next to it; the front end, where nearly
  all errors come from, runs in full.
- Documents are synchronized in full on every change; references, rename, range formatting and semantic
  highlighting are not implemented.
- clang-format reads the HLSL as C++, so a `.clang-format` that only has a `Language: CSharp` section does not apply
  to it: Unity's defaults are used and the code blocks are left as they were written. Options that rewrite code
  rather than lay it out (`InsertBraces`, `RemoveSemicolon`, ...) apply to the HLSL as they would to C++.
