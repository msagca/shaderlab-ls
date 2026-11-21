# shaderlab-ls

A C++20 language server for Unity ShaderLab and HLSL, plus a Neovim plugin that installs and configures it.

## Layout

| Path | What |
| --- | --- |
| `src/` | The server. See `src/CLAUDE.md`. |
| `tests/` | End-to-end, fuzz and plugin tests. See `tests/CLAUDE.md`. |
| `lua/`, `lsp/`, `plugin/`, `ftplugin/` | The Neovim plugin. See `lua/CLAUDE.md`. |
| `docs/` | User docs: install, usage (CLI and settings), Neovim. |
| `.github/workflows/` | `build.yml` (CI), `release.yml` and `tag.yml` (releases). |
| `build.sh`, `build.cmd` | Configure and build only; the plugin runs them to build a checkout. |

## Build and test

```sh
cmake --workflow Release
```

This configures, builds `build/shaderlab-ls(.exe)` and runs every test (about two minutes). A failing test prints its
output. If configuring fails with a compiler path that no longer exists, add `--fresh`.

To reproduce a problem without a client, run the built server on a file:

```sh
build/shaderlab-ls --check path/to/file.shader
```

```sh
build/shaderlab-ls --format path/to/file.shader
```

## Conventions

- Commit subjects are one plain imperative sentence with no prefix, such as "Find the methods of a struct".
- Comments explain why, in full sentences. Keep them accurate when changing the code they describe.
- A user-visible change updates the matching page under `docs/`: CLI flags and settings in `docs/usage/`.
- The version lives only in `project(... VERSION ...)` in `CMakeLists.txt`. Use the `release` skill to release.
