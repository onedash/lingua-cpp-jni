param([string]$RustRoot = '../lingua-rs')
$ErrorActionPreference = 'Stop'
$RustRoot = (Resolve-Path $RustRoot).Path.Replace('\','/')
$root = Split-Path $PSScriptRoot -Parent
$dest = "$root/build/reference"
New-Item -ItemType Directory -Force "$dest/src", "$root/generated", "$root/data" | Out-Null
Copy-Item "$RustRoot/src/*.rs" "$dest/src" -Force
Copy-Item "$PSScriptRoot/export.rs" "$dest/src/export.rs" -Force
Add-Content "$dest/src/lib.rs" "`npub mod export;"
Copy-Item "$PSScriptRoot/reference.rs" "$dest/src/main.rs" -Force
$original = Get-Content "$RustRoot/Cargo.toml" -Raw
$deps = [regex]::Matches($original, '(?m)^lingua-[^\r\n]+') | ForEach-Object {
    $_.Value.Replace('path = "language-models/', "path = `"$RustRoot/language-models/")
}
$features = $original.Substring($original.IndexOf('[features]'))
$features = $features -replace '(?m)^(accuracy-reports|benchmark|python) = .*', '$1 = []'
$manifest = @'
[package]
name = "lingua-reference"
version = "0.1.0"
edition = "2024"
[workspace]
[dependencies]
counter = "=0.7.1"
dashmap = "=6.2.1"
fastrand = "=2.5.0"
fst = "=0.4.7"
include_dir = "=0.7.4"
itertools = "=0.14.0"
maplit = "=1.0.2"
regex = "=1.13.1"
serde = { version = "=1.0.229", features = ["derive"] }
strum = "=0.27.2"
strum_macros = "=0.27.2"
rayon = "=1.12.0"
'@
Set-Content "$dest/Cargo.toml" ($manifest + "`n" + ($deps -join "`n") + "`n" + $features) -Encoding utf8
if (Test-Path "$PSScriptRoot/reference.lock") { Copy-Item "$PSScriptRoot/reference.lock" "$dest/Cargo.lock" -Force }
Write-Host "Reference source copied unchanged; offline dependency versions are pinned in build/reference/Cargo.toml."
