$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Root

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) {
    $Python = Get-Command py -ErrorAction SilentlyContinue
}
if ($Python) {
    if ($Python.Name -eq "py.exe" -or $Python.Name -eq "py") {
        & $Python.Source -3 (Join-Path $Root "scripts\check-wgsl-reserved.py")
    } else {
        & $Python.Source (Join-Path $Root "scripts\check-wgsl-reserved.py")
    }
    if ($LASTEXITCODE -ne 0) {
        throw "WGSL reserved-word validation failed."
    }
    if ($Python.Name -eq "py.exe" -or $Python.Name -eq "py") {
        & $Python.Source -3 (Join-Path $Root "scripts\check-tool-icons.py")
    } else {
        & $Python.Source (Join-Path $Root "scripts\check-tool-icons.py")
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Tool icon validation failed."
    }
} else {
    Write-Warning "Python was not found; build-time WGSL and tool-icon scans skipped. Runtime preflight remains active."
}

if (-not $env:CMAKE_PREFIX_PATH) {
    Write-Warning "CMAKE_PREFIX_PATH is not set. Set it to your Qt MSVC directory if CMake cannot find Qt."
}

$WebGpuCMakeArgument = "-DVFXPHOTOLAB_ENABLE_WEBGPU=ON"
if ($env:VFXPHOTOLAB_SKIP_WGPU_FETCH -eq "1") {
    $WebGpuCMakeArgument = "-DVFXPHOTOLAB_ENABLE_WEBGPU=OFF"
    Write-Host "Skipping wgpu-native acquisition; building with the CPU renderer only."
} elseif ($env:WGPU_ROOT) {
    Write-Host "Using manually supplied wgpu-native SDK from WGPU_ROOT=$env:WGPU_ROOT"
} else {
    try {
        & (Join-Path $Root "scripts\fetch-wgpu-native.ps1")
    } catch {
        $WebGpuCMakeArgument = "-DVFXPHOTOLAB_ENABLE_WEBGPU=OFF"
        Write-Warning "wgpu-native acquisition failed; continuing with a CPU-only build. $($_.Exception.Message)"
    }
}


$OcioCMakeArguments = @("-DVFXPHOTOLAB_ENABLE_OCIO=ON")
if ($env:VFXPHOTOLAB_SKIP_OCIO_FETCH -eq "1") {
    $OcioCMakeArguments = @("-DVFXPHOTOLAB_ENABLE_OCIO=OFF")
    Write-Host "Skipping OpenColorIO acquisition; building with ICC-only colour management."
} else {
    $OcioPrefix = if ($env:OCIO_ROOT) { $env:OCIO_ROOT } else { Join-Path $Root "third_party\opencolorio" }
    $OcioDependencyPrefix = Join-Path $Root "build\deps\opencolorio-2.5.2\ext\dist"
    if ($env:OCIO_ROOT) {
        Write-Host "Using manually supplied OpenColorIO installation from OCIO_ROOT=$env:OCIO_ROOT"
    } else {
        try {
            & (Join-Path $Root "scripts\fetch-opencolorio.ps1")
        } catch {
            $OcioCMakeArguments = @("-DVFXPHOTOLAB_ENABLE_OCIO=OFF")
            Write-Warning "OpenColorIO acquisition failed; continuing with ICC-only colour management. $($_.Exception.Message)"
        }
    }

    if ($OcioCMakeArguments[0] -eq "-DVFXPHOTOLAB_ENABLE_OCIO=ON") {
        $OcioCMakeArguments += "-DVFXPHOTOLAB_OCIO_ROOT=$OcioPrefix"
        $OcioCMakeArguments += "-DVFXPHOTOLAB_OCIO_DEPENDENCY_ROOT=$OcioDependencyPrefix"
        foreach ($Candidate in @(
            (Join-Path $OcioPrefix "lib\cmake\OpenColorIO"),
            (Join-Path $OcioPrefix "lib64\cmake\OpenColorIO")
        )) {
            if (Test-Path (Join-Path $Candidate "OpenColorIOConfig.cmake")) {
                $OcioCMakeArguments += "-DOpenColorIO_DIR=$Candidate"
                Write-Host "Using OpenColorIO package: $Candidate"
                break
            }
        }
    }
}

cmake --preset release $WebGpuCMakeArgument @OcioCMakeArguments
cmake --build --preset release --parallel

Write-Host ""
Write-Host "Built: $Root\build\release\VFXPhotoLab.exe"
