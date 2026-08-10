param(
    [Parameter(Mandatory = $true)]
    [string] $FfmpegRoot,

    [Parameter(Mandatory = $true)]
    [string] $NvCodecRoot,

    [Parameter(Mandatory = $true)]
    [string] $CudaRoot,

    [Parameter(Mandatory = $true)]
    [string] $DxcRoot,

    [string] $KhronosIncludeRoot,
    [string] $BuildRoot,
    [string] $OutputRoot
)

$ErrorActionPreference = "Stop"

if (-not $BuildRoot) {
    $BuildRoot = Join-Path $PSScriptRoot "build\native-full"
}

if (-not $OutputRoot) {
    $OutputRoot = Join-Path $PSScriptRoot "common\src\main\resources"
}

function Resolve-RequiredTool {
    param(
        [string] $Name,
        [string[]] $Candidates = @()
    )

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "未找到构建工具：$Name"
}

function Invoke-CheckedTool {
    param(
        [string] $Tool,
        [string[]] $Arguments
    )

    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "工具执行失败：$Tool，退出码：$LASTEXITCODE"
    }
}

function Resolve-WindowsSdkTool {
    param([string] $Name)

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $candidate = Get-ChildItem -LiteralPath $kitsRoot -Recurse -Filter $Name -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "\\x64\\" } |
        Sort-Object FullName -Descending |
        Select-Object -First 1

    if (-not $candidate) {
        throw "未找到 Windows SDK 工具：$Name"
    }

    return $candidate.FullName
}

function Write-EmbeddedBytes {
    param(
        [string] $InputPath,
        [string] $SizeName,
        [string] $DataName,
        [string] $OutputPath
    )

    $bytes = [System.IO.File]::ReadAllBytes($InputPath)
    $values = [System.String]::Join(",", ($bytes | ForEach-Object { "0x{0:X2}" -f $_ }))
    $source = @"
extern "C" unsigned long long $SizeName = $($bytes.Length);
extern "C" unsigned char $DataName[] = {$values};
"@
    Set-Content -LiteralPath $OutputPath -Value $source -Encoding ascii
}

foreach ($path in @($FfmpegRoot, $NvCodecRoot, $CudaRoot, $DxcRoot)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "依赖目录不存在：$path"
    }
}

$sourceRoot = Join-Path $PSScriptRoot "common\src\main\cpp"
$javaHome = $env:JAVA_HOME
if (-not $javaHome -or -not (Test-Path -LiteralPath (Join-Path $javaHome "include\jni.h"))) {
    throw "JAVA_HOME 未指向包含 JNI 头文件的 JDK。"
}

$ffmpegInclude = Join-Path $FfmpegRoot "include"
$ffmpegLib = Join-Path $FfmpegRoot "lib"
$nvCodecInclude = Join-Path $NvCodecRoot "include"
if (-not (Test-Path -LiteralPath (Join-Path $nvCodecInclude "ffnvcodec"))) {
    $nvCodecInclude = $NvCodecRoot
}
$cudaInclude = Join-Path $CudaRoot "include"
$cudaLib = Join-Path $CudaRoot "lib\x64"

foreach ($path in @(
        (Join-Path $ffmpegInclude "libavcodec\avcodec.h"),
        (Join-Path $ffmpegLib "avcodec.lib"),
        (Join-Path $nvCodecInclude "ffnvcodec\dynlink_loader.h"),
        (Join-Path $cudaInclude "cuda_runtime.h"),
        (Join-Path $cudaLib "cudart_static.lib"))) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "native 依赖文件不存在：$path"
    }
}

$ffmpegLibraries = Get-ChildItem -LiteralPath $ffmpegLib -Recurse -Filter "*.lib" -File |
    Select-Object -ExpandProperty FullName
if (-not $ffmpegLibraries) {
    throw "FFmpeg 静态库目录中没有可链接的 .lib 文件：$ffmpegLib"
}

$cl = Resolve-RequiredTool "cl.exe"
$link = Resolve-RequiredTool "link.exe"
$nvcc = Resolve-RequiredTool "nvcc.exe" @((Join-Path $CudaRoot "bin\nvcc.exe"))
$fxc = Resolve-WindowsSdkTool "fxc.exe"
$dxc = Resolve-RequiredTool "dxc.exe" @((Join-Path $DxcRoot "dxc.exe"))

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$shaderRoot = Join-Path $BuildRoot "shader"
New-Item -ItemType Directory -Force -Path $shaderRoot | Out-Null

$shader = Join-Path $sourceRoot "cs.hlsl"
$dxbc = Join-Path $shaderRoot "cs.dxbc"
$dxil = Join-Path $shaderRoot "cs.dxil"
Invoke-CheckedTool $fxc @("/nologo", "/T", "cs_5_0", "/E", "main", "/Fo", $dxbc, $shader)
Invoke-CheckedTool $dxc @("-T", "cs_6_0", "-E", "main", "-Fo", $dxil, $shader)

$resourceSource = Join-Path $BuildRoot "shader_resources.cpp"
Write-EmbeddedBytes $dxbc "cs_size" "cs" $resourceSource
Add-Content -LiteralPath $resourceSource -Value "" -Encoding ascii
Write-EmbeddedBytes $dxil "cs_dxc_size" "cs_dxc" (Join-Path $BuildRoot "shader_resources_dxc.cpp")
Add-Content -LiteralPath $resourceSource -Value (Get-Content -LiteralPath (Join-Path $BuildRoot "shader_resources_dxc.cpp") -Raw) -Encoding ascii

