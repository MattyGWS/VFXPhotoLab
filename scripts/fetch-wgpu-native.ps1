$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Version = if ($env:VFXPHOTOLAB_WGPU_VERSION) { $env:VFXPHOTOLAB_WGPU_VERSION } else { "29.0.1.1" }
$Target = if ($env:WGPU_ROOT) { $env:WGPU_ROOT } else { Join-Path $Root "third_party\wgpu-native" }
$ReleaseBase = if ($env:VFXPHOTOLAB_WGPU_RELEASE_BASE) {
    $env:VFXPHOTOLAB_WGPU_RELEASE_BASE
} else {
    "https://github.com/gfx-rs/wgpu-native/releases/download/v$Version"
}

function Test-WgpuSdk([string]$Path) {
    $Header = (Test-Path (Join-Path $Path "include\webgpu\webgpu.h")) -or
              (Test-Path (Join-Path $Path "include\webgpu-headers\webgpu.h"))
    $Library = (Test-Path (Join-Path $Path "lib\wgpu_native.lib")) -or
               (Test-Path (Join-Path $Path "lib64\wgpu_native.lib")) -or
               (Test-Path (Join-Path $Path "lib\wgpu.lib")) -or
               (Test-Path (Join-Path $Path "lib64\wgpu.lib"))
    return $Header -and $Library
}

if (Test-WgpuSdk $Target) {
    Write-Host "wgpu-native $Version is already present."
    exit 0
}

$Architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
switch ($Architecture) {
    "X64"   { $ArchiveArch = "x86_64" }
    "Arm64" { $ArchiveArch = "aarch64" }
    "X86"   { $ArchiveArch = "i686" }
    default  { throw "Unsupported Windows architecture for the pinned wgpu-native SDK: $Architecture" }
}

$ArchiveName = if ($env:VFXPHOTOLAB_WGPU_ASSET) {
    $env:VFXPHOTOLAB_WGPU_ASSET
} else {
    "wgpu-windows-$ArchiveArch-msvc-release.zip"
}
$ArchiveUrl = if ($env:VFXPHOTOLAB_WGPU_URL) {
    $env:VFXPHOTOLAB_WGPU_URL
} else {
    "$ReleaseBase/$ArchiveName"
}

$Temp = Join-Path ([System.IO.Path]::GetTempPath()) ("vfxphotolab-wgpu-" + [Guid]::NewGuid().ToString("N"))
$Extract = Join-Path $Temp "extract"
$Archive = Join-Path $Temp $ArchiveName
$Stage = Join-Path (Split-Path -Parent $Target) (".wgpu-native-stage-" + [Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Path $Extract -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $Target) -Force | Out-Null

    Write-Host "Downloading wgpu-native $Version ($ArchiveName)..."
    Invoke-WebRequest -Uri $ArchiveUrl -OutFile $Archive -UseBasicParsing
    Expand-Archive -Path $Archive -DestinationPath $Extract -Force

    $Header = Get-ChildItem -Path $Extract -Recurse -File -Filter "webgpu.h" |
        Where-Object { $_.FullName -match '[\\/]include[\\/](webgpu|webgpu-headers)[\\/]webgpu\.h$' } |
        Select-Object -First 1
    if (-not $Header) {
        throw "The downloaded archive does not contain the expected WebGPU headers."
    }

    $IncludeDirectory = Split-Path -Parent (Split-Path -Parent $Header.FullName)
    $ReleaseRoot = Split-Path -Parent $IncludeDirectory
    New-Item -ItemType Directory -Path $Stage -Force | Out-Null
    Copy-Item -Path (Join-Path $ReleaseRoot "*") -Destination $Stage -Recurse -Force

    if (-not (Test-WgpuSdk $Stage)) {
        $Library = Get-ChildItem -Path $Extract -Recurse -File |
            Where-Object { $_.Name -in @("wgpu_native.lib", "wgpu.lib") } |
            Select-Object -First 1
        if ($Library) {
            $LibraryDirectory = Split-Path -Parent $Library.FullName
            $StageLibrary = Join-Path $Stage "lib"
            New-Item -ItemType Directory -Path $StageLibrary -Force | Out-Null
            Copy-Item -Path (Join-Path $LibraryDirectory "*") -Destination $StageLibrary -Recurse -Force
        }
    }

    if (-not (Test-WgpuSdk $Stage)) {
        throw "The downloaded archive could not be normalised into include\ and lib\."
    }

    Set-Content -Path (Join-Path $Stage ".vfxphotolab-version") -Value $Version -NoNewline
    if (Test-Path $Target) {
        Remove-Item -Path $Target -Recurse -Force
    }
    Move-Item -Path $Stage -Destination $Target
    Write-Host "Installed wgpu-native $Version to $Target"
} finally {
    if (Test-Path $Temp) { Remove-Item -Path $Temp -Recurse -Force }
    if (Test-Path $Stage) { Remove-Item -Path $Stage -Recurse -Force }
}
