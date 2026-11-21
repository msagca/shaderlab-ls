# Installing shaderlab-ls

Download the archive for your platform from the
[releases](https://github.com/msagca/shaderlab-ls/releases), unpack it and put `shaderlab-ls` on `PATH`:

| file | for |
| --- | --- |
| `shaderlab-ls-<version>-windows-x64.zip` | Windows 10/11, x86-64 |
| `shaderlab-ls-<version>-linux-x64.tar.gz` | Linux, x86-64, glibc 2.39 or newer |

The Neovim plugin fetches the server for itself (see [../neovim/README.md](../neovim/README.md)).

Optional, and found by themselves when installed:

- [clang-format](https://clang.llvm.org/docs/ClangFormat.html) on `PATH`, for formatting.
- A Unity editor installed through Unity Hub, for Unity's built-in includes and packages, and its DXC.
- On Linux, [DXC](https://github.com/microsoft/DirectXShaderCompiler/releases) with its `lib` folder on
  `LD_LIBRARY_PATH`, for HLSL diagnostics.

To build it instead, with CMake 3.25+ and a C++20 compiler, on Windows:

```
build.cmd
```

On Linux:

```
./build.sh
```

The executable ends up in `build/`.
