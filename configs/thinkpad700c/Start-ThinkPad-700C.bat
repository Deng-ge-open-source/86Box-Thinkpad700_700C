@echo off
setlocal
cd /d "%~dp0"
start "" "%~dp086Box.exe" -N -W -F -R "%~dp0runtime\roms" -P "%~dp0runtime\ThinkPad 700C"
