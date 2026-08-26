#Requires -Version 5.1
#Requires -RunAsAdministrator

$ErrorActionPreference = 'Stop'
$helper = Join-Path $PSScriptRoot 'ApolloVhidDevnode.exe'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) {
  throw "Missing helper: $helper"
}

& $helper remove
if ($LASTEXITCODE -ne 0) {
  throw "Failed to remove Root\ApolloVhid device (exit $LASTEXITCODE)."
}

Write-Host 'Root\ApolloVhid device removed.'
Write-Host 'The staged driver package is intentionally left in the Windows driver store.'
