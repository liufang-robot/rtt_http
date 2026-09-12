if ([string]::IsNullOrWhiteSpace($env:CONDA_PREFIX)) {
    throw "The orocos-dev development environment is required."
}
. (Join-Path $env:CONDA_PREFIX "Library/dev-env.ps1")
$repository = Split-Path -Parent $PSScriptRoot
$cyclicPrefix = Join-Path $repository ".ci-dependencies/install"
$env:CMAKE_PREFIX_PATH = "$cyclicPrefix;$env:CMAKE_PREFIX_PATH"
$env:PKG_CONFIG_PATH = "$cyclicPrefix/lib/pkgconfig;$env:PKG_CONFIG_PATH"
$env:PATH = "$cyclicPrefix/bin;$cyclicPrefix/lib;$cyclicPrefix/lib/orocos/win32/types;$cyclicPrefix/lib/orocos/win32/plugins;$env:PATH"
$env:RTT_COMPONENT_PATH = "$cyclicPrefix/lib/orocos"
$env:OROCOS_COMPONENT_PATH = $env:RTT_COMPONENT_PATH
