-- Filetypes, set when this repository is on 'runtimepath'. Neovim knows almost none of these extensions: it has no
-- shaderlab filetype of its own, does not detect *.hlsl or *.glslinc, and gives *.shader to Godot's gdshader. Only
-- *.glsl it already maps to glsl. A vim.filetype.add() call of your own still wins over this one.
vim.filetype.add {
  extension = {
    cginc = 'hlsl',
    compute = 'hlsl',
    glslinc = 'glsl',
    hlsl = 'hlsl',
    hlslinc = 'hlsl',
    shader = function(_, buffer)
      for _, line in ipairs(vim.api.nvim_buf_get_lines(buffer, 0, 20, false)) do
        if line:match '^%s*shader_type%s' then return 'gdshader' end  -- Godot 3 also used *.shader
      end
      return 'shaderlab'
    end,
  },
}

-- The executable the server runs is provided from here as well, not only when the first shader is opened. A plugin
-- manager checks this repository out and leaves it, so an install or an update is exactly the moment there is
-- something to download or build, and the moment nothing else is waiting on it. Doing it then, in the background,
-- means the first shader of the session is met by a server that is already current rather than by a build.
-- lsp/shaderlab_ls.lua still checks when a client starts, so nothing here is load-bearing.
local ok, provision = pcall(require, 'shaderlab-ls')
if not ok then return end

-- After startup rather than during it: 'runtimepath' and the user's vim.lsp.enable() call are both done by then,
-- so a server that lands seconds later has a config to restart, and the check itself is off the startup path.
local function refresh(force)
  vim.schedule(function() provision.refresh(force) end)
end

if vim.v.vim_did_enter == 1 then
  refresh()
else
  vim.api.nvim_create_autocmd('VimEnter', { once = true, callback = function() refresh() end })
end

local function same_path(a, b)
  a, b = vim.fs.normalize(a or ''), vim.fs.normalize(b or '')
  -- Case-insensitively on Windows: vim.pack and 'runtimepath' need not have spelled the drive or the folders alike.
  if vim.fn.has 'win32' == 1 then return a:lower() == b:lower() end
  return a == b
end

-- An update lands new sources under a checkout that may already have been provisioned this session, spending the
-- one attempt a session is otherwise allowed, so it asks for another (force). An install is announced before this
-- file has been loaded and cannot be seen here at all; the pass above is what covers that.
vim.api.nvim_create_autocmd('PackChanged', {
  desc = 'Provide the shaderlab-ls executable for an updated checkout',
  callback = function(event)
    local data = event.data
    if data.kind == 'delete' or not same_path(data.path, provision.root) then return end
    refresh(true)
  end,
})
