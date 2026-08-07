# Tokenometer

原生 Windows 桌面小组件，目标是用 WinUI 3、C++/WinRT、Direct3D 11、Windows.Graphics.Capture 与 HLSL 复现参考视频中的实时液态玻璃效果。

## 构建

需要 Visual Studio 2026 Build Tools（C++ WinUI 工具与 Windows 11 SDK 10.0.26100）。

```powershell
.\build.cmd
```

输出位于 `src\Tokenometer\bin\x64\Debug\Tokenometer.exe`。
