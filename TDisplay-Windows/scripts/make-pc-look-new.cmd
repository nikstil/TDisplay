@echo off
rem Double-click this to run make-pc-look-new.ps1 with the administrator rights it needs.
powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \"%~dp0make-pc-look-new.ps1\"'"
