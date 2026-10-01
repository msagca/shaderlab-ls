-- Neovim's runtime has no ftplugin for this filetype, so 'commentstring' would stay empty and gc would refuse to
-- comment anything. Line comments, as Unity's own shaders use; inside a .shader's HLSL blocks gc goes by hlsl's.
vim.bo.commentstring = '// %s'

vim.b.undo_ftplugin = (vim.b.undo_ftplugin or '') .. '\n setl commentstring<'
