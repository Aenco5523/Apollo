#Requires -Version 5.1

param(
  [switch]$Refresh
)

$ErrorActionPreference = 'Stop'
$bundleRoot = Split-Path -Parent $PSScriptRoot
$portableZip = Join-Path $bundleRoot 'Host\Apollo-VHID-Portable.zip'
$runtimeRoot = Join-Path $bundleRoot 'Runtime'
$apolloDir = Join-Path $runtimeRoot 'Apollo'
$sunshine = Join-Path $apolloDir 'sunshine.exe'
$configDir = Join-Path $bundleRoot 'Config'
$configPath = Join-Path $configDir 'sunshine.conf'
$helper = Join-Path $PSScriptRoot 'ApolloVhidDevnode.exe'

if (-not (Test-Path -LiteralPath $portableZip -PathType Leaf)) {
  throw "Missing portable package: $portableZip"
}

if ($Refresh -and (Test-Path -LiteralPath $runtimeRoot)) {
  Remove-Item -LiteralPath $runtimeRoot -Recurse -Force
}

if (-not (Test-Path -LiteralPath $sunshine -PathType Leaf)) {
  New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
  Write-Host 'Extracting Apollo portable package...'
  Expand-Archive -LiteralPath $portableZip -DestinationPath $runtimeRoot -Force
}

if (-not (Test-Path -LiteralPath $sunshine -PathType Leaf)) {
  throw "Apollo portable package did not contain: $sunshine"
}

New-Item -ItemType Directory -Force -Path $configDir | Out-Null
if (Test-Path -LiteralPath $configPath) {
  $text = Get-Content -LiteralPath $configPath -Raw
  if ($text -match '(?m)^\s*virtual_hid\s*=') {
    $text = [regex]::Replace($text, '(?m)^\s*virtual_hid\s*=.*$', 'virtual_hid = true')
  } else {
    if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) {
      $text += "`r`n"
    }
    $text += "virtual_hid = true`r`n"
  }
  Set-Content -LiteralPath $configPath -Value $text -NoNewline -Encoding UTF8
} else {
  Set-Content -LiteralPath $configPath -Value "virtual_hid = true`r`n" -NoNewline -Encoding UTF8
}

if (Test-Path -LiteralPath $helper -PathType Leaf) {
  & $helper status | Out-Host
  if ($LASTEXITCODE -ne 0) {
    Write-Warning 'Root\ApolloVhid is not currently present. Apollo will fall back to SendInput.'
  }
} else {
  Write-Warning 'ApolloVhidDevnode.exe is missing; device presence was not checked.'
}

Write-Host "Using config: $configPath"
Write-Host 'Look for: Virtual HID keyboard/mouse backend enabled'
Write-Host 'Press Ctrl+C to stop this foreground test instance.'
Push-Location $apolloDir
try {
  & $sunshine $configPath
  exit $LASTEXITCODE
}
finally {
  Pop-Location
}
