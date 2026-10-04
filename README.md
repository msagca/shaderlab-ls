# shaderlab-ls

[![build](https://github.com/msagca/shaderlab-ls/actions/workflows/build.yml/badge.svg)](https://github.com/msagca/shaderlab-ls/actions/workflows/build.yml)

> [!WARNING]
> Built with AI, use with caution.

A language server for Unity ShaderLab (`.shader`) and the HLSL inside it, plus `.compute`, `.hlsl` and `.cginc`
files. Written in C++, it compiles the HLSL the way Unity does: with **FXC** (`d3dcompiler_47.dll`) by default, and
with **DXC** for shaders that ask for it with `#pragma use_dxc`. It runs on Linux too, where FXC does not exist and
DXC does all the compiling (see [Platforms](#platforms)). ShaderLab syntax, values and documentation come from the
Unity 6.6 Manual's [ShaderLab language reference](https://docs.unity3d.com/Manual/SL-Reference.html). GLSL — a
`GLSLPROGRAM` block or a `.glsl`/`.glslinc` file — is formatted but never analyzed; pair it with
[glsl_analyzer](https://github.com/nolanderc/glsl_analyzer) for the rest, and see
[Integrations](#integrations) for which of the two should format.

For **syntax highlighting**, pair it with [tree-sitter-shaderlab](https://github.com/msagca/tree-sitter-shaderlab),
my Tree-sitter grammar for ShaderLab. An editor with no grammar for shaders can take the server's semantic tokens
instead; the Neovim plugin uses them for the languages it has no Tree-sitter parser for, and leaves the rest to
Tree-sitter.

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
- **Variants**: one is checked at a time, Unity's default unless you choose another. A code action on a
  `multi_compile` or `shader_feature` line checks the variant with another of its keywords, or without any, and one
  more goes back to the default; compiler messages from a variant you chose name its keywords.
- **Inactive code**: what an `#if`, `#ifdef` or `#elif` leaves out of the variant checked is reported as unnecessary
  code, which editors fade out, and its declarations give way to the ones compiled. Conditions are worked out with
  the variant's keywords — those of `#include_with_pragmas` files too — the compile's defines and the file's own
  `#define`s; one that depends on something it can't know, such as a macro from an include file, counts as possibly
  true, so code is only faded when it is left out for certain. An include file is worked out in each shader that
  includes it, with that shader's variant and the `#define`s before its `#include`, and faded where none of them
  compiles it; one no shader includes is faded only where its own `#define`s decide.
- **Code actions**: the variant actions above, and a quick fix that declares a material property a `[_Prop]` names.
- **Completion, hover, go to definition and document symbols**, for ShaderLab keywords, commands, values, tags and
  pragmas, and for HLSL symbols — properties, entry points, `#include` paths, functions, structs, variables, macros
  — from the shader and everything it includes (URP/HDRP packages, `UnityCG.cginc`, ...). Hover also describes
  HLSL's own keywords, types, intrinsics, semantics, `[attributes]` and texture methods.
- **Include files in context**: an `.hlsl` or `.cginc` file sees what the shaders that include it declare — the
  includer's own code and its other includes — so hover, go to definition, member completion and signature help work
  on names it uses without including them, such as `_BaseMap` in URP's `LitForwardPass.hlsl`, which `LitInput.hlsl`
  declares before it.
- **Signature help** inside a call: the document's and its includes' functions, overloads included, the intrinsics
  and the methods of the object before a `.`, with the parameter the cursor is on.
- **Workspace symbols**: the functions, structs, cbuffers, globals and macros of the workspace's shader files, and
  the shaders by name, matched by the query's letters in order.
- **Members after `.`**: a struct's fields, a vector's components (`xyzw`, `rgba`), a matrix's `_mRC` elements and a
  texture's or buffer's methods, for parameters, locals, globals, fields, `buffer[i]` elements and function results
  (`input.uv.`, `_Lights[i].color.`, `GetSurface().normal.`), Unity's `TEXTURE2D(...)` declarations included.
  Hover and go to definition on a member find the field of the right struct.
- **References, highlights and rename**. A material property's uses are its declaration, its `[_Prop]` references
  and the shader variables of that name in every pass, HLSL or GLSL; an `HLSLINCLUDE` symbol's, every pass that sees
  it; a pass's own `vert`, only that pass, `#pragma vertex vert` included; a parameter or local, the block it is
  declared in, `for` and `if` headers included, and not a global or another local it shadows or is shadowed by. A
  symbol declared in an include file reaches across the workspace: the file declaring it and every
  `.shader`, `.compute`, `.hlsl` and `.cginc` that includes it, open or not — and with them the material properties
  of the shaders whose variable it is. Renaming a texture property renames its `_ST`, `_TexelSize`, `_HDR` and
  `sampler` variables with it. A rename is refused, with the reason, for names declared outside the workspace (in a
  package or the editor), for struct members, which are matched by name only, and when the new name is already in
  use where it would go.
- **Semantic tokens** for ShaderLab and HLSL: keywords, types, properties, functions, parameters, locals, fields,
  macros, semantics, attributes; for GLSL, only comments, strings, numbers and directives.
- **Formatting** of `.shader`, `.compute`, `.hlsl`, `.cginc`, `.hlslinc`, `.glsl` and `.glslinc` files, over LSP
  (`textDocument/formatting` and `rangeFormatting`) or the command line (`--format`).

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

A range is formatted as part of the whole file, and only the changes that touch its lines are kept: its lines come
out as they would from formatting everything, and the lines around it are left as they are.

clang-format is looked up on `PATH`; `clangFormatPath` (or `--clang-format`) points at another one. It is optional
for `.shader` files, whose code blocks are then left as they were written, but a file that is code from end to end
cannot be formatted without it.

## Platforms

| | Windows | Linux |
| --- | --- | --- |
| ShaderLab diagnostics, completion, hover, definition, symbols, references, rename, semantic tokens | yes | yes |
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
| `shaderlab-ls-<version>-linux-x64.tar.gz` | Linux, x86-64, glibc 2.39 or newer |

Unpack it and put `shaderlab-ls` on `PATH`; `SHA256SUMS` on the release covers both archives. The Windows binary
needs nothing installed beside it, its runtime being linked in; the Linux one needs the C and C++ runtimes of
the image it is built on, currently glibc 2.39 and libstdc++ from GCC 13, or newer. DXC is loaded only if it is there
(see [Platforms](#platforms)). The Neovim plugin fetches these for itself
(see [Neovim](#neovim)), so installing by hand is for everything else.

## Build

The server can equally be built on the machine that runs it. It needs CMake 3.25+ and a C++20 compiler — Visual
Studio with the C++ workload and a Windows 10/11 SDK (for `d3dcompiler.h`) on Windows, any recent GCC or Clang on
Linux; `nlohmann/json` is fetched at configure time.
[CI](.github/workflows/build.yml) builds and tests it on both on every push and keeps each build as an artifact.

```
build.cmd            :: Release build in .\build; build.cmd Debug for a debug one
./build.sh           # the same on Linux
```

Both scripts use the `Release` and `Debug` presets in `CMakePresets.json`, which IDEs pick up too; `cmake --preset
Release` then `cmake --build --preset Release` does the same by hand. The generator and the compiler are CMake's
defaults — Visual Studio and MSVC on Windows — and `CMAKE_GENERATOR`, `CC` and `CXX` choose others as usual. Whatever
the generator, the executable ends up in `build/`.

The tests need Python 3, and the formatting ones clang-format on `PATH`. They compile HLSL with whatever the server
finds; the `use_dxc` check needs a DXC, so set `SHADERLAB_LS_TEST_DXC` or have a Unity editor installed on Windows.
There are three suites — `lsp`, `fuzz` and `plugin` — and ctest runs them against the executable in `build/`:

```
ctest --preset Release                                :: all three
ctest --preset Release -LE slow                       :: all but the fuzzing
python tests\fuzz.py build\shaderlab-ls.exe 300       :: any one by hand, here fuzzing for longer
```

`plugin` tests the [Neovim plugin](#neovim) rather than the server: the filetypes it claims, and the providing of
the executable — when a build is started, what serves a buffer while one runs, and what happens to that buffer when
it lands. It needs Neovim 0.12 or newer on `PATH`, and is reported skipped without one. Each case runs in its own
headless Neovim against a throwaway checkout whose build script stands in for cmake, so nothing is compiled;
downloading a release is the one path left untested, wanting the network. [CI](.github/workflows/build.yml) runs all
three on every push.

### Releasing

`CMakeLists.txt` holds the version and the tag follows it, never the other way round: bump
`project(... VERSION x.y.z)`, commit, then tag `vx.y.z` and push the tag. The
[release workflow](.github/workflows/release.yml) refuses a tag that disagrees with that version, and refuses a
binary that reports a different one from `--version`, so a release cannot be named something its contents deny.
It builds, tests and fuzzes both targets before publishing, and attaches `SHA256SUMS`.

```
git tag v0.3.0 && git push origin v0.3.0
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
| `indexCache` | `true` | Where the index is saved between sessions: `true` for the user's cache folder, `false` for nowhere, or a folder. Read at `initialize`. |
| `semanticTokens` | `true` | Semantic tokens: `true` or `false` for all languages, or `{"shaderlab": ..., "hlsl": ..., "glsl": ...}` for each (`hlsl` covers `.compute`, `.cginc` and `.hlslinc` too). Offered at all only if one is on at `initialize`. |

References, renames and workspace symbols reach into the client's workspace folders (`workspaceFolders`, else
`rootUri`), skipping `Library`, `Temp`, `Logs`, `obj`, `UserSettings`, `Build`, `Builds`, `node_modules` and the
folders Unity hides (a name starting with `.` or ending with `~`). A document outside all of them brings in the Unity
project it is in.

### The index

The server indexes those folders in the background when it starts: every shader file's declarations, the names its
code uses, and what it includes — the package and editor files it reaches through `#include` too, so that the
include graph is whole. Questions about the workspace are then lookups: which files name `X`, which include `Y`,
what is declared as something like `Z`. A workspace question asked before the first crawl is done waits for it, up
to ten seconds, and is answered by reading the folders after that.

The index is saved between sessions, one file per set of folders, in the user's cache folder: `%LOCALAPPDATA%` on
Windows, `$XDG_CACHE_HOME` or `~/.cache` elsewhere, under `shaderlab-ls/index` — never in the project. The next
session loads it and indexes only the files that changed in the meantime: URP's 287 shader files and the package
files they include take about two seconds from scratch and a tenth of one from the saved index, which is under a
megabyte. A saved index unused for 30 days is deleted, and one written by another version of the server is not
read.

It is kept current from three sources: open documents as they are typed, the client's reports of files changing on
disk (`workspace/didChangeWatchedFiles`, which the server registers for when the client can watch files), and, for a
client that can't, a crawl of the folders for changes at most every five seconds, when a workspace question is
asked.

## Integrations

### Neovim

This repository is also the plugin: [`lsp/shaderlab_ls.lua`](lsp/shaderlab_ls.lua) is the server config,
[`lua/shaderlab-ls.lua`](lua/shaderlab-ls.lua) provides the executable that config names, and
[`plugin/shaderlab-ls.lua`](plugin/shaderlab-ls.lua) maps the filetypes and sets the providing off — Neovim has no
`shaderlab` filetype of its own, detects neither `.hlsl` nor `.glslinc`, and gives `.shader` to Godot's `gdshader`, so
the mapping claims `.shader` unless the file declares a `shader_type`. Neovim has no ftplugin for `shaderlab` or `hlsl`
either, so [`ftplugin/`](ftplugin) sets their `'commentstring'` to `// %s` for `gc`. The server attaches to `glsl`
buffers as well, but only to format them, so [glsl_analyzer](https://github.com/nolanderc/glsl_analyzer) can run
alongside it for completion, hover, definitions and diagnostics. Formatting is the one thing both offer, and it is
better left here: glsl_analyzer 1.7 does not parse the `precision` declarations that Unity's `GLSLSupport.glslinc` opens
with — the file Unity includes in every `GLSLPROGRAM` snippet — and returns nothing for it.

The same GLSL server also covers the `GLSLPROGRAM` blocks of a `.shader`. Any config enabled for the `glsl`
filetype whose executable is found gets a second client, named after it with ` (shaderlab)` appended, on each shader
that has a GLSL block. It is shown the shader as a `.glsl` document with everything outside the blocks blanked, so
positions line up as they are. Requests from outside a block never reach it, nor does anything it reports there, and
its formatting is turned off. Set `vim.g.shaderlab_ls_glsl = false` to keep it off shaders. Needs Neovim 0.12+:

```lua
vim.pack.add { 'https://github.com/msagca/shaderlab-ls' }
vim.lsp.enable 'shaderlab_ls'
```

A plugin manager checks the repository out but does not build it, so the plugin provides the server itself. When
the executable in `build/` is missing or older than the sources, it fetches the release binary for the checkout's
version, checks it against that release's `SHA256SUMS`, and unpacks it there. Nothing to install and nothing to
configure — the two lines above are the whole setup, on Windows and Linux x86-64.

Anywhere else, and whenever a download cannot be had, it builds the checkout instead, which needs CMake and a C++
compiler. Both streams of a build go to a log that `:ShaderlabLsBuildLog` opens, and a failure reports
the exit code with the last lines of it, which is where Ninja and MSBuild both leave the errors.

The check runs just after startup, and again whenever `PackChanged` announces this plugin: an install or an update
is met by the download or the build there and then, in the background, with no shader open and nothing waiting on
it. It also runs whenever a client starts, which is what catches a checkout that moved with nothing announcing it
— a bare `git pull`, another plugin manager, or a `vim.pack.update()` confirmed in a session you have since left.
Whichever pass finds the work, the server restarts onto the executable once it lands, and a shader opened while it
was still coming moves onto it without being reopened.

One attempt per session, so a build that cannot succeed is reported once instead of retried at every restart. An
update announced later asks for another, and one announced while a build is running is left to that build.

A downloaded binary is current for the version it was released as, so ordinary commits between tags cause no
downloads and no builds; the version in `CMakeLists.txt` is what moves it. Two switches, either of which leaves
the other path to do the work:

| setting | effect |
| --- | --- |
| `vim.g.shaderlab_ls_download = false` | never download; build the checkout, which is what a developer wants |
| `vim.g.shaderlab_ls_auto_build = false` | never build; a stale or missing executable is reported instead |

With both set, the plugin only reports, and falls back to a `shaderlab-ls` on `PATH` when the checkout has none.

The variant a shader is checked as is chosen with code actions (`gra`, or `vim.lsp.buf.code_action()`) on a
`multi_compile` or `shader_feature` line, and stays chosen while the buffer is open.

Highlighting is Tree-sitter's where there is a parser for it: the config asks the server for semantic tokens only
for the languages — `shaderlab`, `hlsl`, `glsl` — that no Tree-sitter parser is installed for, since the tokens are
drawn above Tree-sitter's highlighting and would paint over it. Which parsers there are is read each time the
server starts. Set `init_options.semanticTokens` to `true`, `false` or a table like `{ shaderlab = false, hlsl = true
}` to decide for yourself.

`:checkhealth shaderlab-ls` reports on all of it: whether the config is enabled, which executable runs and whether
it is current for the checkout, why the last download or build failed, whether clang-format and the compiler
settings are usable, what the shader extensions map to, which languages get semantic tokens, and which GLSL servers
the `GLSLPROGRAM` blocks go to.

The server keeps running while its replacement builds. Neither Windows nor Linux lets a linker write over a
running executable, so the old one is moved aside first and put back if the build fails: a build that cannot
succeed leaves exactly what was there before, and the server restarts onto the new executable when one lands.

Note that `vim.pack.update()` checks a plugin out — and only then fires `PackChanged` — when you `:write` its
confirmation buffer; closing that buffer updates nothing. Nothing rests on that event either way: an update that
landed unannounced is found by the pass at the next startup, or by the one at the next client start.

By hand, or with another plugin manager: copy `lsp/shaderlab_ls.lua` into a runtime `lsp/` folder, put the
executable on `PATH`, and map the filetypes yourself:

```lua
vim.filetype.add {
  extension = { shader = 'shaderlab', hlsl = 'hlsl', compute = 'hlsl', cginc = 'hlsl', glslinc = 'glsl' },
}
vim.api.nvim_create_autocmd('FileType', {
  pattern = { 'shaderlab', 'hlsl' },
  callback = function() vim.bo.commentstring = '// %s' end,  -- for gc, which ftplugin/ would otherwise set
})
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
  `UNITY_COMPILER_DXC` under DXC, and the keywords of the variant: for each `multi_compile`/`shader_feature`
  set, the one chosen for the document with a code action, else the one the `keywords` setting names, else Unity's
  default — the first of the set unless it allows "all off".
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

- One variant per program is compiled at a time, so an error in another variant shows only once you choose it, and
  FXC often reports only the first error of a function body.
- `#if` conditions are worked out, not preprocessed: macros are not expanded, so a condition on a function-like
  macro, or on a macro an include file defines, is not known either way, and the code under it counts as active.
  Include files are searched for declarations in every branch.
- Member completion reads types from declarations, so a type behind a macro — other than Unity's `TEXTURE2D(...)`
  and the like — or a `typedef` is not seen through. Semantic tokens color a name by what it is declared as in the
  shader or its includes, not by the scope it is in.
- `UNITY_VERSION` is computed as `major * 100 + minor * 10` from the editor version (2021.2 -> 202120).
- On Linux every shader compiles with DXC for shader model 6, while Unity uses FXC for most. The two mostly agree
  (all but 3 of the 233 shaders shipped with the Unity 6000.6 editor compile cleanly under both), but SM6 rules
  apply: the pixel shader output semantic `COLOR`, which FXC accepts for `SV_Target`, is an error, for example. DXC
  also runs without the DXIL validator (`-Vd`), which would need `dxil.dll` next to it; the front end, where nearly
  all errors come from, runs in full.
- Documents are synchronized in full on every change.
- References and renames follow `#include`s through the workspace's shader files and nothing else: a material
  property renamed here keeps its old name in `.mat` files and C# scripts. Scopes are read lexically: a parameter or
  local that a macro declares is not seen as one.
- An include file's context comes from its 16 nearest includers, merged: where two shaders declare one of its names
  differently, the nearer one's declaration is used. Its `#if` directives are worked out in the shaders that include
  it directly, with their default variants; when one of its includers is another include file, they are not known
  either way.
- clang-format reads the HLSL as C++, so a `.clang-format` with only a `Language: CSharp` section does not apply to
  it, and options that rewrite code rather than lay it out (`InsertBraces`, `RemoveSemicolon`, ...) apply to HLSL as
  they would to C++.
