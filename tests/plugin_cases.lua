-- The Neovim side of tests/run_plugin_tests.py: one case per headless session, named by $CASE, run against the
-- fixture checkout at $FIXTURE, reporting to $LOG as one event per line for the runner to assert on.
--
-- Started with `nvim --headless -u this-file`, so 'runtimepath' is set before the fixture's plugin/ and lsp/ files
-- are loaded, exactly as a plugin manager would have it. Every case ends by quitting, and the runner kills a
-- session that does not.

local fixture = assert(vim.env.FIXTURE, 'FIXTURE is not set')
local case = assert(vim.env.CASE, 'CASE is not set')
local logfile = assert(vim.env.LOG, 'LOG is not set')

local function log(event)
  local handle = assert(io.open(logfile, 'a'))
  handle:write(event .. '\n')
  handle:close()
end

-- The standalone case is the one install where the repository is not on 'runtimepath' at all, so it has to be
-- kept off before the plugin files under it would be loaded: removing it later is too late, the eager pass in
-- plugin/shaderlab-ls.lua having already run.
vim.opt.runtimepath:prepend(case == 'standalone' and vim.fs.joinpath(fixture, 'standalone') or fixture)
vim.opt.swapfile = false

-- Downloading is the one path these tests leave alone: it wants the network and a published release, neither of
-- which belongs in a test run. Everything here is therefore the build path, which the fixture's own build script
-- stands in for.
vim.g.shaderlab_ls_download = false

vim.api.nvim_create_autocmd('LspAttach', {
  callback = function(event) log(('attach buf=%d client=%d'):format(event.buf, event.data.client_id)) end,
})
vim.api.nvim_create_autocmd('LspDetach', {
  callback = function(event) log(('detach buf=%d client=%d'):format(event.buf, event.data.client_id)) end,
})

