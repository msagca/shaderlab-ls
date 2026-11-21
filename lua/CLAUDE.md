# Neovim plugin

It targets Neovim 0.12+ and spans four runtime folders at the repository root:

| File | What |
| --- | --- |
| `plugin/shaderlab-ls.lua` | Runs at startup: filetype detection for every extension, and the eager `refresh()` that provisions the server. |
| `lsp/shaderlab_ls.lua` | The `vim.lsp.config` for the server. It also works as a standalone copy with no repository beside it. |
| `lua/shaderlab-ls.lua` | Provisioning: decides whether `build/`'s executable fits this checkout, then downloads the release or runs `build.sh`/`build.cmd`. |
| `lua/shaderlab-ls/glsl.lua` | Proxies GLSL servers onto `.shader` buffers, with everything outside `GLSLPROGRAM` blanked. |
| `lua/shaderlab-ls/health.lua` | `:checkhealth shaderlab-ls`. It reads the provisioning state and never starts a build or a download. |
| `ftplugin/*.lua` | `commentstring` for the `shaderlab` and `hlsl` filetypes. |

The plugin runs the build scripts, which must only configure and build. Running the tests there would mean
starting Neovim from inside Neovim.

Test changes here with the `plugin` test. See `tests/CLAUDE.md`.
