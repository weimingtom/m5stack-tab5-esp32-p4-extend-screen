<#
.SYNOPSIS
    Runs ESP-IDF (v5.5.6, installed by the ESP-IDF Installer / EIM) with the
    arguments given on the command line, e.g.

        .\tools\idf.ps1 set-target esp32p4
        .\tools\idf.ps1 build
        .\tools\idf.ps1 -B build-wifi -D SDKCONFIG=sdkconfig.wifi build
        .\tools\idf.ps1 -p COM5 flash monitor

.DESCRIPTION
    Every pwsh invocation on this machine starts a fresh process, so the ESP-IDF
    environment has to be re-created each time. This script does that and nothing
    else, so that `idf.py` behaves exactly like it does inside an
    "ESP-IDF PowerShell" shortcut.

    The arguments are taken from $args verbatim (no param() block) so that
    idf.py options such as `-B`, `-D` or `-p` are forwarded unchanged.
#>

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force -ErrorAction SilentlyContinue

$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.6'
$env:IDF_TOOLS_PATH = 'D:\Espressif'

$pythonDir = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts'
$gitDir = 'D:\Espressif\tools\idf-git\2.44.0\cmd'
$env:PATH = "$pythonDir;$gitDir;$env:IDF_TOOLS_PATH;$env:PATH"
$env:PYTHONPATH = $null
$env:PYTHONHOME = $null
$env:PYTHONNOUSERSITE = 'True'

# `activate.py` reports progress on stderr; keep it from being treated as a
# terminating error, and keep the export's own output quiet.
$ErrorActionPreference = 'Continue'
& "$env:IDF_PATH\export.ps1" 2>$null
$ErrorActionPreference = 'Stop'
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw 'ESP-IDF export failed: idf.py is not available on PATH.'
}

& idf.py @args
exit $LASTEXITCODE
