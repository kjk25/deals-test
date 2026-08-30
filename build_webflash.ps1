$ErrorActionPreference = "Stop"

$buildDir = Join-Path $PSScriptRoot ".pio\build\esp32dev"
$releaseDir = Join-Path $PSScriptRoot "release"
$version = (Get-Content -LiteralPath (Join-Path $PSScriptRoot "VERSION.txt") -Raw).Trim()
$safeVersion = ($version -replace '[^0-9A-Za-z]+', '_').Trim('_')
$output = Join-Path $releaseDir "DEALS_V${safeVersion}_webflash.bin"
$bootApp0 = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string] $Command,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Arguments
  )

  & $Command @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Befehl fehlgeschlagen: $Command $($Arguments -join ' ')"
  }
}

Invoke-Checked pio run
Invoke-Checked pio run -t buildfs

New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

$mergeArgs = @(
  "pkg", "exec", "-p", "tool-esptoolpy", "--",
  "esptool.py", "--chip", "esp32", "merge_bin", "-o", $output,
  "0x1000", (Join-Path $buildDir "bootloader.bin"),
  "0x8000", (Join-Path $buildDir "partitions.bin"),
  "0xe000", $bootApp0,
  "0x10000", (Join-Path $buildDir "firmware.bin"),
  "0x290000", (Join-Path $buildDir "littlefs.bin")
)

Invoke-Checked pio @mergeArgs

$profilePaths = @(
  $env:USERPROFILE,
  ($env:USERPROFILE -replace '\\', '/')
) | Where-Object { $_ } | Select-Object -Unique

$bytes = [IO.File]::ReadAllBytes($output)
foreach ($profilePath in $profilePaths) {
  $needle = [Text.Encoding]::ASCII.GetBytes($profilePath)
  if ($needle.Length -eq 0) {
    continue
  }

  for ($i = 0; $i -le $bytes.Length - $needle.Length; $i++) {
    $match = $true
    for ($j = 0; $j -lt $needle.Length; $j++) {
      if ($bytes[$i + $j] -ne $needle[$j]) {
        $match = $false
        break
      }
    }

    if ($match) {
      $bytes[$i] = [byte][char]'.'
      for ($j = 1; $j -lt $needle.Length; $j++) {
        $bytes[$i + $j] = 0
      }
      $i += $needle.Length - 1
    }
  }
}
[IO.File]::WriteAllBytes($output, $bytes)

Write-Host "DEALS Web-Flasher BIN erstellt: $output" -ForegroundColor Green
