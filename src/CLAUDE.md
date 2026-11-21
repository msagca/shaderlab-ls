# The server

Everything is in namespace `sls`; the reference tables are in `sls::ref` (ShaderLab) and `sls::hlsl` (HLSL).

## Code style

- Headers are `.hpp`, included with angle brackets relative to `src/`: `#include <analysis/analysis.hpp>`.
- Format with the root `.clang-format`: `clang-format -i <files>`. It sorts includes and keeps no blank lines.
- A new `.cpp` goes in `add_executable` in `CMakeLists.txt`.
- Each header documents its API in comments. Read the header before its `.cpp`.

## How a request flows

1. `lsp/transport` reads a JSON-RPC message from stdio.
2. `lsp/server` `Server::dispatch` routes it by method. Edits re-run `analyze()`. Compiles are scheduled on a worker
   thread (`schedule`, `workerLoop`, `runCheck`), and diagnostics go out through `publish`.
3. `analysis/analysis` `analyze()` builds an immutable `Analysis`: the ShaderLab parse, one `HlslUnit` per code block
   (or for the whole file), syntax and directive diagnostics, and the code `#if` leaves inactive.
4. `lsp/features` and `lsp/semantic_tokens` answer requests from that `Analysis`, plus the `Workspace` and `Index`
   for cross-file questions.

Offsets are byte `Span`s into UTF-8 text. `LineIndex` converts them to LSP positions in the negotiated `Encoding`.
Never mix them up.

## Modules

| Folder | Role |
| --- | --- |
| `common/` | `Span`, `Diagnostic`, `LineIndex`; string and path helpers (`pathKey` for comparing paths). |
| `shaderlab/` | ShaderLab lexer, parser (syntax and value diagnostics) and `model.hpp` (`ShaderFile`). `reference.cpp` holds the keyword, command and pragma tables from the Unity manual. |
| `hlsl/` | Error-tolerant HLSL lexer and `scanHlsl` (declarations, pragmas, includes; no preprocessing), `localBindings` for block scopes, and the `builtins.cpp` tables of keywords, types, intrinsics and methods. |
| `analysis/` | `analyze()`, `#pragma` and entry point checks, include resolution (`includedFiles`), `conditionals` (three-valued `#if` evaluation), `variant` (keyword choice), and `hlsl_check` (compiling with FXC or DXC and mapping messages back). |
| `compiler/` | `HlslCompiler` with FXC (`d3dcompiler_47.dll`, Windows) and DXC (loaded at run time), plus their message parsers. |
| `unity/` | `UnityProject`: finding the project and editor, `UNITY_VERSION`, and resolving package includes. |
| `index/` | `Index`, the background workspace index (declarations, includes, includers), cached to disk between sessions. |
| `format/` | `formatShaderLab` lays out ShaderLab itself and hands code blocks to clang-format (`style.cpp` runs it and resolves the style). `diff.cpp` turns a result into minimal edits. |
| `lsp/` | Transport, server, `Workspace`, and the features. See `lsp/CLAUDE.md`. |
| `main.cpp` | Command line: `--stdio`, `--check`, `--format`, `--version`, `--help`. |

## Where to add things

- A ShaderLab keyword, command, tag or pragma: the tables in `shaderlab/reference.cpp`. Completion, hover and
  validation read them.
- An HLSL intrinsic, type, semantic or method: `hlsl/builtins.cpp`.
- A ShaderLab diagnostic: `shaderlab/parser.cpp`. A `#pragma` or entry point diagnostic: `validatePragmas` and
  `checkProgramEntryPoints` in `analysis/analysis.cpp`.
- A setting: `Server::applySettings` in `lsp/server.cpp`, the table in `docs/usage/README.md`, and a test.
