param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Command)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "activate-sdk.ps1")
if ($Command.Count -eq 0) { throw "A development command is required." }
$executable = $Command[0]
$arguments = if ($Command.Count -gt 1) { $Command[1..($Command.Count - 1)] } else { @() }
& $executable @arguments
exit $LASTEXITCODE
