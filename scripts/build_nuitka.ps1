Param(
    [string]$PythonExe = "",
    [string]$OutputName = "XmlMergeTool"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-ManifestTool {
    $cmd = Get-Command mt.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    if (-not (Test-Path $kitsRoot)) {
        return $null
    }

    $versions = Get-ChildItem -Path $kitsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending

    foreach ($version in $versions) {
        $candidate = Join-Path $version.FullName "x64\mt.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = (Resolve-Path (Join-Path $scriptRoot "..")).Path
Set-Location $projectRoot

if ([string]::IsNullOrWhiteSpace($PythonExe)) {
    $venvPython = Join-Path $projectRoot ".venv\Scripts\python.exe"
    if (Test-Path $venvPython) {
        $PythonExe = $venvPython
    }
    else {
        $PythonExe = "python"
    }
}

Write-Output "Using Python: $PythonExe"

$entry = "src\xml_merge_tool\main.py"
$outputDir = "dist"
$outputFile = Join-Path $outputDir "$OutputName.exe"
$manifestFile = Join-Path $projectRoot "scripts\windows\app.manifest"
$cacheDir = Join-Path $projectRoot "tmp\nuitka-cache"
New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
$env:NUITKA_CACHE_DIR = $cacheDir
$srcPath = Join-Path $projectRoot "src"
if ([string]::IsNullOrWhiteSpace($env:PYTHONPATH)) {
    $env:PYTHONPATH = $srcPath
}
elseif (-not ($env:PYTHONPATH.Split(';') -contains $srcPath)) {
    $env:PYTHONPATH = "$srcPath;$($env:PYTHONPATH)"
}

$nuitkaArgs = @(
    "-m", "nuitka",
    "--onefile",
    "--enable-plugin=pyqt6",
    "--include-qt-plugins=iconengines,imageformats,platforms,styles",
    "--noinclude-qt-plugins=tls",
    "--noinclude-dlls=PyQt6/Qt6/plugins/imageformats/qpdf.*",
    "--noinclude-dlls=qt6pdf.*",
    "--noinclude-dlls=qt6network.*",
    "--windows-console-mode=disable",
    "--windows-icon-from-ico=resources/image/logo.ico",
    "--output-dir=$outputDir",
    "--output-filename=$OutputName.exe",
    "--include-data-dir=resources=resources",
    "--include-package=xml_merge_tool",
    "--include-package=openpyxl",
    "--include-package=robot",
    "--include-package-data=robot",
    "--include-module=robot.rebot",
    "--msvc=latest",
    "--assume-yes-for-downloads",
    $entry
)

& $PythonExe @nuitkaArgs
if ($LASTEXITCODE -ne 0) {
    throw "Nuitka build failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $outputFile)) {
    throw "Build completed but output not found: $outputFile"
}

if (-not (Test-Path $manifestFile)) {
    throw "Manifest file not found: $manifestFile"
}

$mtExe = Find-ManifestTool
if ([string]::IsNullOrWhiteSpace($mtExe)) {
    throw "Unable to locate mt.exe for manifest injection."
}

Write-Output "Injecting PerMonitorV2 manifest: $manifestFile"
& $mtExe "-manifest" $manifestFile "-outputresource:$outputFile;#1"
if ($LASTEXITCODE -ne 0) {
    throw "Manifest injection failed with exit code $LASTEXITCODE"
}

Write-Output "Nuitka build complete: $outputFile"
