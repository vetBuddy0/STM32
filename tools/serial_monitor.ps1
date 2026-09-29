<#
.SYNOPSIS
    Show what the wearable prints on its serial port.

.DESCRIPTION
    The board reports over USART1 through the ST-Link's virtual COM port, at
    115200 baud. Everything the firmware says - CSV rows, "##" incident lines,
    "net:" link lines - arrives here. Nothing is sent to the board.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\serial_monitor.ps1
    powershell -ExecutionPolicy Bypass -File tools\serial_monitor.ps1 -Port COM7
#>
param(
    [string]$Port = "COM6",
    [int]$Baud = 115200,
    [switch]$Timestamps
)

$available = [System.IO.Ports.SerialPort]::GetPortNames()
if ($available -notcontains $Port) {
    Write-Host "Port $Port not found. Ports available: $($available -join ', ')"
    Write-Host "The ST-Link port is the one that disappears when you unplug the board."
    exit 1
}

$serial = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$serial.ReadTimeout = 500
$serial.NewLine = "`n"

try {
    $serial.Open()
} catch {
    Write-Host "Could not open ${Port}: $($_.Exception.Message)"
    Write-Host "Something else is probably holding it - close any other terminal."
    exit 1
}

Write-Host "Listening on $Port at $Baud baud. Ctrl+C to stop."
Write-Host ""

try {
    while ($true) {
        try {
            $line = $serial.ReadLine().TrimEnd("`r")
            if ($Timestamps) {
                Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $line)
            } else {
                Write-Host $line
            }
        } catch [TimeoutException] {
            # Nothing arrived in the last half second; keep waiting.
        }
    }
} finally {
    $serial.Close()
    Write-Host "`nclosed $Port"
}
