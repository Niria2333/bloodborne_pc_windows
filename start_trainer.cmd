@rem Windows port modifications by yaonikaixin999999, 2026-10-06.
@rem SPDX-License-Identifier: GPL-2.0-or-later
@echo off
chcp 65001 >nul
cd /d "%~dp0"
where pythonw >nul 2>nul
if not errorlevel 1 (
    start "" pythonw "%~dp0bloodborne_trainer.py" --gui
    exit /b
)
python "%~dp0bloodborne_trainer.py" --gui
if errorlevel 1 pause
