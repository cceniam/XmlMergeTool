Param(
    [string]$BuildDir = "build/windows",
    [string]$OutputDir = "dist/native",
    [string]$QtRoot = $env:QT_ROOT_DIR,
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$SkipTests
)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $projectRoot
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
if ([string]::IsNullOrEmpty($VcpkgRoot)) {
    $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
}
if ([string]::IsNullOrEmpty($VcpkgRoot)) { throw "Set VCPKG_ROOT to a bootstrapped vcpkg checkout." }
# Hosted runners can contain an older/shallow registry checkout. Fetch our
# immutable baseline before manifest resolution, without changing that checkout.
$manifest = Get-Content (Join-Path $projectRoot "cpp/vcpkg.json") -Raw | ConvertFrom-Json
$baseline = $manifest.'builtin-baseline'
& git -C $VcpkgRoot cat-file -e "${baseline}:versions/baseline.json" 2>$null
if ($LASTEXITCODE -ne 0) {
    Invoke-Checked "git" @("-C", $VcpkgRoot, "fetch", "origin", $baseline)
}
$build = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDir))
$output = [IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDir))
$configure = @("-S", "cpp", "-B", $build, "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",
    "-DVCPKG_TARGET_TRIPLET=x64-windows", "-DVCPKG_APPLOCAL_DEPS=ON",
    "-DXMLMERGETOOL_BUILD_APP=ON", "-DBUILD_TESTING=ON")
if ($QtRoot) { $configure += "-DCMAKE_PREFIX_PATH=$QtRoot" }
Invoke-Checked "cmake" $configure
Invoke-Checked "cmake" @("--build", $build, "--config", "Release", "--parallel", "4")
if (-not $SkipTests) {
    Invoke-Checked "ctest" @("--test-dir", $build, "-C", "Release", "--output-on-failure")
}
$stage = Join-Path $build "package-stage"
Invoke-Checked "cmake" @("-E", "rm", "-rf", $stage)
Invoke-Checked "cmake" @("--install", $build, "--config", "Release", "--component", "Runtime", "--prefix", $stage)
$payload = Join-Path $build "payload.zip"
Push-Location $stage
try {
    # Qt places platforms/imageformats under plugins/, alongside bin/.
    $entries = @(Get-ChildItem -Name)
    Invoke-Checked "cmake" (@("-E", "tar", "cf", $payload, "--format=zip", "--") + $entries)
}
finally { Pop-Location }
$launcher = Join-Path $build "launcher"
Invoke-Checked "cmake" @("-S", "cpp/launcher", "-B", $launcher,
    "-G", "Visual Studio 17 2022", "-A", "x64", "-DPAYLOAD_ZIP=$payload")
Invoke-Checked "cmake" @("--build", $launcher, "--config", "Release", "--parallel", "4")
New-Item -ItemType Directory -Force $output | Out-Null
Copy-Item (Join-Path $launcher "Release/XmlMergeTool.exe") (Join-Path $output "XmlMergeTool.exe") -Force
Write-Output "Built: $(Join-Path $output 'XmlMergeTool.exe')"
