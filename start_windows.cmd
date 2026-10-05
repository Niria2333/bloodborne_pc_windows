@rem Windows port modifications by yaonikaixin999999, 2026-10-05.
@rem SPDX-License-Identifier: GPL-2.0-or-later
@echo off
chcp 65001 >nul
cd /d "%~dp0"
python run_windows.py --gui
if errorlevel 1 pause
