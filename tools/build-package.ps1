param([string]$InnoCompilerPath)

$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$clionBin = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin'
$mingwBin = Join-Path $clionBin 'mingw\bin'
$cmake = Join-Path $clionBin 'cmake\win\x64\bin\cmake.exe'
$ninja = Join-Path $clionBin 'ninja\win\x64\ninja.exe'
$cCompiler = Join-Path $mingwBin 'gcc.exe'
$cxxCompiler = Join-Path $mingwBin 'g++.exe'
$rcCompiler = Join-Path $mingwBin 'windres.exe'

foreach ($tool in @($cmake, $ninja, $cCompiler, $cxxCompiler, $rcCompiler)) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Required CLion tool not found: $tool" }
}
$env:PATH = "$mingwBin;$env:PATH"
if (-not $InnoCompilerPath) {
    $InnoCompilerPath = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'
}
if (-not (Test-Path -LiteralPath $InnoCompilerPath)) {
    throw "Inno Setup compiler not found: $InnoCompilerPath"
}

& (Join-Path $PSScriptRoot 'make-icon.ps1') `
    -SourcePath (Join-Path $projectRoot 'icon.png') `
    -OutputPath (Join-Path $projectRoot 'assets\app.ico')
if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'assets\app.ico'))) {
    throw 'Icon creation failed.'
}

$buildDir = Join-Path $projectRoot 'cmake-build-release'
$configureArgs = @('-S', $projectRoot, '-B', $buildDir, '-G', 'Ninja',
                   '-DCMAKE_BUILD_TYPE=Release',
                   "-DCMAKE_C_COMPILER=$cCompiler", "-DCMAKE_CXX_COMPILER=$cxxCompiler",
                   "-DCMAKE_RC_COMPILER=$rcCompiler", "-DCMAKE_MAKE_PROGRAM=$ninja")
$cachedLibssh2 = Join-Path $projectRoot 'cmake-build-debug\_deps\libssh2-src'
if (Test-Path -LiteralPath $cachedLibssh2) {
    $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_LIBSSH2=$cachedLibssh2"
}
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed.' }
& $cmake --build $buildDir --target SSHClient -j 6
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

$licenseDir = Join-Path $buildDir 'licenses'
[System.IO.Directory]::CreateDirectory($licenseDir) | Out-Null
$libssh2Source = if (Test-Path -LiteralPath $cachedLibssh2) {
    $cachedLibssh2
} else {
    Join-Path $buildDir '_deps\libssh2-src'
}
$licenseFiles = @(
    @{ Source = (Join-Path $libssh2Source 'COPYING'); Name = 'libssh2.txt' },
    @{ Source = (Join-Path $mingwBin '..\licenses\gcc\COPYING3'); Name = 'gcc-gplv3.txt' },
    @{ Source = (Join-Path $mingwBin '..\licenses\gcc\COPYING.RUNTIME'); Name = 'gcc-runtime-exception.txt' },
    @{ Source = (Join-Path $mingwBin '..\licenses\winpthreads\COPYING'); Name = 'winpthreads.txt' },
    @{ Source = (Join-Path $mingwBin '..\licenses\crt\COPYING'); Name = 'mingw-crt.txt' }
)
foreach ($item in $licenseFiles) {
    if (-not (Test-Path -LiteralPath $item.Source)) { throw "License file not found: $($item.Source)" }
    Copy-Item -LiteralPath $item.Source -Destination (Join-Path $licenseDir $item.Name) -Force
}

& $InnoCompilerPath (Join-Path $projectRoot 'installer\SSHClient.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
Write-Output "Package ready: $(Join-Path $projectRoot 'dist\SSHClient-Setup-0.1.1.exe')"
