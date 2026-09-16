# Serial (SMP over shell) MCUmgr OTA helper for craner_ai_safety_hook.
#
# The hook controller has no Ethernet, so firmware updates are uploaded over
# the debug console (uart5, 115200) with the MCUmgr "serial" connection type.
#
# Examples:
#   .\tool\hook_ota_serial.ps1 -action list
#   .\tool\hook_ota_serial.ps1 -action upload -port COM7
#   .\tool\hook_ota_serial.ps1 -action test -hash <image hash from list>
#   .\tool\hook_ota_serial.ps1 -action reset
#   .\tool\hook_ota_serial.ps1 -action confirm -hash <image hash>

param(
    [ValidateSet("upload", "list", "test", "confirm", "reset")]
    [string]$action = "list",
    [string]$port = "",
    [int]$baud = 0,
    [string]$image = "",
    [string]$hash = "",
    [string]$mcu_mgr = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$configPath = Join-Path $projectRoot "project_config.json"
$config = Get-Content -Path $configPath -Raw | ConvertFrom-Json

function Expand-ConfigValue {
    param([string]$Value)

    $result = $Value
    $map = @{
        "{BoardName}"   = [string]$config.Base.BoardName
        "{ProjectName}" = [string]$config.Base.ProjectName
        "{ProductId}"   = [string]$config.Base.ProductId
        "{DeviceId}"    = [string]$config.Base.DeviceId
        "{BuildDir}"    = [string]$config.Build.BuildDir
    }

    foreach ($key in $map.Keys) {
        $result = $result.Replace($key, $map[$key])
    }

    return $result
}

if ([string]::IsNullOrWhiteSpace($port)) {
    $port = [string]$config.Base.OtaTarget
}
if ($baud -eq 0) {
    $baud = [int]$config.Ota.Port
}
if ([string]::IsNullOrWhiteSpace($mcu_mgr)) {
    $mcu_mgr = [string]$config.Ota.McuMgr
}
if ([string]::IsNullOrWhiteSpace($image)) {
    $image = Join-Path $projectRoot (Expand-ConfigValue ([string]$config.Ota.AppSignedBinPath))
}

if ([string]::IsNullOrWhiteSpace($port)) {
    Write-Error "No serial port configured. Use -port COMx or set Base.OtaTarget in project_config.json."
}

$connection = "dev=$port,baud=$baud"

function Invoke-McuMgr {
    param([string[]]$Arguments)

    Write-Host ("{0} {1}" -f $mcu_mgr, ($Arguments -join " "))
    & $mcu_mgr @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

$baseArgs = @("--conntype", "serial", "--connstring", $connection)

switch ($action) {
    "upload" {
        if (-not (Test-Path -LiteralPath $image)) {
            Write-Error "Signed image not found: $image. Run .\dev.ps1 build -target all first."
        }
        Invoke-McuMgr ($baseArgs + @("image", "upload", $image))
        Write-Host "Upload finished. Mark the new slot1 hash as test image, then reset."
    }
    "list" {
        Invoke-McuMgr ($baseArgs + @("image", "list"))
    }
    "test" {
        if ([string]::IsNullOrWhiteSpace($hash)) {
            Write-Error "-hash is required for -action test (copy it from 'image list')."
        }
        Invoke-McuMgr ($baseArgs + @("image", "test", $hash))
        Invoke-McuMgr ($baseArgs + @("reset"))
    }
    "confirm" {
        if ([string]::IsNullOrWhiteSpace($hash)) {
            Invoke-McuMgr ($baseArgs + @("image", "confirm"))
        } else {
            Invoke-McuMgr ($baseArgs + @("image", "confirm", $hash))
        }
    }
    "reset" {
        Invoke-McuMgr ($baseArgs + @("reset"))
    }
}
