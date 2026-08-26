#Requires -Version 5.1
#Requires -RunAsAdministrator

param(
  [string]$DriverDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Driver')
)

$ErrorActionPreference = 'Stop'
$hardwareId = 'Root\ApolloVhid'
$helper = Join-Path $PSScriptRoot 'ApolloVhidDevnode.exe'
$inf = Join-Path $DriverDir 'ApolloVhid.inf'
$cat = Join-Path $DriverDir 'apollovhid.cat'

if ([Environment]::OSVersion.Version.Build -lt 16299) {
  throw 'ApolloVhid requires Windows 10 version 1709 (build 16299) or newer.'
}

foreach ($path in @($helper, $inf, $cat)) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Missing required file: $path"
  }
}

$catalogSignature = Get-AuthenticodeSignature -FilePath $cat
Write-Host "Catalog signature status: $($catalogSignature.Status)"
if ($catalogSignature.Status -ne 'Valid') {
  throw @"
ApolloVhid.cat is not trusted by Windows. Installation stopped.
Use a properly signed driver package. This script will not disable or bypass
Windows driver-signature enforcement, Secure Boot, or other security checks.
"@
}

& $helper status
$statusResult = $LASTEXITCODE
if ($statusResult -gt 1) {
  throw "ApolloVhidDevnode status failed with exit code $statusResult"
}

$created = $false
if ($statusResult -eq 1) {
  Write-Host "Creating $hardwareId root device..."
  & $helper create
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to create $hardwareId root device."
  }
  $created = $true
}

Write-Host 'Adding the signed driver package and installing it on matching devices...'
& "$env:WINDIR\System32\pnputil.exe" /add-driver $inf /install
$installResult = $LASTEXITCODE
if ($installResult -ne 0) {
  if ($created) {
    Write-Warning 'Driver installation failed; removing the root device created by this script.'
    & $helper remove | Out-Host
  }
  throw "PnPUtil failed with exit code $installResult"
}

Write-Host ''
Write-Host 'Apollo V-HID device/driver status:'
& "$env:WINDIR\System32\pnputil.exe" /enum-devices /deviceid $hardwareId /drivers
if ($LASTEXITCODE -ne 0) {
  throw 'PnPUtil could not verify the Apollo V-HID device after installation.'
}

Write-Host ''
Write-Host 'Apollo V-HID installation completed without changing Windows security settings.'
