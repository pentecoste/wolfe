# Builds Wolfe's installer: dist\Wolfe-Setup-<version>.exe
#
#   powershell -ExecutionPolicy Bypass -File installer\build.ps1
#
# Needs MinGW-w64 (C:\msys64\mingw64), CMake and Inno Setup 6. The program is
# built in Release into build\release; usbip-win2's installer is downloaded
# once into installer\cache and checked against its published SHA-256.

param(
    [string]$MinGW = "C:\msys64\mingw64\bin",
    [string]$CMakeBin = "D:\CMake\bin"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$env:PATH = "$MinGW;$CMakeBin;" + $env:PATH

# The version is the project's (CMakeLists.txt).
$project = Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(Wolfe VERSION ([0-9.]+)'
if (-not $project) { throw "No version in CMakeLists.txt" }
$version = $project.Matches[0].Groups[1].Value

# usbip-win2: the version wolfe.iss names, and the digest GitHub publishes for it.
$usbipVersion = "0.9.8.1"
$usbipSha256 = "38cad6d4432b52d5bb9409d9ad03b72fdffc4ada4cd3a48fbeca1a2752a8518a"
$usbipFile = "USBip-$usbipVersion-x64.exe"
$cache = "$root\installer\cache"
New-Item -ItemType Directory -Force $cache | Out-Null
$usbip = "$cache\$usbipFile"
if (-not (Test-Path $usbip) -or (Get-FileHash $usbip -Algorithm SHA256).Hash -ne $usbipSha256) {
    Write-Host "Downloading $usbipFile"
    Invoke-WebRequest -UseBasicParsing -OutFile $usbip "https://github.com/vadimgrn/usbip-win2/releases/download/v.$usbipVersion/$usbipFile"
    if ((Get-FileHash $usbip -Algorithm SHA256).Hash -ne $usbipSha256) {
        Remove-Item $usbip
        throw "$usbipFile does not match its published SHA-256"
    }
}

# The program, in Release.
$build = "$root\build\release"
cmake -S $root -B $build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $build --target wolfe wolfe-cli -j 8
if ($LASTEXITCODE) { throw "cmake build failed" }

# The installer.
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
& $iscc /Qp "/DAppVersion=$version" "/DBuildDir=$build" "/DOutputDir=$root\dist" "$root\installer\wolfe.iss"
if ($LASTEXITCODE) { throw "Inno Setup failed" }
Write-Host "Built $root\dist\Wolfe-Setup-$version.exe"
