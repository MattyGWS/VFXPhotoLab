$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Version = "2.5.2"
$Prefix = if ($env:OCIO_ROOT) { $env:OCIO_ROOT } else { Join-Path $Root "third_party\opencolorio" }
$Config = Join-Path $Prefix "lib\cmake\OpenColorIO\OpenColorIOConfig.cmake"
if (Test-Path $Config) {
    Write-Host "OpenColorIO $Version is already present at $Prefix"
    exit 0
}

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) { $Python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $Python) { throw "OpenColorIO acquisition requires Python 3." }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "OpenColorIO acquisition requires CMake." }
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { throw "OpenColorIO acquisition requires Ninja." }

$Downloads = Join-Path $Root "build\deps\downloads"
$Sources = Join-Path $Root "build\deps\src"
$Archive = Join-Path $Downloads "opencolorio-$Version.tar.gz"
$Source = Join-Path $Sources "opencolorio-$Version"
$Build = Join-Path $Root "build\deps\opencolorio-$Version"
$Url = "https://files.pythonhosted.org/packages/6a/ea/9d930df6740f9b09b0b342f40a5ef165da5050141e496081ef80b302e566/opencolorio-$Version.tar.gz"
$Expected = "fecebd0914089b0c8238c55648f8eb2ccd2702ab4b2eea53856a0e368ded8262"
New-Item -ItemType Directory -Force $Downloads, $Sources, $Build, $Prefix | Out-Null

if (-not (Test-Path $Archive)) {
    Write-Host "Downloading OpenColorIO $Version source package..."
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile "$Archive.part"
    Move-Item -Force "$Archive.part" $Archive
}
$Actual = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant()
if ($Actual -ne $Expected) {
    Remove-Item -Force $Archive
    throw "OpenColorIO archive checksum mismatch."
}

if (-not (Test-Path (Join-Path $Source "CMakeLists.txt"))) {
    if (Test-Path $Source) { Remove-Item -Recurse -Force $Source }
    New-Item -ItemType Directory -Force $Source | Out-Null
    $PythonArgs = @("-c", @'
import pathlib, tarfile, sys
archive, destination = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
with tarfile.open(archive, 'r:gz') as tf:
    members = tf.getmembers()
    roots = {m.name.split('/', 1)[0] for m in members if m.name}
    prefix = next(iter(roots)) + '/' if len(roots) == 1 else ''
    for member in members:
        name = member.name[len(prefix):] if prefix and member.name.startswith(prefix) else member.name
        if not name: continue
        target = (destination / name).resolve()
        if destination.resolve() not in target.parents and target != destination.resolve():
            raise RuntimeError('Unsafe path in OpenColorIO source archive')
        member.name = name
        try:
            tf.extract(member, destination, filter='data')
        except TypeError:
            # Python 3.11 and older do not expose extraction filters. The
            # explicit resolved-path check above still prevents traversal.
            tf.extract(member, destination)
'@, $Archive, $Source)
    if ($Python.Name -eq "py.exe" -or $Python.Name -eq "py") { & $Python.Source -3 @PythonArgs } else { & $Python.Source @PythonArgs }
    if ($LASTEXITCODE -ne 0) { throw "Could not extract OpenColorIO source." }
}

cmake -S $Source -B $Build -G Ninja `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_INSTALL_PREFIX=$Prefix" `
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON" `
    "-DBUILD_SHARED_LIBS=OFF" `
    "-DOCIO_BUILD_APPS=OFF" `
    "-DOCIO_BUILD_DOCS=OFF" `
    "-DOCIO_BUILD_GPU_TESTS=OFF" `
    "-DOCIO_BUILD_PYTHON=OFF" `
    "-DOCIO_BUILD_TESTS=OFF" `
    "-DOCIO_INSTALL_EXT_PACKAGES=ALL"
if ($LASTEXITCODE -ne 0) { throw "OpenColorIO configure failed." }
cmake --build $Build --parallel
if ($LASTEXITCODE -ne 0) { throw "OpenColorIO build failed." }
cmake --install $Build
if ($LASTEXITCODE -ne 0) { throw "OpenColorIO install failed." }
Write-Host "Installed OpenColorIO $Version to $Prefix"
