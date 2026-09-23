param(
    [string]$OutputName = 'Rift-Barebones',
    [switch]$DisableLto,
    [string]$BuildDirectory = 'build-vs'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($OutputName -notmatch '^[A-Za-z0-9_-]+$') { throw 'OutputName must contain only letters, numbers, underscores or hyphens.' }
if ([System.IO.Path]::IsPathRooted($BuildDirectory) -or $BuildDirectory -match '(^|[\\/])\.\.([\\/]|$)') { throw 'BuildDirectory must stay inside the repository.' }

$buildRoot = $PSScriptRoot
$buildPath = Join-Path $buildRoot $BuildDirectory
$gitCommand = Get-Command git -ErrorAction SilentlyContinue
if (-not $gitCommand) { throw 'Git is required. Install Git for Windows and reopen PowerShell.' }

$vcpkgToolchain = Join-Path $buildRoot 'dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake'
if (-not (Test-Path -LiteralPath $vcpkgToolchain)) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot '.git'))) { throw 'This source archive does not contain Git submodules. Clone the repository with git clone --recursive, then run this script again.' }
    & $gitCommand.Source -C $buildRoot submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw 'Dependency download failed while initializing Git submodules.' }
}
if (-not (Test-Path -LiteralPath $vcpkgToolchain)) { throw 'The vcpkg submodule is incomplete. Run git submodule update --init --recursive.' }

$vcpkgExecutable = Join-Path $buildRoot 'dependencies/vcpkg/vcpkg.exe'
if (-not (Test-Path -LiteralPath $vcpkgExecutable)) {
    $bootstrap = Join-Path $buildRoot 'dependencies/vcpkg/bootstrap-vcpkg.bat'
    if (-not (Test-Path -LiteralPath $bootstrap)) { throw 'vcpkg bootstrap script was not found. Reinitialize the Git submodules.' }
    & $bootstrap -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed.' }
}

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmakePath = $cmakeCommand.Source
} else {
    $cmakeCandidates = @(
        "$env:ProgramFiles/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
        "$env:ProgramFiles/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
        "$env:ProgramFiles/Microsoft Visual Studio/2022/Enterprise/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
        "$env:ProgramFiles/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
    )
    $cmakePath = $cmakeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $cmakePath) { throw 'CMake was not found. Install Visual Studio 2022 with Desktop development with C++ and C++ CMake tools for Windows.' }

& $cmakePath -S $buildRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64 `
    -DVCPKG_MANIFEST_INSTALL=ON -DVCPKG_TARGET_TRIPLET=x64-windows-static `
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5" `
    -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON "-DCEMU_EXECUTABLE_NAME=$OutputName" `
    "-DCEMU_ENABLE_LTO=$(-not $DisableLto)"
if ($LASTEXITCODE -ne 0) { throw 'Rift configuration failed.' }

& $cmakePath --build $buildPath --target CemuBin --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Rift compilation failed.' }

$executable = Join-Path $buildRoot "bin/$OutputName.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw "The build completed but $OutputName.exe was not found in the bin folder." }
Write-Host "Built $executable"
