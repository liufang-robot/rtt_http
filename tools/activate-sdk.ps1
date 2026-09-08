if ([string]::IsNullOrWhiteSpace($env:CONDA_PREFIX)) {
    throw "The orocos-dev development environment is required."
}
. (Join-Path $env:CONDA_PREFIX "Library/dev-env.ps1")
