# shaderlab-ls

A language server for Unity ShaderLab (`.shader`) and the HLSL inside it, plus `.compute`, `.hlsl` and `.cginc`
files. It reports ShaderLab and compiler (FXC/DXC) diagnostics, and offers completion, hover, go to definition,
references, rename, signature help, symbols, semantic tokens and formatting.

For syntax highlighting, pair it with [tree-sitter-shaderlab](https://github.com/msagca/tree-sitter-shaderlab).

## Install

Download the archive for your platform from the
[releases](https://github.com/msagca/shaderlab-ls/releases), unpack it and put `shaderlab-ls` on `PATH`:

| file | for |
| --- | --- |
| `shaderlab-ls-<version>-windows-x64.zip` | Windows 10/11, x86-64 |
| `shaderlab-ls-<version>-linux-x64.tar.gz` | Linux, x86-64, glibc 2.39 or newer |

The Neovim plugin fetches the server for itself (see [Neovim](#neovim)).

Optional, and found by themselves when installed:

- [clang-format](https://clang.llvm.org/docs/ClangFormat.html) on `PATH`, for formatting.
- A Unity editor installed through Unity Hub, for Unity's built-in includes and packages, and its DXC.
- On Linux, [DXC](https://github.com/microsoft/DirectXShaderCompiler/releases) with its `lib` folder on
  `LD_LIBRARY_PATH`, for HLSL diagnostics.

To build it instead, with CMake 3.25+ and a C++20 compiler, on Windows:

```
build.cmd
```

On Linux:

```
./build.sh
```

The executable ends up in `build/`.

## Usage

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

### Settings

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

## Integrations

### Neovim

This repository is also the plugin. It maps the filetypes and downloads the server, or builds it when no release
binary fits. Needs Neovim 0.12+:

```lua
vim.pack.add { 'https://github.com/msagca/shaderlab-ls' }
vim.lsp.enable 'shaderlab_ls'
```

`:checkhealth shaderlab-ls` reports on the setup, and `:ShaderlabLsBuildLog` opens the log of the last build.

| setting | effect |
| --- | --- |
| `vim.g.shaderlab_ls_download = false` | never download; build the checkout |
| `vim.g.shaderlab_ls_auto_build = false` | never build; a stale or missing executable is reported instead |
| `vim.g.shaderlab_ls_glsl = false` | don't attach GLSL servers to the `GLSLPROGRAM` blocks of a shader |

By hand, or with another plugin manager: copy `lsp/shaderlab_ls.lua` into a runtime `lsp/` folder, put the
executable on `PATH`, and map the filetypes yourself:

```lua
vim.filetype.add {
  extension = {
    shader = 'shaderlab',
    hlsl = 'hlsl',
    compute = 'hlsl',
    cginc = 'hlsl',
    glslinc = 'glsl',
  },
}
vim.api.nvim_create_autocmd('FileType', {
  pattern = { 'shaderlab', 'hlsl' },
  callback = function() vim.bo.commentstring = '// %s' end,
})
vim.lsp.enable 'shaderlab_ls'
```

The autocmd sets `commentstring` for `gc`, which `ftplugin/` would otherwise set.

### conform.nvim

[conform.nvim](https://github.com/stevearc/conform.nvim) never uses a language server unless it is told to, so name
the formatter for the filetypes:

```lua
require('conform').setup {
  formatters = {
    shaderlab_ls = {
      command = function()
        return vim.lsp.config.shaderlab_ls.executable()
      end,
      args = function(_, ctx)
        return { '--format', '-', '--assume-filename', ctx.filename }
      end,
    },
  },
  formatters_by_ft = {
    shaderlab = { 'shaderlab_ls' },
    hlsl = { 'shaderlab_ls' },
    glsl = { 'shaderlab_ls' },
  },
}
```

`command` runs the executable the LSP config found. The layout comes from the `.clang-format` that applies to the
file, so `--assume-filename` tells it where the buffer lives.

Or leave the filetypes out of `formatters_by_ft` and pass `lsp_format = 'fallback'` to format through the running
server.

## License

[MIT](LICENSE)
