-- :checkhealth shaderlab-ls. It reports what the plugin would do and what it has done, and does none of it itself: it
-- reads the provisioning state rather than calling executable(), which would start the very download or build it
-- is asked about, and it asks the executable for its version but never runs it as a server.
local M = {}

local health = vim.health

local function check_neovim()
  health.start 'Neovim'
  if vim.fn.has 'nvim-0.12' == 1 then
    health.ok(tostring(vim.version()))
  else
    health.error(('Neovim %s is too old'):format(tostring(vim.version())), 'The plugin needs Neovim 0.12 or newer.')
  end
end

-- The version the executable reports, or nil and why not.
local function version_of(exe)
  local ok, process = pcall(vim.system, { exe, '--version' }, { text = true })
  if not ok then return nil, tostring(process) end
  local result = process:wait(5000)
  if result.code ~= 0 then return nil, ('exited with %s'):format(result.code) end
  return vim.trim(result.stdout or '')
end

local function check_server(provision, config)
  health.start 'Server'
  local name = provision and provision.name or 'shaderlab_ls'

  if not config then
    health.error(('no LSP config named %q'):format(name),
      'Put this repository on \'runtimepath\' (vim.pack.add or any plugin manager), or copy lsp/shaderlab_ls.lua '
        .. 'into a runtime "lsp/" folder.')
  elseif vim.lsp.is_enabled(name) then
    health.ok(('%s is enabled'):format(name))
  else
    health.warn(('%s is not enabled, so it never starts'):format(name), ('vim.lsp.enable(%q)'):format(name))
  end

  local status = provision and provision.status() or { checkout = false }
  local exe = status.built or status.moved or 'shaderlab-ls'
  local path = vim.fn.exepath(exe)
  if path == '' then
    if status.running then
      health.info 'no executable yet: a download or build is running'
    else
      local advice = { 'Install a release binary and put it on PATH: https://github.com/msagca/shaderlab-ls/releases' }
      if status.checkout and (vim.g.shaderlab_ls_download ~= false or vim.g.shaderlab_ls_auto_build ~= false) then
        advice = { 'Restart Neovim to provide one, or see :ShaderlabLsBuildLog if a build failed.' }
      elseif status.checkout then
        advice = { 'Downloading and building are both off: turn one on, or put shaderlab-ls on PATH.' }
      end
      health.error('no shaderlab-ls executable', advice)
    end
  else
    local version, err = version_of(path)
    local where = status.built and 'built/downloaded' or status.moved and 'set aside by a build' or 'on PATH'
    if version then
      health.ok(('%s (%s): %s'):format(version, where, path))
    else
      health.error(('%s does not run: %s'):format(path, err))
    end
  end

  if not status.checkout then
    health.info 'not a checkout of the repository: the shaderlab-ls on PATH is used, and nothing is downloaded or built'
  else
    if status.release then
      health.info(('the executable is the downloaded release v%s'):format(status.release))
    elseif status.built then
      health.info 'the executable was built from this checkout'
    end
    if status.built and status.current then
      health.ok(('current for this checkout (v%s)'):format(status.version or '?'))
    elseif status.built then
      health.warn('the executable is older than this checkout', status.running and {}
        or { 'It is replaced in the background at startup; restart Neovim, or see the errors below.' })
    end
    if status.running then health.info 'a download or build is running' end
    if status.error then
      health.error(status.error, status.log and { ':ShaderlabLsBuildLog opens the last build log.' } or {})
    end

    local download = vim.g.shaderlab_ls_download ~= false
    local build = vim.g.shaderlab_ls_auto_build ~= false
    if not download then health.info 'downloading is off (vim.g.shaderlab_ls_download = false)' end
    if not build then health.info 'building is off (vim.g.shaderlab_ls_auto_build = false)' end
    if download and not status.asset then
      health.info 'no released binary for this platform: the executable is built from the checkout'
    end
    if download and status.asset and vim.fn.executable 'curl' == 0 then
      health.warn('curl is not on PATH, so no release can be downloaded', build and { 'The checkout is built instead.' } or {})
    end
    -- Only where building is what would happen: a download needs neither of these.
    if build and (not download or not status.asset) then
      if vim.fn.executable 'cmake' == 1 then
        health.ok('cmake: ' .. vim.fn.exepath 'cmake')
      else
        health.warn('cmake is not on PATH, so the checkout cannot be built', 'Building needs CMake and a C++ compiler.')
      end
    end
  end

  local clients = vim.lsp.get_clients { name = name }
  if #clients > 0 then
    health.info(('%d client(s) running'):format(#clients))
  end
end

local function check_tools(config)
  health.start 'Formatting and compilers'
  local options = config and config.init_options or {}

  local clang = options.clangFormatPath
  if clang then
    if vim.fn.executable(clang) == 1 then
      health.ok('clang-format (clangFormatPath): ' .. clang)
    else
      health.error('clangFormatPath does not name an executable: ' .. clang)
    end
  elseif vim.fn.executable 'clang-format' == 1 then
    health.ok('clang-format: ' .. vim.fn.exepath 'clang-format')
  else
    health.warn('clang-format is not on PATH',
      'Nothing but ShaderLab is formatted without it. Install it, or set init_options.clangFormatPath.')
  end

  local diagnostics = options.diagnostics or {}
  local compiler = diagnostics.compiler or 'auto'
  if compiler == 'none' then
    health.info 'HLSL compiling is off (diagnostics.compiler = "none")'
  elseif vim.fn.has 'win32' == 1 then
    health.info(('HLSL diagnostics from %s'):format(compiler == 'auto' and 'FXC, and DXC for #pragma use_dxc' or compiler:upper()))
  elseif compiler == 'fxc' then
    health.error('diagnostics.compiler is "fxc", which exists only on Windows', 'Use "auto" or "dxc".')
  else
    health.info 'HLSL diagnostics from DXC: FXC exists only on Windows'
  end
  if options.dxcPath then
    if vim.uv.fs_stat(options.dxcPath) then
      health.ok('dxcPath: ' .. options.dxcPath)
    else
      health.error('dxcPath does not exist: ' .. options.dxcPath)
    end
  end
  if compiler ~= 'none' then
    health.info 'the server reports which compilers it found when it starts: see :LspLog'
  end
end

local function check_filetypes()
  health.start 'Filetypes'
  local expected = {
    shader = 'shaderlab', hlsl = 'hlsl', hlslinc = 'hlsl', cginc = 'hlsl', compute = 'hlsl', glslinc = 'glsl',
    glsl = 'glsl',
  }
  -- A scratch buffer, empty: the .shader rule reads a buffer to tell Unity's shaders from Godot's.
  local scratch = vim.api.nvim_create_buf(false, true)
  for _, extension in ipairs { 'shader', 'hlsl', 'hlslinc', 'cginc', 'compute', 'glsl', 'glslinc' } do
    local ok, filetype = pcall(vim.filetype.match, { buf = scratch, filename = 'health.' .. extension })
    filetype = ok and filetype or nil
    if filetype == expected[extension] then
      health.ok(('*.%s: %s'):format(extension, filetype))
    else
      health.warn(('*.%s: %s, not %s'):format(extension, filetype or 'nothing', expected[extension]),
        'Another vim.filetype.add() call claims it; the server attaches to shaderlab, hlsl and glsl only.')
    end
  end
  vim.api.nvim_buf_delete(scratch, { force = true })
end

local function check_glsl()
  health.start 'GLSL blocks'
  if vim.g.shaderlab_ls_glsl == false then
    health.info 'off (vim.g.shaderlab_ls_glsl = false)'
    return
  end
  local found, glsl = pcall(require, 'shaderlab-ls.glsl')
  if not found then
    health.error('lua/shaderlab-ls/glsl.lua did not load: ' .. tostring(glsl))
    return
  end
  local servers, missing = glsl.servers()
  for _, config in ipairs(servers) do
    health.ok(('%s is attached to the GLSLPROGRAM blocks of shaders'):format(config.name))
  end
  for _, config in ipairs(missing) do
    local cmd = type(config.cmd) == 'table' and config.cmd[1] or '?'
    health.warn(('%s is enabled for glsl, but %s is not executable'):format(config.name, cmd))
  end
  if #servers == 0 and #missing == 0 then
    health.info('no GLSL language server is enabled for glsl: GLSLPROGRAM blocks are formatted but not analyzed '
      .. '(glsl_analyzer, for instance, would analyze them)')
  end
end

function M.check()
  local found, provision = pcall(require, 'shaderlab-ls')
  local name = found and provision.name or 'shaderlab_ls'
  local ok, config = pcall(function() return vim.lsp.config[name] end)
  config = ok and config or nil

  check_neovim()
  check_server(found and provision or nil, config)
  check_tools(config)
  check_filetypes()
  check_glsl()
end

return M
