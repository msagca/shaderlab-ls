-- Neovim 0.11+ config for shaderlab-ls. It is found automatically when this repository is on 'runtimepath'
-- (vim.pack.add or any plugin manager); otherwise copy this file into a runtime "lsp/" folder, for example
-- ~/.config/nvim/lsp/shaderlab_ls.lua. Either way, enable it with vim.lsp.enable('shaderlab_ls').

-- The executable this repository builds, when it is there: a plugin manager checks the repository out but does not
-- build it, and its build\ folder is unlikely to be on PATH. Falls back to the shaderlab-ls on PATH.
local here = vim.fn.fnamemodify(debug.getinfo(1, 'S').source:sub(2), ':p')
local root = vim.fs.dirname(vim.fs.dirname(here))
local built = vim.fs.joinpath(root, 'build', vim.fn.has 'win32' == 1 and 'shaderlab-ls.exe' or 'shaderlab-ls')
local exe = vim.uv.fs_stat(built) and built or 'shaderlab-ls'

---@type vim.lsp.Config
return {
  cmd = { exe, '--stdio' },
  filetypes = { 'shaderlab', 'hlsl' },
  root_markers = { 'ProjectSettings', 'Assets', '.git' },
  init_options = {
    -- unityEditorPath = 'C:/Program Files/Unity/Hub/Editor/6000.6.0f1/Editor',
    -- clangFormatPath = 'C:/Program Files/LLVM/bin/clang-format.exe',  -- formatting; found on PATH by default
    -- dxcPath = '/opt/dxc/lib/libdxcompiler.so',  -- DXC, before the Unity editor's and the system's
    -- keywords = { '_NORMALMAP' },       -- shader keywords to treat as enabled
    -- defines = { 'MY_DEFINE=1' },       -- extra macros for every compile
    diagnostics = { compiler = 'auto', delay = 400 },  -- compiler: 'auto', 'fxc', 'dxc' or 'none'
  },
}
