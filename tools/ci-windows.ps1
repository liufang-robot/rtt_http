param([ValidateSet("configure", "build", "test", "install")][string]$Step)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "activate-sdk.ps1")
$repository = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repository "build"
# v0.1.5 predates HTTP. Supplement its SDK using the same pinned vcpkg/Boost
# baseline. The cyclic RTT build is selected by activate-sdk.ps1.
$httpDependencies = Join-Path $repository ".ci-dependencies/vcpkg/installed/x64-windows"
$env:CMAKE_PREFIX_PATH = "$env:CMAKE_PREFIX_PATH;$httpDependencies"
$env:PATH = "$httpDependencies/bin;$env:PATH"
switch ($Step) {
    "configure" {
        & cmake -S $repository -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_INSTALL_PREFIX=$repository/install" `
            "-DRTT_HTTP_HTTPLIB_INCLUDE_DIR=$repository/.ci-dependencies/cpp-httplib" `
            "-DOPENSSL_ROOT_DIR=$httpDependencies"
    }
    "build" { & cmake --build $build --parallel 2 }
    "test" { & ctest --test-dir $build --output-on-failure --no-tests=error }
    "install" { & cmake --install $build }
}
exit $LASTEXITCODE
