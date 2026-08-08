# Third-party notices

Tokenometer's self-contained Windows output redistributes Microsoft components.
`package.ps1` reads the exact resolved NuGet graph and copies every license and
notice file included in those packages without modification to
`LICENSES/<package-id>/<version>/`. If a build-only package contains only a
license URL, its original NuGet manifest is preserved instead. The archive also
contains `DEPENDENCIES.txt`, which is the authoritative package/version
inventory for that build.

The v0.1 graph includes Microsoft Windows App SDK and its Base, DWrite,
Foundation, Interactive Experiences, ML, Runtime, Widgets, WinUI, and AI
components; Microsoft Windows AI Machine Learning; Microsoft Edge WebView2;
Microsoft Windows C++/WinRT; and Microsoft Windows SDK build tooling. Some of
these packages have distinct license or notice files even when they arrive as
transitive dependencies, so the release archive preserves all of them rather
than relying on the Windows App SDK meta-package notice alone.

Those original package files, rather than this summary, govern redistribution
of the corresponding binaries. Tokenometer itself is licensed under the
repository's MIT `LICENSE`.

## LiquidGlass design reference

The liquid-glass shader design is informed by
[OverShifted/LiquidGlass](https://github.com/OverShifted/LiquidGlass),
Copyright (c) 2026 Sepehr Kalanaki, licensed under the MIT License:

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
