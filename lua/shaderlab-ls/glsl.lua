-- GLSLPROGRAM blocks in a .shader, handed to whatever language server the user runs for GLSL. shaderlab-ls formats
-- GLSL but never analyzes it, and a GLSL server cannot be attached to a .shader as it stands: it would be sent the
-- whole file, ShaderLab and HLSL included, and report every line of it.
--
-- So each GLSL server enabled for the glsl filetype gets a second client on .shader buffers, whose "server" is a
-- proxy in this process. The proxy starts the real server itself and forwards everything to it, with one change to
-- the document: every character outside a GLSL block is blanked, newlines kept, and the block keeps its columns.
-- The server sees a GLSL file that happens to have empty lines around the code, positions need no mapping in either
-- direction, and only the URI is rewritten, to one ending in .glsl, which is how GLSL servers tell a file's language.
-- Requests from outside a block are answered here, empty, so no GLSL completion is offered in ShaderLab.
local M = {}

-- Every code block ShaderLab has, so a GLSLPROGRAM written inside HLSL (in a comment, say) does not open one.
local closers = {
  CGPROGRAM = 'ENDCG', CGINCLUDE = 'ENDCG',
  HLSLPROGRAM = 'ENDHLSL', HLSLINCLUDE = 'ENDHLSL',
  GLSLPROGRAM = 'ENDGLSL', GLSLINCLUDE = 'ENDGLSL',
}

local function find_word(text, word, from)
  while true do
    local s, e = text:find(word, from, true)
    if not s then return nil end
    if not text:sub(s - 1, s - 1):match '[%w_]' and not text:sub(e + 1, e + 1):match '[%w_]' then return s, e end
    from = e + 1
  end
end