$includeArgs = @(
    "/I$($javaHome)\include",
    "/I$($javaHome)\include\win32",
    "/I$ffmpegInclude",
    "/I$nvCodecInclude",
    "/I$cudaInclude"
)
if ($KhronosIncludeRoot) {
    $khronosHeader = Join-Path $KhronosIncludeRoot "KHR\khrplatform.h"
    if (-not (Test-Path -LiteralPath $khronosHeader)) {
        throw "Khronos 头文件不存在：$khronosHeader"
    }
    $includeArgs += "/I$KhronosIncludeRoot"
}
$moduleArgs = @(
    "/nologo", "/std:c++20", "/EHsc", "/O2", "/MD", "/wd5202", "/c",
    "/ifcOutput", "$BuildRoot\",
    "/Fo$BuildRoot\media.obj"
) + $includeArgs + @((Join-Path $sourceRoot "media.ixx"))
Invoke-CheckedTool $cl $moduleArgs

$resourceModuleArgs = @(
    "/nologo", "/std:c++20", "/EHsc", "/O2", "/MD", "/c",
    "/ifcOutput", "$BuildRoot\",
    "/Fo$BuildRoot\resource.obj"
) + $includeArgs + @((Join-Path $sourceRoot "res.ixx"))
Invoke-CheckedTool $cl $resourceModuleArgs

$cppFiles = @(
    "main.cpp",
    "decoder.cpp",
    "frame.cpp",
    "audio.cpp",
    "jnipp.cpp"
)
$objects = @(
    (Join-Path $BuildRoot "media.obj"),
    (Join-Path $BuildRoot "resource.obj")
)
$commonCompileArgs = @(
    "/nologo", "/std:c++20", "/EHsc", "/O2", "/MD", "/wd5202", "/c",
    "/ifcSearchDir", $BuildRoot,
    "/reference", "Media=$BuildRoot\Media.ifc",
    "/reference", "Resource=$BuildRoot\Resource.ifc"
) + $includeArgs

foreach ($file in $cppFiles) {
    $source = Join-Path $sourceRoot $file
    $object = Join-Path $BuildRoot ([System.IO.Path]::ChangeExtension($file, ".obj"))
    Invoke-CheckedTool $cl ($commonCompileArgs + @("/Fo$object", $source))
    $objects += $object
}

foreach ($file in @("gl.c", "wgl.c")) {
    $source = Join-Path $sourceRoot $file
    $object = Join-Path $BuildRoot ([System.IO.Path]::ChangeExtension($file, ".obj"))
    Invoke-CheckedTool $cl (@("/nologo", "/O2", "/MD", "/TC", "/c") + $includeArgs + @("/Fo$object", $source))
    $objects += $object
}

$cudaObject = Join-Path $BuildRoot "cuda.obj"
Invoke-CheckedTool $nvcc @(
    "-c", (Join-Path $sourceRoot "cuda.cu"),
    "-o", $cudaObject,
    "-std=c++20",
    "-O3",
    "-cudart", "static",
    "-ccbin", $cl,
    "-Xcompiler", "/EHsc /O2 /MD",
    "-I$($javaHome)\include",
    "-I$($javaHome)\include\win32",
    "-I$ffmpegInclude",
    "-I$nvCodecInclude",
    "-I$cudaInclude"
)
$objects += $cudaObject

$resourceObject = Join-Path $BuildRoot "shader_resources.obj"
Invoke-CheckedTool $cl (@("/nologo", "/EHsc", "/O2", "/MD", "/c", "/Fo$resourceObject", $resourceSource))
$objects += $resourceObject

$outputDll = Join-Path $OutputRoot "MediaPlayer.dll"
$outputLib = Join-Path $BuildRoot "MediaPlayer.lib"
$outputPdb = Join-Path $BuildRoot "MediaPlayer.pdb"
$linkArgs = @(
    "/NOLOGO", "/DLL", "/OPT:REF", "/OPT:ICF", "/NODEFAULTLIB:LIBCMT",
    "/OUT:$outputDll", "/IMPLIB:$outputLib", "/PDB:$outputPdb"
) + $objects + $ffmpegLibraries + @(
    "/LIBPATH:$cudaLib",
    "cudart_static.lib", "d3d11.lib", "d3d12.lib", "dxgi.lib", "d3dcompiler.lib",
    "opengl32.lib", "ole32.lib", "user32.lib", "gdi32.lib",
    "advapi32.lib", "bcrypt.lib", "secur32.lib", "shell32.lib", "shlwapi.lib",
    "oleaut32.lib", "uuid.lib", "version.lib", "vfw32.lib", "strmiids.lib",
    "mf.lib", "mfplat.lib", "mfuuid.lib",
    "userenv.lib", "ws2_32.lib", "crypt32.lib", "ncrypt.lib"
)
Invoke-CheckedTool $link $linkArgs

$manifest = @(
    "MediaPlayer-BBS 完整 native 后端",
    "",
    "该目录用于复制到 MediaPlayer-BBS/common/src/main/resources。",
    "MediaPlayer.dll 静态链接 FFmpeg 和 CUDA runtime，包含 CUDA/D3D11VA/D3D12VA 构建路径。",
    "运行时只需要本目录中的 MediaPlayer.dll，不需要额外携带 FFmpeg 或 CUDA runtime DLL。",
    "GitHub Actions runner 没有可用于运行时验证的 NVIDIA GPU，因此硬解只完成编译，不代表本机运行时必然启用。"
)
Set-Content -LiteralPath (Join-Path $OutputRoot "BUILD-MANIFEST.txt") -Value $manifest -Encoding utf8

Write-Host "完整 native 后端构建完成：$OutputRoot"
