@echo off
REM Bake every shipped preset into out\. ASCII only, no emoji.
setlocal
cd /d "%~dp0"

where uv >nul 2>nul
if errorlevel 1 (
    echo ERROR: uv is not on PATH. Install it from https://docs.astral.sh/uv/ and retry.
    exit /b 1
)

uv sync || exit /b 1
uv run bake.py --all %*
endlocal
