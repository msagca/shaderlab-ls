-- Neovim 0.11+ config for shaderlab-ls. It is found automatically when this repository is on 'runtimepath'
-- (vim.pack.add or any plugin manager); otherwise copy this file into a runtime "lsp/" folder, for example
-- ~/.config/nvim/lsp/shaderlab_ls.lua. Either way, enable it with vim.lsp.enable('shaderlab_ls').

local here = vim.fn.fnamemodify(debug.getinfo(1, 'S').source:sub(2), ':p')
local root = vim.fs.dirname(vim.fs.dirname(here))
local name = vim.fn.fnamemodify(here, ':t:r')  -- the config name is this file's, so a renamed copy still restarts itself
local windows = vim.fn.has 'win32' == 1
local built = vim.fs.joinpath(root, 'build', windows and 'shaderlab-ls.exe' or 'shaderlab-ls')
local sources = vim.fs.joinpath(root, 'src')

local function mtime(path)
  local stat = vim.uv.fs_stat(path)
  return stat and stat.mtime.sec or nil
end

-- Nothing builds this repository on its own: a plugin manager checks it out and leaves it, so after an update the
-- executable in build/ is older than the sources it was built from and quietly keeps serving the previous version.
-- Comparing the two mtimes is what a build system does anyway, and it needs no stamp file, no git and no cooperation
-- from whatever moved the checkout: a plain `git pull` is caught as readily as vim.pack's update.
local function newest_source()
  local newest = mtime(vim.fs.joinpath(root, 'CMakeLists.txt')) or 0
  for path, kind in vim.fs.dir(sources, { depth = 8 }) do
    if kind == 'file' then newest = math.max(newest, mtime(vim.fs.joinpath(sources, path)) or 0) end
  end
  return newest
end

-- Set vim.g.shaderlab_ls_auto_build = false to be told about a stale executable rather than have one built.
local attempted = false

local function build()
  attempted = true  -- at most one build a session, so a build that cannot fix the staleness cannot loop either
  local cmd = windows and { 'cmd.exe', '/c', 'build.cmd' } or { 'sh', 'build.sh' }
  vim.notify 'shaderlab-ls: building'
  vim.system(cmd, { cwd = root, text = true }, function(result)
    vim.schedule(function()
      if result.code ~= 0 then
        vim.notify('shaderlab-ls build failed:\n' .. (result.stderr or ''), vim.log.levels.ERROR)
        return
      end
      vim.notify 'shaderlab-ls built'
      vim.lsp.enable(name, false)  -- pick the new executable up in the buffers that are already open
      vim.lsp.enable(name)
    end)
  end)
end

local function resolve()
  if not vim.uv.fs_stat(sources) then return 'shaderlab-ls' end  -- a copy of this file alone, not the repository
  local have = mtime(built)
  if have and have >= newest_source() then return built end
  if not attempted then
    if vim.g.shaderlab_ls_auto_build == false then
      vim.notify(
        ('shaderlab-ls: %s in %s'):format(
          have and 'the executable is out of date; rebuild it' or 'nothing is built yet; build it', root),
        vim.log.levels.WARN
      )
      attempted = true
    else
      build()
    end
  end
  -- Until the build lands, serve the stale executable rather than nothing; with none, fall back to PATH.
  return have and built or 'shaderlab-ls'
end

---@type vim.lsp.Config
return {
  -- Resolved per client start, not once when this file is read: vim.lsp.enable() reads it eagerly and caches the
  -- result, so an executable that appears later in the session (the build above, or one started by hand) would
  -- otherwise never be picked up, not even by reopening the shader.
  cmd = function(dispatchers, config)
    return vim.lsp.rpc.start({ resolve(), '--stdio' }, dispatchers, {
      cwd = config.cmd_cwd,
      env = config.cmd_env,
      detached = config.detached,
    })
  end,
  -- GLSL is formatted but never analyzed, here as in a GLSLPROGRAM block; pair it with glsl_analyzer for the rest.
  filetypes = { 'shaderlab', 'hlsl', 'glsl' },
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
