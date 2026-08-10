# 兼容旧环境的纯软件解码构建入口；完整 native 构建请使用 build-native.ps1。
param(
    [string] $FfmpegRoot = "$env:TEMP\bbs-mediaplayer-native\ffmpeg-master-latest-win64-lgpl-shared",
    [string] $VsRoot = "D:\yingyong\Microsoft Visual Studio\18\BuildTools"
)

$ErrorActionPreference = "Stop"

$source = Join-Path $PSScriptRoot "common\src\main\cpp\software_backend.cpp"
$resources = Join-Path $PSScriptRoot "common\src\main\resources"
$outputRoot = Join-Path $env:TEMP "bbs-mediaplayer-native\build"
$outputDll = Join-Path $outputRoot "MediaPlayer.dll"
$outputObj = Join-Path $outputRoot "software_backend.obj"
$outputPdb = Join-Path $outputRoot "MediaPlayer.pdb"
$outputImportLib = Join-Path $outputRoot "MediaPlayer.lib"
$vcvars = Join-Path $VsRoot "VC\Auxiliary\Build\vcvars64.bat"
$javaHome = $env:JAVA_HOME

if (-not (Test-Path -LiteralPath $source)) {
    throw "native source not found: $source"
}

if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}

if (-not $javaHome) {
    throw "JAVA_HOME is not set"
}

if (-not (Test-Path -LiteralPath (Join-Path $FfmpegRoot "include\libavcodec\avcodec.h"))) {
    throw "FFmpeg development package not found. Pass -FfmpegRoot to the extracted shared package."
}

New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$includeJava = Join-Path $javaHome "include"
$includeJavaWin32 = Join-Path $javaHome "include\win32"
$includeFfmpeg = Join-Path $FfmpegRoot "include"
$libFfmpeg = Join-Path $FfmpegRoot "lib"

$compileCommand = @"
call "$vcvars" >nul && cl /nologo /utf-8 /std:c++20 /EHsc /O2 /MD /LD /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"$outputObj" /Fd"$outputPdb" /I"$includeJava" /I"$includeJavaWin32" /I"$includeFfmpeg" "$source" /link /NOLOGO /OUT:"$outputDll" /IMPLIB:"$outputImportLib" /PDB:"$outputPdb" /LIBPATH:"$libFfmpeg" avcodec.lib avformat.lib avutil.lib swscale.lib swresample.lib opengl32.lib
"@

cmd /c $compileCommand
if ($LASTEXITCODE -ne 0) {
    throw "native DLL build failed. Exit code: $LASTEXITCODE"
}

Copy-Item -LiteralPath $outputDll -Destination (Join-Path $resources "MediaPlayer.dll") -Force

$dependencyNames = @(
    "avcodec-*.dll",
    "avformat-*.dll",
    "avutil-*.dll",
    "swscale-*.dll",
    "swresample-*.dll"
)

foreach ($pattern in $dependencyNames) {
    Get-ChildItem -LiteralPath (Join-Path $FfmpegRoot "bin") -Filter $pattern |
        Copy-Item -Destination $resources -Force
}

Write-Host "MediaPlayer.dll and FFmpeg runtime dependencies were copied."
