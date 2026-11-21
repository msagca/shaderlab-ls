# LSP layer

## server.cpp

- `Server::dispatch` is one chain of `if (method == ...)`. Document requests share one branch that takes a
  `snapshot()` of the document and calls into `features.hpp`.
- `Server::initialize` negotiates the position encoding and snippet support, reads `initializationOptions`, and
  returns the capabilities. A new request needs its capability there.
- `Server::applySettings` reads every setting, from `initializationOptions` and `didChangeConfiguration`.
- `Server::codeActions` and `Server::executeCommand` handle the quick fixes and the variant (keyword) commands.
- `Server::formatEdits` formats and diffs the result into edits.

## features.cpp

The file is long, so look up the function you need instead of reading it all. The public entry points are at the
end, and each calls helpers in the anonymous namespace above it:

| Request | Entry point | Main helpers |
| --- | --- | --- |
| completion | `completion` | `shaderLabCompletion`, `hlslCompletion`, `addFunctions`, `addMembers` |
| hover | `hover` | `shaderLabHover`, `hlslHover`, `declMarkdown`, `localMarkdown` |
| definition | `definition` | `findLocalDecl`, `findExternalDecl`, `bestDecl` (picks a declaration by its `#if` guards), `memberDecl`, `localDecl` |
| signature help | `signatureHelp` | `signature` |
| symbols | `documentSymbols`, `workspaceSymbols` | `declSymbols`, `fuzzyMatch` |
| references, highlights | `references`, `documentHighlights` | `symbolUses`, `propertyUses`, `crossFileUses`, `reachesWorkspace` |
| rename | `prepareRename`, `rename` | `renamable`, `renameEdits`, `nameInUse`, `textureCompanions` |

Member access (`a.b.c`) is resolved by `chainBefore` and the `TypeResolver`. Locals and parameters come from
`localBindings` in `hlsl/scopes.cpp`.

A request that is understood but can't be done safely throws `RequestRefused` with the reason for the user. Don't
return a guess.

## Other files

- `semantic_tokens.cpp`: the legend and the whole-document tokens. GLSL gets only comments, strings, numbers and
  directives.
- `workspace.cpp`: the files that references and renames reach, with open documents standing in for their files.
- `transport.cpp`: `Content-Length` framing over stdio.
