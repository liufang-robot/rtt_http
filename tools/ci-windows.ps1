param([ValidateSet("configure", "build", "test", "install")][string]$Step)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "activate-sdk.ps1")
$repository = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repository "build"
switch ($Step) {
    "configure" {
        & cmake -S $repository -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_INSTALL_PREFIX=$repository/install" `
            "-DRTT_HTTP_HTTPLIB_INCLUDE_DIR=$repository/.ci-dependencies/cpp-httplib"
    }
    "build" { & cmake --build $build --parallel 2 }
    "test" { & ctest --test-dir $build --output-on-failure --no-tests=error }
    "install" { & cmake --install $build }
}
exit $LASTEXITCODE
