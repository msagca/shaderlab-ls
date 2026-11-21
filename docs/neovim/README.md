# Neovim

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

## conform.nvim

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
