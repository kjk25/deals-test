$ErrorActionPreference = "Stop"
pio run
pio run -t upload
pio run -t uploadfs
Write-Host "DEALS: Firmware und LittleFS erfolgreich hochgeladen." -ForegroundColor Green
