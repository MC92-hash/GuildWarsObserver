@echo off
cd /d "%~dp0..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc /std:c++20 /W0 /MD /I SourceFiles /I DearImGui /I DearImGui/backends tools/navigation_preview/preview.cpp DearImGui/imgui.cpp DearImGui/imgui_draw.cpp DearImGui/imgui_tables.cpp DearImGui/imgui_widgets.cpp DearImGui/imgui_impl_dx11.cpp /Fo:tools/navigation_preview/ /Fe:tools/navigation_preview/preview.exe /link d3d11.lib d3dcompiler.lib dxgi.lib windowscodecs.lib ole32.lib
if errorlevel 1 exit /b 1
tools\navigation_preview\preview.exe

