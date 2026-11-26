@echo off
rem Builds the app into dist\SCCT-Launcher\  (run dist\SCCT-Launcher\SCCT-Launcher.exe - NOT anything in a "build" folder).
setlocal
cd /d "%~dp0"

rem Prefer a mature Python (3.13/3.12/3.11); fall back to whatever "python" is.
set "PYV="
for %%V in (3.13 3.12 3.11) do if not defined PYV (py -%%V -c "pass" >nul 2>nul && set "PYV=%%V")
if not exist ".build-venv\Scripts\python.exe" (
  if defined PYV (py -%PYV% -m venv .build-venv) else (python -m venv .build-venv)
)
if not exist ".build-venv\Scripts\python.exe" (echo Could not create a Python environment. & pause & exit /b 1)

".build-venv\Scripts\python.exe" -m pip install -q -r requirements.txt pyinstaller || (echo Dependency install failed. & pause & exit /b 1)

if exist dist\SCCT-Launcher rmdir /s /q dist\SCCT-Launcher
".build-venv\Scripts\pyinstaller.exe" --noconfirm --clean --onedir --windowed ^
  --name SCCT-Launcher --icon "%~dp0static\assets\scct_icon.ico" ^
  --workpath "%TEMP%\scct-pyi-build" --distpath "%~dp0dist" ^
  --add-data "%~dp0static;static" --add-data "%~dp0mod;mod" ^
  --add-binary "%~dp0scct_p2p_x64.dll;." ^
  --hidden-import=clr_loader --hidden-import=pythonnet --hidden-import=webview ^
  start_launcher.py || (echo Build failed. & pause & exit /b 1)

if not exist "dist\SCCT-Launcher\_internal\" (echo Build output incomplete. & pause & exit /b 1)

rem Optional installer + delta updates (needs: dotnet tool install -g vpk)
where vpk >nul 2>nul && vpk pack --packId EchelonRe.SCCTCoopLauncher --packVersion 2.2.0 --runtime win-x64 --packDir dist\SCCT-Launcher --mainExe SCCT-Launcher.exe --icon static\assets\scct_icon.ico --outputDir Releases

echo.
echo Built: %~dp0dist\SCCT-Launcher\SCCT-Launcher.exe
start "" explorer "%~dp0dist\SCCT-Launcher"
