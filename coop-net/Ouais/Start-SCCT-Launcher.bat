@echo off
cd /d "%~dp0"
if not exist ".venv\Scripts\pythonw.exe" (
  echo First run: setting up...
  py -3 -m venv .venv 2>nul || python -m venv .venv
  ".venv\Scripts\python.exe" -m pip install -q -r requirements.txt || (echo Dependency install failed. & pause & exit /b 1)
)
start "" ".venv\Scripts\pythonw.exe" start_launcher.py
