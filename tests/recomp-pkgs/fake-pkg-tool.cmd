@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem A stand-in for ps4-pkg-tool on Windows (tests/recomp-pkgs/run-wine.sh): PKG STAGING. "PLAN base" PKGs unpack to a tiny tree, others to an update.
findstr /C:"PLAN base" "%~1" >nul
if %errorlevel%==0 (
  mkdir "%~2\CUSA03173\sce_sys" "%~2\CUSA03173\sce_module"
  echo base> "%~2\CUSA03173\sce_sys\base.txt"
  echo old> "%~2\CUSA03173\file.txt"
  echo Progress-bytes: 500 1000
) else (
  mkdir "%~2\CUSA03173\sce_sys\changeinfo"
  echo update> "%~2\CUSA03173\sce_sys\changeinfo\bb_test_marker.txt"
  echo new> "%~2\CUSA03173\file.txt"
)
echo Progress-bytes: 1000 1000
