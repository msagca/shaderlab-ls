# shaderlab-ls

A language server for Unity ShaderLab (`.shader`) and the HLSL inside it, plus `.compute`, `.hlsl` and `.cginc`
files. It reports ShaderLab and compiler (FXC/DXC) diagnostics, and offers completion, hover, go to definition,
references, rename, signature help, symbols, semantic tokens and formatting.

For syntax highlighting, pair it with [tree-sitter-shaderlab](https://github.com/msagca/tree-sitter-shaderlab).

## Documentation

| Read | For |
| --- | --- |
| [docs/install/](docs/install/README.md) | Installing a release, or building from source |
| [docs/usage/](docs/usage/README.md) | The command line, and the language server's settings |
| [docs/neovim/](docs/neovim/README.md) | Using it from Neovim, and formatting with conform.nvim |

## License

[MIT](LICENSE)
