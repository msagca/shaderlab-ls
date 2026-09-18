-- Filetypes, set when this repository is on 'runtimepath'. Neovim knows none of these extensions: it has no
-- shaderlab filetype of its own, does not detect *.hlsl, and gives *.shader to Godot's gdshader. A
-- vim.filetype.add() call of your own still wins over this one.
vim.filetype.add {
  extension = {
    cginc = 'hlsl',
    compute = 'hlsl',
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