--- The GLSL blocks in a ShaderLab source, as byte ranges of their contents, keywords excluded.
--- @param text string
--- @return {[1]: integer, [2]: integer}[]
local function scan(text)
  local found, i = {}, 1
  while true do
    local s = text:find('[%a_/"]', i)
    if not s then return found end
    local c = text:sub(s, s)
    if c == '/' then
      local n = text:sub(s + 1, s + 1)
      if n == '/' then
        i = (text:find('\n', s, true) or #text) + 1
      elseif n == '*' then
        i = (select(2, text:find('*/', s + 2, true)) or #text) + 1
      else
        i = s + 1
      end
    elseif c == '"' then
      i = (text:find('"', s + 1, true) or #text) + 1
    else
      local e = select(2, text:find('^[%w_]+', s))
      local word = text:sub(s, e)
      local closer = closers[word]
      if closer then
        local cs, ce = find_word(text, closer, e + 1)
        if closer == 'ENDGLSL' then found[#found + 1] = { e + 1, (cs or #text + 1) - 1 } end
        i = (ce or #text) + 1
      else
        i = e + 1
      end
    end
  end
end

--- The text a GLSL server is shown, and where its blocks are in the server's position encoding.
local function mask(text, encoding)
  local parts, ranges, from = {}, {}, 1
  local line, line_start = 0, 1
  local function advance(to)  -- count the lines in text[from, to)
    for position in text:sub(from, to - 1):gmatch '()\n' do
      line, line_start = line + 1, from + position
    end
  end
  local function column(at) return vim.str_utfindex(text:sub(line_start, at - 1), encoding) end
  for _, block in ipairs(scan(text)) do
    local first, last = block[1], block[2]
    -- Outside the block: the newlines, then as many spaces as the block's first line is indented by in characters.
    local outside = text:sub(from, first - 1)
    local newline = outside:match '.*()\n'
    parts[#parts + 1] = newline and outside:sub(1, newline):gsub('[^\r\n]', '') or ''
    advance(first)
    local start = { line, column(first) }
    parts[#parts + 1] = (' '):rep(start[2])
    parts[#parts + 1] = text:sub(first, last)
    from = first
    advance(last + 1)
    from = last + 1
    ranges[#ranges + 1] = { start, { line, column(last + 1) } }
  end
  parts[#parts + 1] = text:sub(from):gsub('[^\r\n]', '')
  return table.concat(parts), ranges
end

local function inside(ranges, position)
  for _, range in ipairs(ranges or {}) do
    local s, e = range[1], range[2]
    local after = position.line > s[1] or (position.line == s[1] and position.character >= s[2])
    local before = position.line < e[1] or (position.line == e[1] and position.character <= e[2])
    if after and before then return true end
  end
  return false
end

-- Strings, and keys, equal to one of the URIs in `map` swapped for their counterpart, in a copy.
local function rewrite(value, map)
  if type(value) == 'string' then return map[value] or value end
  if type(value) ~= 'table' then return value end
  local copy = {}
  for k, v in pairs(value) do copy[rewrite(k, map)] = rewrite(v, map) end
  return setmetatable(copy, getmetatable(value))
end

local formatting = {
  documentFormattingProvider = true,
  documentRangeFormattingProvider = true,
  documentOnTypeFormattingProvider = true,
}
local formatting_methods = {
  ['textDocument/formatting'] = true,
  ['textDocument/rangeFormatting'] = true,
  ['textDocument/rangesFormatting'] = true,
  ['textDocument/onTypeFormatting'] = true,
}

--- A cmd for vim.lsp.start that runs `cmd` behind the proxy.
local function proxy(cmd)
  return function(dispatchers, config)
    local docs = {}  -- real URI -> block ranges
    local to, back = {}, {}  -- real URI <-> the URI the server knows it by
    local encoding = 'utf-16'
    local fake_id = 0

    local function shadow(uri)
      if not to[uri] then
        to[uri] = uri .. '.glsl'
        back[to[uri]] = uri
      end
      return to[uri]
    end

    local server = {
      notification = function(method, params)
        params = rewrite(params, back)
        if method == 'textDocument/publishDiagnostics' and docs[params.uri] then
          -- Nothing outside a block is the server's to report on, not even the empty lines it was given there.
          params.diagnostics = vim.tbl_filter(function(d) return inside(docs[params.uri], d.range.start) end,
            params.diagnostics or {})
        end
        dispatchers.notification(method, params)
      end,
      server_request = function(method, params)
        params = rewrite(params, back)
        if method == 'client/registerCapability' and params and params.registrations then
          params.registrations = vim.tbl_filter(function(r) return not formatting_methods[r.method] end,
            params.registrations)
        end
        return dispatchers.server_request(method, params)
      end,
      on_exit = dispatchers.on_exit,
      on_error = dispatchers.on_error,
    }
    local rpc = type(cmd) == 'function' and cmd(server, config)
      or vim.lsp.rpc.start(cmd, server, { cwd = config.cmd_cwd, env = config.cmd_env, detached = config.detached })

    local function show(uri, text)
      local masked, ranges = mask(text, encoding)
      docs[uri] = ranges
      return masked
    end

    return {
      request = function(method, params, callback, notify_reply)
        if method == 'initialize' then
          return rpc.request(method, params, function(err, result, id)
            local capabilities = result and result.capabilities
            if capabilities then
              encoding = capabilities.positionEncoding or 'utf-16'
              -- shaderlab-ls formats the whole file, GLSL blocks included; two formatters would take turns.
              for key in pairs(formatting) do capabilities[key] = nil end
              -- Whole documents, always: a block can only be found, and the rest blanked, in the full text.
              local sync = capabilities.textDocumentSync
              capabilities.textDocumentSync = {
                openClose = true,
                change = 1,
                save = type(sync) == 'table' and sync.save or nil,
              }
            end
            callback(err, result, id)
          end, notify_reply)
        end
        local uri = params and params.textDocument and params.textDocument.uri
        if params and params.position and docs[uri] and not inside(docs[uri], params.position) then
          fake_id = fake_id - 1
          if notify_reply then notify_reply(fake_id) end
          callback(nil, nil, fake_id)
          return true, fake_id
        end
        return rpc.request(method, rewrite(params, to), function(err, result, id)
          callback(err, rewrite(result, back), id)
        end, notify_reply)
      end,
      notify = function(method, params)
        if method == 'textDocument/didOpen' then
          local document = params.textDocument
          shadow(document.uri)
          params = rewrite(params, to)
          params.textDocument.languageId = 'glsl'
          params.textDocument.text = show(document.uri, document.text)
        elseif method == 'textDocument/didChange' then
          local uri, changes = params.textDocument.uri, params.contentChanges
          params = rewrite(params, to)
          params.contentChanges = { { text = show(uri, changes[#changes].text) } }
        elseif method == 'textDocument/didSave' and params.text then
          local uri = params.textDocument.uri
          params = rewrite(params, to)
          params.text = show(uri, params.text)
        elseif method == 'textDocument/didClose' then
          docs[params.textDocument.uri] = nil
          params = rewrite(params, to)
        else
          params = rewrite(params, to)
        end
        return rpc.notify(method, params)
      end,
      is_closing = function() return rpc.is_closing() end,
      terminate = function() rpc.terminate() end,
    }
  end
end

local function has_glsl(bufnr)
  return #scan(table.concat(vim.api.nvim_buf_get_lines(bufnr, 0, -1, false), '\n')) > 0
end

-- Found as the user has it: a table cmd has to name something executable, a function one is taken at its word.
local function runnable(config)
  if type(config.cmd) == 'function' then return true end
  return type(config.cmd) == 'table' and config.cmd[1] ~= nil and vim.fn.executable(config.cmd[1]) == 1
end

local function start(bufnr, config)
  config = vim.deepcopy(config)
  -- A name of its own: it is not the user's config, so lsp.enable() must not manage it, nor reuse it for .glsl files.
  config.name = config.name .. ' (shaderlab)'
  config.cmd = proxy(config.cmd)
  local function go()
    if not vim.api.nvim_buf_is_valid(bufnr) then return end
    vim.lsp.start(config, { bufnr = bufnr, reuse_client = config.reuse_client, _root_markers = config.root_markers })
  end
  if type(config.root_dir) == 'function' then
    config.root_dir(bufnr, function(root)
      config.root_dir = root
      vim.schedule(go)
    end)
  else
    go()
  end
end

--- Attach every GLSL server the user has enabled to a ShaderLab buffer, if it has a GLSL block.
--- @param bufnr integer
function M.attach(bufnr)
  if vim.g.shaderlab_ls_glsl == false or not vim.lsp.get_configs then return end
  if vim.bo[bufnr].buftype ~= '' or not has_glsl(bufnr) then return end
  for _, config in ipairs(vim.lsp.get_configs { enabled = true, filetype = 'glsl' }) do
    -- shaderlab-ls itself takes glsl buffers too, and it already has this one.
    if not vim.list_contains(config.filetypes, 'shaderlab') and runnable(config) then start(bufnr, config) end
  end
end

M._scan, M._mask = scan, mask  -- for the tests

return M
