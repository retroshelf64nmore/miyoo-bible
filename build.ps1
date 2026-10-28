# Builds the ARM binary into app\Bible and packs two releases (both extract to SD root as App/Bible/...):
#   dist\Bible-<version>.zip                English only (KJV + WEB)
#   dist\Bible-International-<version>.zip  every translation in app\Bible\data
# Needs Zig 0.14 (https://ziglang.org/download/) on PATH or passed with -Zig, and app\Bible\data built by tools\make_data.py.
#   .\build.ps1 -Version 1.1.0
#   .\build.ps1 -Preview          # also builds bible_preview.exe (renders PNG frames on a PC)
param([string]$Version = '1.1.0', [string]$Zig = 'zig', [switch]$Preview)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

& $Zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -static -s src/bible.c -o app/Bible/bible -lm
if ($LASTEXITCODE) { throw 'ARM build failed' }
if ($Preview) {
    & $Zig cc -target x86_64-windows-gnu -DPREVIEW -O2 src/bible.c -o bible_preview.exe
    if ($LASTEXITCODE) { throw 'preview build failed' }
}

# Zip with forward-slash entry names (Compress-Archive on Windows PowerShell writes backslashes).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
New-Item -ItemType Directory -Force dist | Out-Null
$root = Join-Path $PSScriptRoot 'app'
function Pack([string]$name, [scriptblock]$keep) {
    $zip = Join-Path $PSScriptRoot "dist\$name"
    if (Test-Path $zip) { Remove-Item $zip }
    $archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
    try {
        Get-ChildItem $root -Recurse -File | Where-Object { $_.Name -notin 'state.txt', 'state.tmp' } | ForEach-Object {
            $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
            if (& $keep $rel) { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, "App/$rel", 'Optimal') | Out-Null }
        }
    }
    finally { $archive.Dispose() }
    "built $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
}
$english = 'eng-kjv', 'engwebp'
Pack "Bible-$Version.zip" { param($rel) $rel -notmatch '^Bible/data/([^/]+)/' -or $english -contains $Matches[1] }
Pack "Bible-International-$Version.zip" { param($rel) $true }
