-- Provisioning for the server executable: what decides whether the one in build/ is the one this checkout should
-- be serving, and downloads or builds it when it is not. It lives here rather than in lsp/shaderlab_ls.lua because
-- it has to run without it. That config is read when the first shader is opened, which is far too late to notice
-- that an install or an update left nothing to run: the work belongs to the install, in the background, not to the
-- first shader anyone opens. plugin/shaderlab-ls.lua calls refresh() for that; the config calls executable(), so
-- opening a shader still checks and nothing depends on the eager pass having run.
local M = {}

local here = vim.fn.fnamemodify(debug.getinfo(1, 'S').source:sub(2), ':p')
local root = vim.fs.dirname(vim.fs.dirname(here))  -- lua/shaderlab-ls.lua, so the repository is two levels up
local windows = vim.fn.has 'win32' == 1
local built = vim.fs.joinpath(root, 'build', windows and 'shaderlab-ls.exe' or 'shaderlab-ls')
local sources = vim.fs.joinpath(root, 'src')
local repository = 'msagca/shaderlab-ls'
local marker = vim.fs.joinpath(root, 'build', '.release')  -- the release version the executable beside it came from

M.root = root

-- The name of the LSP config to restart once an executable lands. lsp/shaderlab_ls.lua overwrites it with its own
-- file name, so a renamed copy restarts itself; until something enables that name, restarting is a no-op anyway.
M.name = 'shaderlab_ls'

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
-- Restarting the server re-reads the config file, so anything tracked in a file-local there is reset by the very
-- restart a finished build performs. That turns "build at most once" into a build every restart, and a build that
-- cannot succeed into a loop of them. The state that must outlive a reload therefore lives past it.
local state = _G.__shaderlab_ls or {}
_G.__shaderlab_ls = state
local log = vim.fs.joinpath(vim.fn.stdpath 'log', 'shaderlab-ls-build.log')

vim.api.nvim_create_user_command('ShaderlabLsBuildLog', function()
  if not vim.uv.fs_stat(log) then
    vim.notify('shaderlab-ls: no build has been run in this profile yet', vim.log.levels.WARN)
    return
  end
  vim.cmd('tabedit ' .. vim.fn.fnameescape(log))
  vim.bo.modifiable = false
end, { desc = 'Open the last shaderlab-ls build log' })

