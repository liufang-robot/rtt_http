$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "activate-sdk.ps1")
$repository = Split-Path -Parent $PSScriptRoot
$staging = Join-Path $repository ".ci-dependencies/install"
$build = Join-Path $repository ".ci-dependencies/build-rtt"
& cmake -S "$repository/.ci-dependencies/rtt" -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_INSTALL_PREFIX=$staging" `
    -DOROCOS_TARGET=win32 -DENABLE_CORBA=OFF -DENABLE_TESTS=OFF `
    -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DOROBLD_FORCE_TINY_DEMARSHALLER=ON `
    -DORO_OS_USE_BOOST_THREAD=ON -DPLUGINS_ENABLE=ON -DPLUGINS_ENABLE_TYPEKIT=ON `
    -DPLUGINS_ENABLE_SCRIPTING=ON -DPLUGINS_ENABLE_MARSHALLING=ON `
    "-DDEFAULT_PLUGIN_PATH=$staging/lib/orocos"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --install $build
exit $LASTEXITCODE
