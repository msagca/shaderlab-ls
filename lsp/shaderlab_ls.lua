-- Neovim 0.12+ config for shaderlab-ls. It is found automatically when this repository is on 'runtimepath'
-- (vim.pack.add or any plugin manager); otherwise copy this file into a runtime "lsp/" folder, for example
-- ~/.config/nvim/lsp/shaderlab_ls.lua. Either way, enable it with vim.lsp.enable('shaderlab_ls').

local here = vim.fn.fnamemodify(debug.getinfo(1, 'S').source:sub(2), ':p')
local name = vim.fn.fnamemodify(here, ':t:r')  -- the config name is this file's, so a renamed copy still restarts itself

-- Providing the executable, by downloading the release for this checkout's version or building the checkout, is
-- lua/shaderlab-ls.lua: an install or an update has to be able to do it without waiting for a shader to be opened,
-- and plugin/shaderlab-ls.lua runs it then. This file asks that same module, so opening a shader still checks, and
-- a checkout nothing provisioned eagerly is provisioned here as it was before.
-- A lone copy of this file has no such module beside it, and no sources to build either: it serves whatever
-- shaderlab-ls is on PATH, which is what installing the released binary by hand leaves.
local ok, provision = pcall(require, 'shaderlab-ls')
if ok then provision.name = name end

local function executable()
  return ok and provision.executable() or 'shaderlab-ls'
end

---@type vim.lsp.Config
return {
  -- Resolved per client start, not once when this file is read: vim.lsp.enable() reads it eagerly and caches the
  -- result, so an executable that appears later in the session (a build, or one started by hand) would otherwise
  -- never be picked up, not even by reopening the shader.
  cmd = function(dispatchers, config)
    return vim.lsp.rpc.start({ executable(), '--stdio' }, dispatchers, {
      cwd = config.cmd_cwd,
      env = config.cmd_env,
      detached = config.detached,
    })
  end,
  -- Not part of vim.lsp.Config, and the reason cmd being a function costs nothing: whatever wants to run the
  -- executable itself rather than talk to the server (conform.nvim's `command`, a keymap, :!) asks for it here.
  -- It resolves exactly as the server does, so it provides the executable too when the checkout is behind.
  executable = executable,
  -- GLSL is formatted but never analyzed, here as in a GLSLPROGRAM block; pair it with glsl_analyzer for the rest,
  -- which plugin/shaderlab-ls.lua then attaches to GLSLPROGRAM blocks too.
  filetypes = { 'shaderlab', 'hlsl', 'glsl' },
  -- root_markers, but conditional: nothing starts until there is something to start. On a checkout whose
  -- executable is still downloading or building, executable() can only answer with the name on PATH. Starting that
  -- would report a missing language server seconds before the work it is waiting on lands and makes the message
  -- untrue; the restart afterwards comes back through here with an executable to name.
  root_dir = function(bufnr, on_dir)
    if vim.fn.executable(executable()) == 0 then return end
    on_dir(vim.fs.root(bufnr, { 'ProjectSettings', 'Assets', '.git' }))
  end,
  init_options = {
    -- unityEditorPath = 'C:/Program Files/Unity/Hub/Editor/6000.6.0f1/Editor',
    -- clangFormatPath = 'C:/Program Files/LLVM/bin/clang-format.exe',  -- formatting; found on PATH by default
    -- dxcPath = '/opt/dxc/lib/libdxcompiler.so',  -- DXC, before the Unity editor's and the system's
    -- keywords = { '_NORMALMAP' },       -- shader keywords to treat as enabled
    -- defines = { 'MY_DEFINE=1' },       -- extra macros for every compile
    diagnostics = { compiler = 'auto', delay = 400 },  -- compiler: 'auto', 'fxc', 'dxc' or 'none'
  },
}
