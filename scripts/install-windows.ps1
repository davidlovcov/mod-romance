# Builds AzerothCore (Playerbot fork) + mod-playerbots + mod-true-companion on Windows 11.
# Requires: Visual Studio 2022 (C++), CMake, Git, MySQL 8, Boost (BOOST_ROOT), OpenSSL.
#   powershell -ExecutionPolicy Bypass -File scripts\install-windows.ps1 [-Source C:\azerothcore] [-Pinned]
param(
    [string]$Source = "",
    [string]$Config = "RelWithDebInfo",
    [switch]$Pinned
)
$ErrorActionPreference = "Stop"
$Module = Split-Path -Parent $PSScriptRoot
if (-not $Source) {
    if ((Split-Path -Leaf (Split-Path -Parent $Module)) -eq "modules") { $Source = Split-Path -Parent (Split-Path -Parent $Module) }
    else { $Source = "C:\azerothcore" }
}
$AcCommit = "93333d6a9eb00fdf0179adde30da3c17618207d6"
$PbCommit = "05a1f6fceb8a32aa879b13f5f48934c6e46a774c"

if (-not (Test-Path $Source)) { git clone https://github.com/mod-playerbots/azerothcore-wotlk.git --branch=Playerbot $Source }
if (-not (Test-Path "$Source\modules\mod-playerbots")) {
    git clone https://github.com/mod-playerbots/mod-playerbots.git --branch=master "$Source\modules\mod-playerbots"
}
if (-not (Test-Path "$Source\modules\mod-true-companion")) {
    git clone https://github.com/davidlovcov/mod-true-companion.git "$Source\modules\mod-true-companion"
}
if ($Pinned) {
    git -C $Source fetch --depth 1 origin $AcCommit; git -C $Source checkout $AcCommit
    git -C "$Source\modules\mod-playerbots" fetch --depth 1 origin $PbCommit; git -C "$Source\modules\mod-playerbots" checkout $PbCommit
}

cmake -S $Source -B "$Source\build" -G "Visual Studio 17 2022" -A x64 -DSCRIPTS=static -DMODULES=static -DTOOLS_BUILD=all
cmake --build "$Source\build" --config $Config --parallel

$conf = "$Source\build\bin\$Config\configs\modules"
foreach ($c in @("mod_true_companion", "playerbots")) {
    if ((Test-Path "$conf\$c.conf.dist") -and -not (Test-Path "$conf\$c.conf")) { Copy-Item "$conf\$c.conf.dist" "$conf\$c.conf" }
}
if (Get-Command ollama -ErrorAction SilentlyContinue) { ollama pull qwen3.5:4b; ollama pull qwen3:4b-instruct }
else { Write-Warning "Ollama not found here: install it or point TrueCompanion.Ollama.Url to the machine running it." }
Write-Host "Done. Binaries: $Source\build\bin\$Config  -  Addon: https://github.com/davidlovcov/azerothcore-true-companion-addon"
