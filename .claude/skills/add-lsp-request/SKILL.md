---
name: add-lsp-request
description: Add support for a new LSP request or notification to shaderlab-ls, or a new capability of an existing one - routing, capability, implementation, test and docs. Use when implementing an LSP method the server doesn't handle yet, such as textDocument/foldingRange or inlayHint.
---

# Adding an LSP request

Each step names the place to change, so read only those parts of the large files.

1. **Declare** the entry point in `src/lsp/features.hpp`, next to the others. It takes `const Analysis &`, the
   offset when there is one, and `const FeatureContext &`, and returns `nlohmann::json`. Add `const Workspace &` only
   if it has to see other files. A large feature of its own goes in a new file such as `src/lsp/folding.cpp`, which
   must also be added to `add_executable` in `CMakeLists.txt`.
2. **Implement** it at the end of `src/lsp/features.cpp`, with its helpers in the anonymous namespace. Reuse the
   existing helpers: `toRange` and `location` for positions, `wordAt`, `unitAt` and `visibleUnits` to find the code
   at an offset, `includedFiles` for includes, and `symbolUses` for anything that follows a name. Keep offsets as
   byte `Span`s until the JSON is built, then convert with `context.encoding`.
3. **Route** it in `Server::dispatch` in `src/lsp/server.cpp`. A document request joins the shared branch that
   takes a `snapshot()`. Add the method both to that branch's condition list and to the `if` chain inside it.
4. **Advertise** the capability in the result of `Server::initialize`.
5. **Test** it in `tests/run_tests.py`: add a `print("<request>")` section with `check()` calls against a fixture
   in `tests/fixtures/`. Check that `"advertises features"` in the `initialize` section still covers the new
   capability. `fuzz.py` sends a fixed set of requests at random positions in mangled text. Add a
   `client.request(...)` call for the new method beside the others, so mangled text can't crash it.
6. **Document** it: if the request is user-visible, add it to the feature list in the root `README.md`.
7. Run `cmake --workflow Release` and format the changed C++ files with `clang-format -i`.
