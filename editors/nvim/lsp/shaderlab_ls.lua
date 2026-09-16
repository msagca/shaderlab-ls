-- Neovim 0.11+ config for shaderlab-ls. Put this file in a runtime "lsp/" folder (for example
-- ~/.config/nvim/lsp/shaderlab_ls.lua), then call vim.lsp.enable('shaderlab_ls').
--
-- Neovim maps *.shader to Godot's gdshader by default; override it for Unity projects:
--   vim.filetype.add { extension = { shader = 'shaderlab', compute = 'hlsl', cginc = 'hlsl' } }

---@type vim.lsp.Config
return {
  cmd = { 'shaderlab-ls', '--stdio' },
  filetypes = { 'shaderlab', 'hlsl' },
  root_markers = { 'ProjectSettings', 'Assets', '.git' },
  init_options = {
    -- unityEditorPath = 'C:/Program Files/Unity/Hub/Editor/6000.6.0f1/Editor',
    -- keywords = { '_NORMALMAP' },       -- shader keywords to treat as enabled
    -- defines = { 'MY_DEFINE=1' },       -- extra macros for every FXC compile
    diagnostics = { fxc = true, delay = 400 },
  },
}
