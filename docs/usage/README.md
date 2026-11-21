# Using shaderlab-ls

## Command line

```text
shaderlab-ls [--stdio]
shaderlab-ls --check <file> [check options]
shaderlab-ls --format <file|-> [format options]
shaderlab-ls --version | --help
```

Modes:

| option | effect |
| --- | --- |
| `--stdio` | Run the language server over stdio (the default). |
| `--check <file>` | Print diagnostics for a `.shader`, `.compute` or `.hlsl` file; exit 1 on errors. |
| `--format <file\|->` | Print the formatted file (`-` reads stdin); exit 1 if it can't be formatted. |

Check options:

| option | effect |
| --- | --- |
| `--editor <path>` | Unity editor to take includes from. |
| `--dxc <library>` | DXC library to try first. |
| `--compiler <name>` | `auto` (default), `fxc`, `dxc` or `none`. |

Format options:

| option | effect |
| --- | --- |
| `--assume-filename <path>` | Where stdin's text lives, which picks the `.clang-format` that applies. |
| `--clang-format <exe>` | The clang-format to format with. |

## Settings

Pass as `initializationOptions`, or later through `workspace/didChangeConfiguration` (optionally under a `shaderlab`
key):

| Setting | Default | Meaning |
| --- | --- | --- |
| `clangFormatPath` | auto | The clang-format to format with. |
| `dxcPath` | auto | The DXC library (`dxcompiler.dll`, `libdxcompiler.so`) to try first. |
| `unityEditorPath` | auto | Unity install to take built-in includes and packages from. |
| `keywords` | `[]` | Shader keywords to treat as enabled when compiling. |
| `defines` | `[]` | Extra macros for every compile, as `NAME` or `NAME=VALUE`. |
| `diagnostics.compiler` | `auto` | `auto`, `fxc`, `dxc` or `none`. |
| `diagnostics.delay` | `400` | Milliseconds to wait after an edit before compiling. |
| `indexCache` | `true` | Where the index is saved between sessions: `true` for the user's cache folder, `false` for nowhere, or a folder. |
| `semanticTokens` | `true` | `true` or `false` for all languages, or `{"shaderlab": ..., "hlsl": ..., "glsl": ...}` for each. |
