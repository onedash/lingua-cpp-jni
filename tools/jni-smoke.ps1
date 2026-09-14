$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$classes = Join-Path $root 'build\jni-classes'
$native = Join-Path $root 'build\cmake\lingua_jni.dll'
$model = Join-Path $root 'data\model.bin'

if (-not (Test-Path -LiteralPath $native)) {
    throw "Build the CMake project first; missing $native"
}

New-Item -ItemType Directory -Force -Path $classes | Out-Null
$sources = Get-ChildItem -LiteralPath (Join-Path $root 'jni\src\main') -Recurse -Filter '*.java'
$smoke = Join-Path $root 'jni\src\test\java\io\github\onedash\linguacpp\LanguageDetectorSmoke.java'
& javac -d $classes $sources.FullName $smoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& java "-Dlingua.cpp.native.path=$native" -cp $classes `
    io.github.onedash.linguacpp.LanguageDetectorSmoke $model
exit $LASTEXITCODE
