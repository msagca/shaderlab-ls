# shaderlab-ls

[![build](https://github.com/msagca/shaderlab-ls/actions/workflows/build.yml/badge.svg)](https://github.com/msagca/shaderlab-ls/actions/workflows/build.yml)

> [!WARNING]
> This tool was built with the help of AI. It is tested, but it has not had the years of use that catch the rarer
> bugs, so use it with caution. Formatting rewrites your files, so keep them under version control and review the
> changes it makes, and treat its diagnostics as a guide rather than the final word: Unity's own compiler is.

A language server for Unity ShaderLab (`.shader`) and the HLSL inside it, plus `.compute`, `.hlsl` and `.cginc`
files. Written in C++, it compiles the HLSL the way Unity does: with **FXC** (`d3dcompiler_47.dll`) by default, and
with **DXC** for shaders that ask for it with `#pragma use_dxc`. It runs on Linux too, where FXC does not exist and
DXC does all the compiling (see [Platforms](#platforms)). ShaderLab syntax, values and documentation come from the
Unity 6.6 Manual's [ShaderLab language reference](https://docs.unity3d.com/Manual/SL-Reference.html). GLSL — a
`GLSLPROGRAM` block or a `.glsl`/`.glslinc` file — is formatted but never analyzed; pair it with
[glsl_analyzer](https://github.com/nolanderc/glsl_analyzer) for the rest, and see
[Integrations](#integrations) for which of the two should format.

For **syntax highlighting**, pair it with [tree-sitter-shaderlab](https://github.com/msagca/tree-sitter-shaderlab),
my Tree-sitter grammar for ShaderLab. This server implements no semantic highlighting; the two are meant to be used
together.

## Features

- **ShaderLab diagnostics**: block structure, material property declarations, render state commands and their values
  (`Blend`, `Cull`, `Stencil`, `ZTest`, ...), tags with closed value sets, `PackageRequirements` placement, commands
  that aren't in the current reference; `[_Property]` references to undeclared properties are hints. Forms the
  reference doesn't list but Unity's own shaders use (`Dependency`, `LOD` in a `Pass`, fixed-function commands,
  `ZTest Off`, ...) are accepted, so all 360 `.shader` files shipped with Unity 6000.6 come out clean.
- **Pragma checks**: unknown directives, `target`, `require` and renderer values, keyword declarations, missing
  `vertex`/`fragment` entry points.
- **HLSL diagnostics from FXC or DXC** for each shader stage (`vs`/`ps`/`gs`/`hs`/`ds`, `cs` for compute kernels),
  with Unity's include resolution and preprocessing — see [How HLSL is checked](#how-hlsl-is-checked). Errors inside
  included files are reported on the `#include` line that pulls them in.
- **Completion, hover, go to definition and document symbols**, for ShaderLab keywords, commands, values, tags and
  pragmas, and for HLSL symbols — properties, entry points, `#include` paths, functions, structs, variables, macros
  — from the shader and everything it includes (URP/HDRP packages, `UnityCG.cginc`, ...).
- **Formatting** of `.shader`, `.compute`, `.hlsl`, `.cginc`, `.hlslinc`, `.glsl` and `.glslinc` files, over LSP
  (`textDocument/formatting`) or the command line (`--format`).

## Formatting

A `.shader` is laid out by this server and the code inside it by
[clang-format](https://clang.llvm.org/docs/ClangFormat.html); a file that is code from end to end — `.compute`,
`.hlsl`, `.cginc`, `.hlslinc`, `.glsl`, `.glslinc` — goes to clang-format whole. GLSL is laid out as C++ too, which
it is close enough to: Unity's own `.glslinc` files come back unchanged but for their layout.

The ShaderLab part gets one statement per line, indented by nesting, with braces where the style puts them, blocks
written on one line (`Tags { ... }`) left on one line, consistent spacing, and no empty lines at a block's edge.
Comments, strings, attribute text, keyword casing, line endings and a byte order mark are preserved; files with
unbalanced braces or unterminated strings, comments or code blocks are refused rather than guessed at. Formatting
all 360 shaders shipped with Unity 6000.6 changes only whitespace, is idempotent, and leaves both the
tree-sitter-shaderlab syntax tree and this server's diagnostics unchanged.

The layout comes from the `.clang-format` (or `_clang-format`) that applies to the file, exactly as clang-format
resolves it. `IndentWidth`, `TabWidth`, `UseTab`, `MaxEmptyLinesToKeep` and `BraceWrapping.AfterStruct` shape the
ShaderLab part, and the code itself goes to clang-format, so the whole file follows one set of settings; the
editor's `tabSize`/`insertSpaces` are ignored, since they would disagree with the code. With no `.clang-format`
anywhere above the file, Unity's own layout is used — `{BasedOnStyle: Microsoft, AlignTrailingComments: false,
ColumnLimit: 0, MaxEmptyLinesToKeep: 0}`: four spaces, braces on their own line, no empty lines, long lines left
alone.

Three things in HLSL are not C++ and are handled around clang-format: entry point attributes
(`[numthreads(8, 8, 1)]`) and Unity `#pragma` directives are hidden from it and put back; `#include` order is never
sorted, because in HLSL it is part of the program; and a return semantic (`float4 frag(v2f i) : SV_Target`), which
reads as a constructor initializer list, is joined back onto its signature. With those settled, the 233 shaders
shipped with the Unity 6000.6 editor come out byte-identical under clang-format 18 and 23 for all but four — the
usual spread across versions, so pin one for a shared project.

clang-format is looked up on `PATH`; `clangFormatPath` (or `--clang-format`) points at another one. It is optional
for `.shader` files, whose code blocks are then left as they were written, but a file that is code from end to end
cannot be formatted without it.

## Platforms

| | Windows | Linux |
| --- | --- | --- |
| ShaderLab diagnostics, completion, hover, definition, symbols | yes | yes |
| Formatting (needs clang-format) | yes | yes |
| HLSL diagnostics from FXC | yes | no: `d3dcompiler_47.dll` is Windows only |
| HLSL diagnostics from DXC | for `#pragma use_dxc` | for every shader |

DXC is loaded when first needed, from the first of: the `dxcPath` setting, the project's Unity editor
(`Data/Tools/dxcompiler.dll`, the DXC Unity itself compiles with; `libdxcompiler.so` on Linux), and the system's
library path — on Linux the
[release archive](https://github.com/microsoft/DirectXShaderCompiler/releases) unpacked with its `lib` folder on
`LD_LIBRARY_PATH` is enough. The server reports at startup which compilers it found; with none it publishes the
ShaderLab diagnostics alone.

Unity installs are found through Unity Hub: `%ProgramFiles%\Unity\Hub\Editor` and
`%APPDATA%\UnityHub\secondaryInstallPath.json` on Windows, `~/Unity/Hub/Editor` and
`$XDG_CONFIG_HOME/UnityHub/secondaryInstallPath.json` on Linux.

## Install

Each release carries an x86-64 binary for Windows and Linux, built and tested by
[CI](.github/workflows/release.yml) from the tag it is named after:

| file | for |
| --- | --- |
| `shaderlab-ls-<version>-windows-x64.zip` | Windows 10/11, x86-64 |
| `shaderlab-ls-<version>-linux-x64.tar.gz` | Linux, x86-64, glibc 2.35 or newer |

Unpack it and put `shaderlab-ls` on `PATH`; `SHA256SUMS` on the release covers both archives. The Windows binary
needs nothing installed beside it, its runtime being linked in; the Linux one needs the C and C++ runtimes it was
built against, which is to say glibc 2.35 and libstdc++ from GCC 11, or newer. DXC is loaded only if it is there
(see [Platforms](#platforms)). The Neovim plugin needs none of this — it builds the server from its own checkout
(see [Neovim](#neovim)).

## Build

The server can equally be built on the machine that runs it. It needs CMake 3.25+, Ninja and a C++20 compiler —
Visual Studio with the C++ workload and a Windows 10/11 SDK (for `d3dcompiler.h`) on Windows, any recent GCC or
Clang on Linux; `nlohmann/json` is fetched at configure time.
[CI](.github/workflows/build.yml) builds and tests it on both on every push and keeps each build as an artifact.

```
build.cmd            :: Release build in .\build; build.cmd Debug for a debug one
./build.sh           # the same on Linux
```

The tests need Python 3, and the formatting ones clang-format on `PATH`. They compile HLSL with whatever the server
finds; the `use_dxc` check needs a DXC, so set `SHADERLAB_LS_TEST_DXC` or have a Unity editor installed on Windows.

```
python tests\run_tests.py build\shaderlab-ls.exe
python tests\fuzz.py build\shaderlab-ls.exe 300
```

### Releasing

`CMakeLists.txt` holds the version and the tag follows it, never the other way round: bump
`project(... VERSION x.y.z)`, commit, then tag `vx.y.z` and push the tag. The
[release workflow](.github/workflows/release.yml) refuses a tag that disagrees with that version, and refuses a
binary that reports a different one from `--version`, so a release cannot be named something its contents deny.
It builds, tests and fuzzes both targets before publishing, and attaches `SHA256SUMS`.

```
git tag v0.2.0 && git push origin v0.2.0
```

## Usage

```
shaderlab-ls --stdio                        language server over stdio
shaderlab-ls --check <file> [--editor <path>] [--dxc <library>] [--compiler auto|fxc|dxc|none]
                                            print diagnostics and exit (1 if there are errors)
shaderlab-ls --format <file> [--assume-filename <path>] [--clang-format <exe>]
                                            print the formatted file (use - to read stdin, and --assume-filename to
                                            say where it lives); exit 1 if refused
```

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

## Integrations

### Neovim

This repository is also the plugin: [`lsp/shaderlab_ls.lua`](lsp/shaderlab_ls.lua) is the server config and
[`plugin/shaderlab-ls.lua`](plugin/shaderlab-ls.lua) maps the filetypes — Neovim has no `shaderlab` filetype of its
own, detects neither `.hlsl` nor `.glslinc`, and gives `.shader` to Godot's `gdshader`, so the mapping claims
`.shader` unless the file declares a `shader_type`. The server attaches to `glsl` buffers as well, but only to
format them, so [glsl_analyzer](https://github.com/nolanderc/glsl_analyzer) can run alongside it for completion,
hover, definitions and diagnostics. Formatting is the one thing both offer, and it is better left here:
glsl_analyzer 1.7 does not parse the `precision` declarations that Unity's `GLSLSupport.glslinc` opens with — the
file Unity includes in every `GLSLPROGRAM` snippet — and returns nothing for it. Needs Neovim 0.11+, and 0.12+ for
`vim.pack`:

```lua
vim.pack.add { 'https://github.com/msagca/shaderlab-ls' }
vim.lsp.enable 'shaderlab_ls'
```

A plugin manager checks the repository out but does not build it (see [Build](#build)), so the config does it: when
the server starts it compares the `build/` executable against the sources, and builds the repository in place if the
executable is missing or older. That is a build on install, a rebuild after every update, and nothing to configure —
the two lines above are the whole setup.

The check runs each time a client starts rather than once when the config is read, so it does not depend on the
plugin manager announcing anything: `vim.pack.update()`, a bare `git pull` and any other plugin manager are all
caught the same way. The build runs in the background and the server restarts itself when it finishes, so a shader
opened while it is going picks the new executable up on its own.

Building needs CMake, Ninja and a C++ compiler. Both streams of the build go to a log, which `:ShaderlabLsBuildLog`
opens; a failure reports the exit code and the last lines of it, which is where ninja leaves the error. Set
`vim.g.shaderlab_ls_auto_build = false` to be warned that the executable is out of date rather than have one built.
Either way the config falls back to a `shaderlab-ls` on `PATH` when the checkout has none.

The server keeps running while its replacement builds. Neither Windows nor Linux lets a linker write over a
running executable, so the old one is moved aside first and put back if the build fails: a build that cannot
succeed leaves exactly what was there before, and the server restarts onto the new executable when one lands.

To build at update time instead of when the first shader is opened, front-load it with a `PackChanged` hook — the
config then finds the executable already current and does nothing:

```lua
local build = vim.fn.has 'win32' == 1 and { 'cmd.exe', '/c', 'build.cmd' } or { 'sh', 'build.sh' }

vim.api.nvim_create_autocmd('PackChanged', {
  callback = function(event)
    local data = event.data
    if data.spec.name ~= 'shaderlab-ls' or data.kind == 'delete' then return end
    vim.system(build, { cwd = data.path })
  end,
})
```

Note that `vim.pack.update()` checks a plugin out — and only then fires `PackChanged` — when you `:write` its
confirmation buffer; closing that buffer updates nothing. A hook is an optimization for that reason too: the config
does not care how, or whether, the checkout moved.

By hand, or with another plugin manager: copy `lsp/shaderlab_ls.lua` into a runtime `lsp/` folder, put the
executable on `PATH`, and map the filetypes yourself:

```lua
vim.filetype.add {
  extension = { shader = 'shaderlab', hlsl = 'hlsl', compute = 'hlsl', cginc = 'hlsl', glslinc = 'glsl' },
}
vim.lsp.enable 'shaderlab_ls'
```

### conform.nvim

[conform.nvim](https://github.com/stevearc/conform.nvim) never uses a language server unless it is told to
(`lsp_format` defaults to `"never"`), so name the formatter for the filetypes:

```lua
require('conform').setup {
  formatters = {
    shaderlab_ls = {
      command = function() return vim.lsp.config.shaderlab_ls.executable() end,  -- the exe the LSP config found
      -- The layout comes from the .clang-format that applies to the file, so tell it where the buffer lives.
      args = function(_, ctx) return { '--format', '-', '--assume-filename', ctx.filename } end,
    },
  },
  formatters_by_ft = { shaderlab = { 'shaderlab_ls' }, hlsl = { 'shaderlab_ls' }, glsl = { 'shaderlab_ls' } },
}
```

The other way round works too: leave the filetypes out of `formatters_by_ft` and pass `lsp_format = 'fallback'` to
format through the running server. The output is the same either way — same code, same `.clang-format`.

`executable()` rather than `cmd[1]`: `cmd` is a function, so that the executable is resolved when the server
starts instead of once when this config is read. `executable()` resolves the same way, building the repository
first if the executable is behind, and is the supported way to get the path for anything that runs it directly.

## How HLSL is checked

For each `HLSLPROGRAM`/`CGPROGRAM` block the server builds the program Unity would compile for Direct3D 11 and runs
FXC on it once per stage. For a program with `#pragma use_dxc` (with no API list, or one naming `d3d11`/`d3d12`) and
no `never_use_dxc`, it builds Unity's Direct3D 12 program and runs DXC instead.

- The file's `HLSLINCLUDE`/`CGINCLUDE` blocks go before the program, as Unity does, and `#line` directives keep the
  compiler's line and column numbers pointing at the `.shader`. `CGPROGRAM` also gets `HLSLSupport.cginc` and
  `UnityShaderVariables.cginc`, as in Unity.
- Defines: `SHADER_API_D3D11`, `SHADER_API_DESKTOP`, `SHADER_TARGET` (from `#pragma target`, default 2.5),
  `SHADER_STAGE_*`, `UNITY_VERSION`, `UNITY_PASS_<LIGHTMODE>` for Built-in Render Pipeline passes,
  `UNITY_COMPILER_DXC` under DXC, and the keywords of the default variant — the first of each
  `multi_compile`/`shader_feature` set unless it allows "all off", overridable with the `keywords` setting.
- Profiles: `*_5_0` from `#pragma target 4.5` up, for `#pragma require` values that need shader model 5, and always
  for hull, domain and compute; `*_4_0` otherwise. DXC starts at `*_6_0` (`*_6_5` for `inlineraytracing`) and
  compiles HLSL 2018 (`-HV 2018`), the language Unity's shader code is written in.
- Includes resolve like the Editor: `Packages/<name>/...` through embedded, `file:` and cached packages and the
  Editor's built-in ones; `Assets/...` from the project root; anything else relative to the including file, then the
  project root, then the Editor's `CGIncludes`.
- Unity's preprocessor accepts things the compilers don't, so sources are rewritten without moving a line or column:
  byte order marks become spaces, Unity `#pragma` lines are blanked, `#pragma once` is honored by the include
  handler, `#include_with_pragmas` and `#define_for_platform_compiler` become `#include` and `#define`, and `NAME()`
  macros with an empty parameter list become object-like.
- Programs that `only_renderers`/`exclude_renderers` rules out are skipped. Surface shaders, ray tracing programs,
  entry points coming from `#include_with_pragmas` and standalone `.hlsl`/`.cginc` files are only preprocessed,
  because there is no complete program to compile.

## Limitations

- One variant per program is checked, and FXC often reports only the first error of a function body.
- Declaration scanning is lexical: it does not evaluate `#if` branches or resolve struct member types, so there is
  no member completion after `.`.
- `UNITY_VERSION` is computed as `major * 100 + minor * 10` from the editor version (2021.2 -> 202120).
- On Linux every shader compiles with DXC for shader model 6, while Unity uses FXC for most. The two mostly agree
  (all but 3 of the 233 shaders shipped with the Unity 6000.6 editor compile cleanly under both), but SM6 rules
  apply: the pixel shader output semantic `COLOR`, which FXC accepts for `SV_Target`, is an error, for example. DXC
  also runs without the DXIL validator (`-Vd`), which would need `dxil.dll` next to it; the front end, where nearly
  all errors come from, runs in full.
- Documents are synchronized in full on every change; references, rename, range formatting and semantic highlighting
  are not implemented.
- clang-format reads the HLSL as C++, so a `.clang-format` with only a `Language: CSharp` section does not apply to
  it, and options that rewrite code rather than lay it out (`InsertBraces`, `RemoveSemicolon`, ...) apply to HLSL as
  they would to C++.