local notified = {}
local notify = vim.notify
vim.notify = function(message, level, options)
  local text = tostring(message):gsub('%s+', ' ')
  notified[#notified + 1] = text
  log(('notify level=%s %s'):format(level or 2, text))
  return notify(message, level, options)
end

local function said(pattern)
  return vim.iter(notified):any(function(text) return text:match(pattern) ~= nil end)
end

local function open(name)
  name = name or 'test.shader'
  vim.cmd.edit(vim.fn.fnameescape(vim.fs.joinpath(fixture, 'project', name)))
  log(('filetype %s %s'):format(name, vim.bo.filetype))
end

local function built()
  local exe = vim.fs.joinpath(fixture, 'build', vim.fn.has 'win32' == 1 and 'shaderlab-ls.exe' or 'shaderlab-ls')
  return vim.uv.fs_stat(exe) ~= nil
end

-- What the fixture's build script recorded: one line per run, written before it does anything else, so a case can
-- assert on the builds that did not happen as readily as on those that did.
local function builds()
  local ok, lines = pcall(vim.fn.readfile, vim.fs.joinpath(fixture, 'build-log'))
  return ok and #lines or 0
end

local function clients()
  return vim.lsp.get_clients {}
end

local function finish()
  log('builds ' .. builds())
  log('enabled ' .. tostring(vim.lsp.is_enabled 'shaderlab_ls'))
  log('clients ' .. #clients())
  log('built ' .. tostring(built()))
  log 'done'
  vim.cmd 'qa!'
end

-- Cases wait for what they are testing rather than for a fixed delay, so a slow runner cannot turn a pass into a
-- failure. The deadline only stops a case waiting for something that is never coming; the assertions that follow
-- are the runner's, and they fail on their own.
local function until_(predicate, deadline)
  local waited, step = 0, 100
  local timer = assert(vim.uv.new_timer())
  timer:start(step, step, function()
    vim.schedule(function()
      waited = waited + step
      if not timer:is_closing() and (predicate() or waited >= (deadline or 60000)) then
        timer:stop()
        timer:close()
        finish()
      end
    end)
  end)
end

local function attached()
  return #clients() > 0
end

local cases = {}

-- The filetypes plugin/shaderlab-ls.lua claims, .shader going to Godot when the file declares a shader_type
-- among them. Nothing else in this case: no server is enabled, so nothing starts.
cases.filetypes = function()
  for _, name in ipairs { 'test.shader', 'godot.shader', 'kernel.compute', 'include.cginc', 'unity.hlsl', 'support.glslinc' } do
    open(name)
  end
  finish()
end

-- An install: nothing to run, no shader open, and the executable is provided anyway. This is the pass that used to
-- wait for the first shader of the session.
cases.install = function()
  vim.lsp.enable 'shaderlab_ls'
  until_(built)
end

-- The same with a shader open. It starts with no client, there being nothing to start, and ends with one attached
-- to that same buffer, which was never reopened.
cases.install_with_shader = function()
  vim.lsp.enable 'shaderlab_ls'
  open()
  until_(attached)
end

-- A stale executable serves the buffer while its replacement is built, and the buffer is moved onto the
-- replacement when it lands: a second client for the same buffer, and the first one detached.
cases.stale = function()
  vim.lsp.enable 'shaderlab_ls'
  open()
  until_(function() return said 'shaderlab%-ls built' and #clients() > 0 and clients()[1].id > 1 end)
end

-- An executable newer than the sources is what this checkout should be serving: no build, and a client on it.
cases.current = function()
  vim.lsp.enable 'shaderlab_ls'
  open()
  until_(attached)
end

-- A build that fails leaves the stale executable exactly where it was, reports the exit code, and keeps serving.
-- Waiting for the client as well as for the report: a failure is reported in milliseconds, and a session that
-- quits on it alone quits before the server it is meant to still have has finished starting.
cases.failing_build = function()
  vim.lsp.enable 'shaderlab_ls'
  open()
  until_(function() return said 'build failed' and attached() end)
end

-- With building switched off and downloading out of the question, a stale executable is reported rather than
-- replaced, and it goes on serving the buffer.
cases.no_provider = function()
  vim.g.shaderlab_ls_auto_build = false
  vim.lsp.enable 'shaderlab_ls'
  open()
  until_(function() return said 'neither downloading nor building' and attached() end)
end

-- Provisioning must not enable a config nobody asked for: the build runs, and no server attaches to anything.
cases.not_enabled = function()
  open()
  until_(built)
end

-- An update announced mid-session, after this session has already provisioned once and spent the one attempt it is
-- otherwise allowed. The same event for another plugin is not this checkout's business, even though the sources
-- are stale by the time it arrives.
cases.pack_changed = function()
  vim.lsp.enable 'shaderlab_ls'
  open()
  local function announce(path, name)
    log('announce ' .. name)
    vim.api.nvim_exec_autocmds('PackChanged', {
      data = { kind = 'update', active = true, path = path, spec = { name = name } },
    })
  end
  -- Once the startup build has finished, so that the guard which leaves one build to another cannot answer for
  -- the filter that decides whose update this is: move the sources on, then announce someone else's update.
  vim.defer_fn(function()
    local now = os.time() + 5
    vim.uv.fs_utime(vim.fs.joinpath(fixture, 'src', 'main.cpp'), now, now)
    announce(vim.fs.joinpath(fixture, 'elsewhere'), 'other')
  end, 6000)
  -- Then this checkout's, spelled the way vim.pack may hand it over: another case, and Windows separators.
  vim.defer_fn(function()
    local path = fixture
    if vim.fn.has 'win32' == 1 then path = (fixture:upper():gsub('/', string.char(92))) end
    announce(path, 'shaderlab-ls')
  end, 9000)
  until_(function() return builds() >= 2 end)
end

-- Two announcements while a build is running are left to that build: nothing starts a second cmake over the same
-- folder. The fixture's build script is slow enough for both to arrive while the first is still running.
cases.pack_changed_while_building = function()
  vim.lsp.enable 'shaderlab_ls'
  open()
  local function announce()
    log 'announce shaderlab-ls'
    vim.api.nvim_exec_autocmds('PackChanged', {
      data = { kind = 'update', active = true, path = fixture, spec = { name = 'shaderlab-ls' } },
    })
  end
  vim.defer_fn(announce, 500)
  vim.defer_fn(announce, 1200)
  until_(function() return said 'shaderlab%-ls built' end)
end

-- The documented one-file install: lsp/shaderlab_ls.lua alone in a runtime folder, the repository nowhere on
-- 'runtimepath', and the binary on PATH. Nothing is built, and what is on PATH is what runs.
cases.standalone = function()
  vim.filetype.add { extension = { shader = 'shaderlab' } }  -- the mapping plugin/shaderlab-ls.lua would have made
  vim.lsp.enable 'shaderlab_ls'
  open()
  until_(attached)
end

-- A GLSL server enabled by the user, here tests/fake_glsl_server.py, is attached to a shader's GLSL blocks and to
-- nothing else of it: it is sent the blocks with everything around them blanked, and what it says about the blank
-- lines, or is asked about them, never reaches the buffer.
cases.glsl = function()
  vim.lsp.config('fake_glsl', {
    cmd = { vim.env.PYTHON, vim.env.FAKE_GLSL, vim.fs.joinpath(fixture, 'glsl-record') },
    filetypes = { 'glsl' },
  })
  vim.lsp.enable { 'fake_glsl', 'shaderlab_ls' }
  open 'test.shader'  -- no GLSL in it, so no GLSL server either
  local plain = vim.api.nvim_get_current_buf()
  open 'glsl.shader'
  local buf = vim.api.nvim_get_current_buf()
  local function proxy() return vim.lsp.get_clients({ bufnr = buf, name = 'fake_glsl (shaderlab)' })[1] end
  local function lines()
    local client = proxy()
    if not client then return {} end
    local namespace = vim.lsp.diagnostic.get_namespace(client.id, false)
    local found = vim.tbl_map(function(d) return d.lnum end, vim.diagnostic.get(buf, { namespace = namespace }))
    table.sort(found)
    return found
  end
  local function request(method, line, character)
    -- Of that client alone: asking the buffer would wait on shaderlab-ls as well, which may still be starting.
    local response = proxy():request_sync(method, {
      textDocument = { uri = vim.uri_from_bufnr(buf) },
      position = { line = line, character = character },
    }, 10000, buf)
    return response and response.result
  end

  vim.wait(30000, function() return #lines() > 0 end, 50)
  log('proxy plain ' .. #vim.lsp.get_clients { bufnr = plain, name = 'fake_glsl (shaderlab)' })
  log('proxy glsl ' .. (proxy() and 1 or 0))
  log('glsl-only clients ' .. #vim.lsp.get_clients { name = 'fake_glsl' })
  log('diagnostics ' .. table.concat(lines(), ','))
  log('formats ' .. tostring(proxy():supports_method 'textDocument/formatting'))
  local hover = request('textDocument/hover', 3, 6)
  log('hover inside ' .. tostring(hover and hover.contents))
  hover = request('textDocument/hover', 0, 2)
  log('hover outside ' .. tostring(hover and hover.contents))
  local definition = request('textDocument/definition', 3, 6)
  log('definition ' .. tostring(definition and definition[1].uri == vim.uri_from_bufnr(buf)))

  vim.api.nvim_buf_set_lines(buf, 3, 3, false, { '    float bad;' })
  vim.wait(10000, function() return table.concat(lines(), ',') ~= '4' end, 50)
  log('diagnostics after edit ' .. table.concat(lines(), ','))
  finish()
end

-- Cases run once startup is over, not while this file is being read: filetype detection and the fixture's own
-- plugin file are both in place by then, as they are for anyone who opens a shader in an editor already running.
-- It also puts the eager pass in plugin/shaderlab-ls.lua and the shader being opened in the order that matters,
-- the pass being the later of the two, so a case sees a client start against an executable that is not there yet.
vim.schedule(assert(cases[case], 'no such case: ' .. case))
