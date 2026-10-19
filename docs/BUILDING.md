# Building Artist Tools

[Back to README](../README.md)

## Requirements

- UNIGINE SDK **2.22** (float or double, x64), with the engine and editor headers and libraries.
- Qt **6.5.3** — the version used to build the 2.22 Editor: `msvc2019_64` on Windows, `gcc_64` on Linux.
- Windows: MSVC (Visual Studio 2022 or later), CMake 3.19+, Ninja.
- Linux: gcc 11+, CMake 3.19+, Ninja.

## Building

The build needs the UNIGINE engine and editor headers and libraries. The default layout is:

```
<project>/source/plugins/zloy_pingvin/artist_tool/   <- this repository
```

Run the commands below from the repository root. For a checkout elsewhere, add `-DUNIGINE_SDK_PATH=<project-or-sdk-root>` to the configure command.

Build **out of tree**: keep the build directory outside this folder so that build files cannot get into a package of the plugin.

Windows (from a Visual Studio x64 developer prompt):

```bat
cmake -S . -B ../../../../../artist_tool_build/win_x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.5.3/msvc2019_64
cmake --build ../../../../../artist_tool_build/win_x64
```

Linux:

```sh
cmake -S . -B ~/artist_tool_build/x64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/qt/6.5.3/gcc_64
cmake --build ~/artist_tool_build/x64
```

Options:

- `-DUNIGINE_DOUBLE=1` — link against the double-precision engine and editor libraries.
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo` — produces the `d`-suffixed binary linked against the `d`-suffixed SDK libraries (the UNIGINE Store "debug" convention: optimized, with debug info).
- `-DUNIGINE_SDK_PATH=<path>` — the project or SDK root to build against, if the repository is not inside a project.
- `-DARTIST_TOOL_OUTPUT_DIR=<path>` — where to write the binary instead of the project's plugins folder.
- `-DARTIST_TOOL_DEV_PAGES=1` — adds two reference pages for plugin development to the **Debug** tab: a gallery of UI controls and a browser for the editor's built-in icons.

The binary is written to `<project>/bin/plugins/zloy_pingvin/artist_tool/`, and the editor loads it at startup. A Release editor loads only a Release plugin; a Debug editor loads only a Debug plugin. `build_linux.sh` builds all four Linux variants.