-- Both streams, always, and kept on disk. cmake, ninja and the compilers write most of what went wrong to stdout,
-- so reporting result.stderr alone reported almost nothing: a failed build looked like a silent one.
local function report(result)
  local lines = {}
  for _, line in ipairs(vim.split((result.stdout or '') .. (result.stderr or ''), '\n')) do
    lines[#lines + 1] = (line:gsub('\r$', ''))
  end
  pcall(vim.fn.mkdir, vim.fs.dirname(log), 'p')
  pcall(vim.fn.writefile, lines, log)
  if result.code == 0 then
    vim.notify 'shaderlab-ls built'
    return
  end
  -- Ninja stops at the first failure and prints it last, so the end of the output is the error itself.
  local tail = {}
  for i = #lines, 1, -1 do
    if lines[i]:match '%S' then
      table.insert(tail, 1, lines[i])
      if #tail >= 20 then break end
    end
  end
  -- The one failure worth naming: something else still has the executable open, so no build can replace it.
  local held = vim.iter(lines):any(function(line) return line:find('LNK1104', 1, true) ~= nil end)
  vim.notify(
    ('shaderlab-ls build failed (exit %d). Full log: %s (:ShaderlabLsBuildLog)%s\n%s'):format(
      result.code,
      log,
      held and '\nAnother process is holding the executable; close other editors running this server.' or '',
      table.concat(tail, '\n')
    ),
    vim.log.levels.ERROR
  )
end

-- Images left behind by a build, newest first. A session that ended mid-build leaves one of these and no `built`,
-- and it is then the only executable there is, so it is found rather than swept.
local function moved_images()
  local directory = vim.fs.dirname(built)
  local found = {}
  if vim.uv.fs_stat(directory) then
    for entry in vim.fs.dir(directory) do
      if entry:match '^shaderlab%-ls%.old%-' then found[#found + 1] = vim.fs.joinpath(directory, entry) end
    end
  end
  table.sort(found, function(a, b) return (mtime(a) or 0) > (mtime(b) or 0) end)
  return found
end

-- A running server holds its own image open, and the linker cannot write over it: Windows fails the link outright
-- with LNK1104, Linux with ETXTBSY. Both allow the file to be *renamed* while it runs, which frees the name for
-- the linker and leaves the running process on the old image until it exits. Stopping the client first is not a
-- substitute: the client list empties before the process does, and an incremental build links seconds later.
local function move_aside()
  if not vim.uv.fs_stat(built) then return end
  local moved = (built:gsub('%.exe$', '')) .. '.old-' .. tostring(vim.uv.hrtime()):sub(-6) .. (windows and '.exe' or '')
  if pcall(vim.uv.fs_rename, built, moved) then state.aside = moved end
end

-- A build that fails must leave what was working exactly where it was. Moving the old executable out of the way is
-- otherwise a way to destroy it: nothing at `built`, nothing on PATH, and a merely stale server has become no
-- server at all. Nothing is deleted until a build has succeeded, for the same reason.
local function restore_aside()
  if state.aside and not vim.uv.fs_stat(built) then pcall(vim.uv.fs_rename, state.aside, built) end
  state.aside = nil
end

local function discard_aside()
  state.aside = nil
  for _, path in ipairs(moved_images()) do pcall(vim.uv.fs_unlink, path) end
end

-- Bring the clients onto whatever is at `built` now. Re-enabling re-reads the config and runs it over the loaded
-- buffers, so a shader opened while the work was in flight ends up with a client even though it never had one:
-- root_dir refuses to start a server there is no executable for, and this is the pass that comes back with one.
-- Guarded, because provisioning also happens at startup, where nothing may have enabled the config at all, and
-- enabling one that was never asked for would attach this server to buffers of its own accord.
local function restart()
  if not vim.lsp.is_enabled(M.name) then return end
  vim.lsp.enable(M.name, false)
  vim.lsp.enable(M.name)
end

local function finish()
  state.running = false
  restart()
end

local function build()
  -- By full path: both scripts locate the repository from their own, and cmd.exe does not look in the working
  -- directory for a script when NoDefaultCurrentDirectoryInExePath is set.
  local script = vim.fs.joinpath(root, windows and 'build.cmd' or 'build.sh')
  local cmd = windows and { 'cmd.exe', '/c', script } or { 'sh', script }
  -- Scheduled, so a client this build was resolved for has spawned before the executable moves underneath it.
  vim.schedule(function()
    move_aside()
    vim.notify('shaderlab-ls: building in ' .. root)
    vim.system(cmd, { cwd = root, text = true }, function(result)
      vim.schedule(function()
        if result.code == 0 then
          discard_aside()
          pcall(os.remove, marker)  -- what is at `built` is this checkout's now, not a release
        else
          restore_aside()
        end
        report(result)
        -- Restart onto what was just linked. The server ran from the moved image throughout, so the only gap is
        -- here, rather than for the length of a build.
        finish()
      end)
    end)
  end)
end

-- Releases carry a binary for the platforms CI builds, so a checkout does not oblige anyone to install a C++
-- toolchain to get a diagnostic. Set vim.g.shaderlab_ls_download = false to always build from the checkout instead.
local function release_version()
  local ok, text = pcall(vim.fn.readfile, vim.fs.joinpath(root, 'CMakeLists.txt'))
  if not ok then return nil end
  for _, line in ipairs(text) do
    -- The project() line, so a three-part cmake_minimum_required cannot answer for it.
    if line:find('project(', 1, true) then
      local version = line:match 'VERSION%s+(%d+%.%d+%.%d+)'
      if version then return version end
    end
  end
end

-- The release asset for this machine, or nothing when no release covers it: only x86-64 Windows and Linux are
-- built, so everything else builds from source as before.
local function asset_for(version)
  local machine = vim.uv.os_uname().machine
  if machine ~= 'x86_64' and machine ~= 'AMD64' then return nil end
  if windows then return ('shaderlab-ls-v%s-windows-x64.zip'):format(version) end
  if vim.fn.has 'linux' == 1 then return ('shaderlab-ls-v%s-linux-x64.tar.gz'):format(version) end
end

local function installed_release()
  local ok, lines = pcall(vim.fn.readfile, marker)
  return ok and lines[1] or nil
end

-- Downloaded, checked against the release's own SHA256SUMS, and only then unpacked over the executable. An archive
-- that does not match what the release says it is never reaches disk as a program.
local function download(version, done)
  local asset = asset_for(version)
  if not asset then return done(false, 'no released binary for this platform') end
  local base = ('https://github.com/%s/releases/download/v%s'):format(repository, version)
  local scratch = vim.fn.tempname()
  vim.fn.mkdir(scratch, 'p')
  local archive = vim.fs.joinpath(scratch, asset)
  local sums = vim.fs.joinpath(scratch, 'SHA256SUMS')
  vim.notify('shaderlab-ls: downloading ' .. asset)
  local fetch = { 'curl', '-sfL', '--retry', '2', '-o', archive, base .. '/' .. asset, '-o', sums, base .. '/SHA256SUMS' }
  vim.system(fetch, { text = true }, function(result)
    vim.schedule(function()
      if result.code ~= 0 then return done(false, 'could not download ' .. asset) end
      local handle = io.open(archive, 'rb')
      local bytes = handle and handle:read 'a'
      if handle then handle:close() end
      if not bytes then return done(false, 'could not read the downloaded archive') end
      local want
      for _, line in ipairs(vim.fn.readfile(sums)) do
        local sum, named = line:match '^(%x+)%s+%*?(.+)$'
        if named == asset then want = sum end
      end
      if not want then return done(false, 'SHA256SUMS does not mention ' .. asset) end
      if want ~= vim.fn.sha256(bytes) then return done(false, 'checksum mismatch for ' .. asset) end
      local directory = vim.fs.dirname(built)
      vim.fn.mkdir(directory, 'p')
      move_aside()  -- the same reason a build needs it: this replaces an executable that may be running
      local unpack = windows
          and { 'powershell', '-NoProfile', '-NonInteractive', '-Command',
            ("Expand-Archive -Path '%s' -DestinationPath '%s' -Force"):format(archive, directory) }
        or { 'tar', '-xzf', archive, '-C', directory }
      vim.system(unpack, { text = true }, function(unpacked)
        vim.schedule(function()
          if unpacked.code ~= 0 or not vim.uv.fs_stat(built) then
            restore_aside()
            return done(false, 'could not unpack ' .. asset)
          end
          discard_aside()
          pcall(vim.fn.writefile, { version }, marker)
          done(true)
        end)
      end)
    end)
  end)
end

-- A release if there is one for this machine, the checkout's own build otherwise, and the build as the fallback
-- when a download cannot be had at all: offline, behind a proxy, or a platform with no asset.
local function provide()
  state.attempted = true
  state.running = true
  local version = release_version()
  local buildable = vim.g.shaderlab_ls_auto_build ~= false
  if vim.g.shaderlab_ls_download == false or not version or not asset_for(version) then
    if buildable then return build() end
    state.running = false
    vim.notify('shaderlab-ls: the executable is out of date and neither downloading nor building is enabled',
      vim.log.levels.WARN)
    return
  end
  download(version, function(ok, err)
    if ok then
      vim.notify('shaderlab-ls: installed v' .. version)
      return finish()
    end
    if buildable then
      vim.notify(('shaderlab-ls: %s; building from the checkout instead'):format(err), vim.log.levels.WARN)
      return build()
    end
    state.running = false
    vim.notify('shaderlab-ls: ' .. err, vim.log.levels.ERROR)
  end)
end

-- Whether what is at `built` is what this checkout should be serving. A released binary is current for the version
-- it was released as: sources that moved on since that tag are unreleased, and rebuilding for them would demand the
-- toolchain the release exists to spare people; bump the version, or set vim.g.shaderlab_ls_download = false, to
-- build what is in the checkout instead.
local function current()
  local have = mtime(built)
  if not have then return false end
  if installed_release() ~= nil and installed_release() == release_version() then return true end
  return have >= newest_source()
end

--- Provide the executable if what is there is missing or behind, and restart the server when one lands.
--- Nothing blocks: the download or build runs in the background and reports for itself.
--- @param force boolean? attempt again even though this session already has. An update replaces the sources under
--- a checkout that was provisioned earlier in the session, and the one attempt it is otherwise allowed is spent.
function M.refresh(force)
  if not vim.uv.fs_stat(sources) then return end  -- the config file alone, copied somewhere, not the repository
  if force then state.attempted = nil end
  if state.running or state.attempted or current() then return end
  provide()
end

--- The executable to run right now, provisioning in the background if it is missing or behind. Until that lands,
--- the stale executable is better than nothing: the one at `built`, or the newest image a build moved out of the
--- way, whether this session's or one left by a session that ended mid-build. Failing both, a shaderlab-ls on PATH.
--- @return string
function M.executable()
  if not vim.uv.fs_stat(sources) then return 'shaderlab-ls' end
  M.refresh()
  if mtime(built) then return built end
  return moved_images()[1] or 'shaderlab-ls'
end

return M
