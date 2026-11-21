# Tests

CMake registers three tests, and `cmake --workflow Release` runs them all:

| Test | Script | What |
| --- | --- | --- |
| `lsp` | `run_tests.py` | Drives the server over stdio as an editor would, against `fixtures/`. |
| `fuzz` | `fuzz.py` | Mangled shaders and random requests. Fails on a crash, hang or error response. |
| `plugin` | `run_plugin_tests.py` + `plugin_cases.lua` | Headless Neovim against a fake checkout. Skipped without Neovim 0.12+. |

## run_tests.py

`main()` is one long sequence of sections. Each starts with `print("<name>")` and runs `check(condition,
description)` calls, which record failures and keep going. Find the section you need with a search on its `print(`
instead of reading the file.

Helpers:
- `open_doc(client, path)` opens a fixture and returns its URI.
- `diags(client, uri)` waits for the last diagnostics pass.
- `has(diagnostics, line, fragment, severity, source)` matches a diagnostic. `line` is 1-based, as in the editor.
- `labels(result)` gives the labels of a completion result.
- `skip(description, why)` marks a check that needs something missing here, such as an HLSL compiler.

The module-level `compiler` is `"fxc"`, `"dxc"` or `None`, depending on what the server found. Guard compile checks
with it, and accept both compilers' wording.

Fixtures may be checked out with CRLF. A test that depends on line endings builds its text with LF itself. Editors
strip trailing spaces from files, so completion positions after a space are added in the test, not in the fixture.

## Plugin tests

Each case in `plugin_cases.lua` is a function named by `$CASE`. It logs events to `$LOG`, one per line, and
`run_plugin_tests.py` asserts on them. The fake build script in the fixture stands in for CMake and never compiles
anything. Downloading releases is always off.
