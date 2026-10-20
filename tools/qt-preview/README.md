# FormaView development Preview runtime

This is an isolated Qt 6.12.0 MSVC x64 debugging plugin, not a change to the
installed Qt SDK and not part of the Release application.

Upstream: Qt qtdeclarative, tag `v6.12.0`, commit
`4332fbbb4c340463dc8cee36dda0c85d932122d8`.
`upstream-manifest.json` records the URL and original SHA-256 of every source.
`formaview-6.12.0.patch` is the complete functional difference from those sources.
The original Qt copyright and license identifiers remain in each file.
License texts are in `LICENSES/`. Local modifications: FormaView contributors, 2026.

## Fixes

1. An internal arrow signal handler executes a wrapper's nested function. Recognize
   both identities so reset does not delete a handler that was saved as external.
2. Preserved instance handlers owned by a rebuilt target leave its deletion list
   while stashed and rejoin it when restored. Handlers owned by external
   Connections retain that owner.
3. Literal changes to QML-defined readonly properties use the QML initializer
   write path. Public readonly access remains unchanged. Native C++ readonly
   properties are excluded; values changed at runtime are still preserved.

There are no observation hooks, tracing connections or runtime memory probes.
This uses private Qt APIs; do not use it with a different Qt version/compiler.

## Build and select

From a shell with CMake and VS 2022:

```powershell
cmake -S tools/qt-preview -B build/qt-preview -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=E:/Qt/6.12.0/msvc2022_64
cmake --build build/qt-preview --config RelWithDebInfo --parallel 2
```

Configure the normal FormaView development build with its existing dependency
options plus:

```text
-DFORMAVIEW_QML_PREVIEW=ON
-DFORMAVIEW_QML_PREVIEW_PLUGIN_DIR=<absolute build/qt-preview/plugins>
```

Build FormaView in RelWithDebInfo, then pass that executable to QML Preview MCP.
The host pins the plugin SHA-256 at configure time and fails explicitly if the
file changes; reconfigure the host after rebuilding the plugin.
Release does not enable QML debugging or use this override.

For a deliberate stock-plugin reproduction only, explicitly configure
`FORMAVIEW_QML_PREVIEW_ALLOW_STOCK=ON` with an empty plugin directory. The stock
6.12.0 plugin has known structural reload and readonly initializer defects.

## Regressions

Configure this standalone project with `-DFORMAVIEW_PREVIEW_BUILD_TESTS=ON`,
build RelWithDebInfo, then run:

```powershell
python tools/qt-preview/tests/run.py --build build/qt-preview --qt E:/Qt/6.12.0/msvc2022_64 --output <new-absolute-results-directory>
```

The runner verifies the actual loaded DLL path, three A→B→A cycles, unchanged
PID/root, readonly Theme and its consumer, handler counts/new function bodies,
and invalid-QML recovery separately. The offscreen protocol tests do not replace
the full application's MCP/GPU check. Compatibility fixtures intentionally include
deprecated Connections syntax; their warnings are retained.
