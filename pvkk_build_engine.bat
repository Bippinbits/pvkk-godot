@echo off
rem Builds the PVKK engine fork for Windows the way .github/workflows/pvkk_all.yml does
rem (editor, template_debug, template_release: MSVC, x86_64, debug_symbols=yes, d3d12 when its
rem SDK is available) and copies the results into the game repo's engine\ folder under the names
rem engine\update_engine.py uses. The Linux and macOS editors stay as CI built them.
rem
rem Usage:  pvkk_build_engine.bat [pvkk repo folder]
rem         default folder: the pvkk checkout next to this one (C:\dev\pvkk for C:\dev\pvkk-godot)
rem
rem Nothing in engine\ changes unless all three builds succeed. Commit and the Sentry symbol
rem upload stay manual.

setlocal EnableExtensions

set "GODOT_DIR=%~dp0"
if "%GODOT_DIR:~-1%"=="\" set "GODOT_DIR=%GODOT_DIR:~0,-1%"
set "PVKK_DIR=%~1"
if "%PVKK_DIR%"=="" set "PVKK_DIR=%GODOT_DIR%\..\pvkk"
for %%I in ("%PVKK_DIR%") do set "PVKK_DIR=%%~fI"
set "ENGINE_DIR=%PVKK_DIR%\engine"
set "BIN=%GODOT_DIR%\bin"
set "VERSION_TXT=%ENGINE_DIR%\version.txt"

if not exist "%ENGINE_DIR%\update_engine.py" (
	echo No engine\update_engine.py under "%PVKK_DIR%". Pass the pvkk repo folder as the first argument.
	exit /b 1
)
where scons >nul 2>nul
if errorlevel 1 (
	echo scons is not on PATH.
	exit /b 1
)

pushd "%GODOT_DIR%"
echo Godot source: %GODOT_DIR%
echo Game engine:  %ENGINE_DIR%

rem --- D3D12 SDK: CI fetches it and builds without D3D12 if that fails. Fetch only when missing.
set "D3D12=no"
if exist "%LOCALAPPDATA%\Godot\build_deps\mesa*" set "D3D12=yes"
if "%D3D12%"=="no" (
	echo.
	echo D3D12 build deps missing, fetching them like CI does...
	python misc\scripts\install_d3d12_sdk_windows.py
	if not errorlevel 1 set "D3D12=yes"
)
if "%D3D12%"=="no" echo D3D12 deps unavailable: building with d3d12=no, as CI does when the fetch fails.

rem --- Build: the same three Windows targets and flags as the CI matrix.
for %%T in (editor template_debug template_release) do (
	echo.
	echo === scons platform=windows target=%%T arch=x86_64 debug_symbols=yes d3d12=%D3D12% ===
	call scons platform=windows target=%%T arch=x86_64 debug_symbols=yes d3d12=%D3D12%
	if errorlevel 1 (
		echo Build of %%T failed. engine\ was not touched.
		goto :fail
	)
)

rem --- Validate every output before touching engine\, like update_engine.py.
for %%T in (editor template_debug template_release) do (
	call :check "%BIN%\godot.windows.%%T.x86_64.exe" || goto :fail
	call :check "%BIN%\godot.windows.%%T.x86_64.pdb" || goto :fail
)

rem An open PVKK editor locks godot.exe; stop here rather than leave a half-updated engine\.
if exist "%ENGINE_DIR%\godot.exe" (
	2>nul (>>"%ENGINE_DIR%\godot.exe" echo off) || (
		echo engine\godot.exe is in use. Close the PVKK editor and run this again: scons has nothing left to build, so it goes straight to the copy.
		goto :fail
	)
)

rem --- Copy, same names and folders as update_engine.py's ARTIFACTS (PDBs are gitignored there).
echo.
echo Replacing engine files...
if not exist "%ENGINE_DIR%\export_templates" mkdir "%ENGINE_DIR%\export_templates"
if not exist "%ENGINE_DIR%\debug_symbols" mkdir "%ENGINE_DIR%\debug_symbols"
call :put godot.windows.editor.x86_64.exe godot.exe || goto :fail
call :put godot.windows.editor.x86_64.pdb godot.pdb || goto :fail
call :put godot.windows.template_debug.x86_64.exe export_templates\windows_debug_x86_64.exe || goto :fail
call :put godot.windows.template_debug.x86_64.pdb debug_symbols\windows_debug_x86_64.pdb || goto :fail
call :put godot.windows.template_release.x86_64.exe export_templates\windows_release_x86_64.exe || goto :fail
call :put godot.windows.template_release.x86_64.pdb debug_symbols\windows_release_x86_64.pdb || goto :fail

rem --- version.txt: rewrite the two header lines the way update_engine.py does and keep the rest.
rem Line 1 is identical in format; line 2 holds the CI run URL there, a local-build note here.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
	"$ErrorActionPreference = 'Stop'; [Console]::OutputEncoding = [Text.Encoding]::UTF8;" ^
	"$g = $env:GODOT_DIR; $py = [IO.File]::ReadAllText((Join-Path $g 'version.py'));" ^
	"$ver = (('major', 'minor', 'patch') | ForEach-Object { [regex]::Match($py, '(?m)^' + $_ + '\s*=\s*(\d+)').Groups[1].Value }) -join '.';" ^
	"$sha = git -C $g rev-parse --short=7 HEAD; $subject = git -C $g log -1 --format=%%s; $branch = git -C $g rev-parse --abbrev-ref HEAD;" ^
	"$note = 'local build - ' + $branch + ' - ' + (Get-Date -Format 'yyyy-MM-dd HH:mm') + ' - Windows only, Linux and macOS binaries unchanged';" ^
	"if (git -C $g status --porcelain --untracked-files=no) { $note += ' - built with uncommitted changes' };" ^
	"$head = @(($ver + ' fork - https://github.com/Bippinbits/pvkk-godot - ' + $sha + ' - ' + $subject), $note);" ^
	"$rest = @([IO.File]::ReadAllLines($env:VERSION_TXT) | Select-Object -Skip 2);" ^
	"[IO.File]::WriteAllText($env:VERSION_TXT, (($head + $rest) -join [char]10));" ^
	"Write-Host ('  version.txt -> ' + $head[0])"
if errorlevel 1 (
	echo The binaries are in place but version.txt was not updated.
	goto :fail
)

echo.
echo Done. engine\ now holds this local Windows build. Review with: git -C "%PVKK_DIR%" status engine
popd
endlocal
exit /b 0

:check
if not exist "%~1" (
	echo Missing build output %~1
	exit /b 1
)
if %~z1 LSS 1048576 (
	echo %~nx1 is suspiciously small, aborting.
	exit /b 1
)
exit /b 0

:put
copy /Y /B "%BIN%\%~1" "%ENGINE_DIR%\%~2" >nul
if errorlevel 1 (
	echo Copy to engine\%~2 failed.
	exit /b 1
)
echo   %~2
exit /b 0

:fail
popd
endlocal
exit /b 1
