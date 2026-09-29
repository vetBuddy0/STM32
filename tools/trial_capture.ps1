<#
.SYNOPSIS
    Record a testing session from the wearable, with a label before each trial.

.DESCRIPTION
    Saves everything the board prints to a file, and lets you label each trial
    as you go: type a name, press Enter, then do the movement. The label is
    stamped with the board's own clock, so tools\analyse_trials.py can line the
    labels up with what the detector did.

    Name trials so the first word says what they are:

        fall 1          a simulated fall           (should be detected)
        walk 1          walking on the spot        (should not be)
        sit 1           sitting down firmly        (should not be)
        bend 1          bending to pick something up
        lower 1         lowering yourself slowly to the floor
        shake 1         shaking the board about

    Anything starting with "fall" counts as an attempted fall; everything else
    counts as an action that must not raise one.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\trial_capture.ps1
    powershell -ExecutionPolicy Bypass -File tools\trial_capture.ps1 -Port COM7
#>
param(
    [string]$Port = "COM6",
    [int]$Baud = 115200,
    [string]$Out = ""
)

if (-not $Out) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $Out = Join-Path (Join-Path $PSScriptRoot "..") "trials\trial_$stamp.log"
}
$folder = Split-Path $Out
if ($folder -and -not (Test-Path $folder)) {
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
}

$available = [System.IO.Ports]::SerialPort::GetPortNames 2>$null
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
if ($ports -notcontains $Port) {
    Write-Host "Port $Port not found. Ports available: $($ports -join ', ')"
    exit 1
}

$serial = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$serial.ReadTimeout = 150
$serial.NewLine = "`n"
try {
    $serial.Open()
} catch {
    Write-Host "Could not open ${Port}: $($_.Exception.Message)"
    exit 1
}

$writer = New-Object System.IO.StreamWriter($Out, $false)
$writer.AutoFlush = $true

Write-Host "Recording to $Out"
Write-Host "Type a label and press Enter before each trial, e.g. 'fall 1' or 'walk 1'."
Write-Host "Ctrl+C when you are done.`n"

$typed = ""
$lastTick = 0

try {
    while ($true) {
        # Take a batch of lines from the board, then look at the keyboard, so
        # a steady stream of CSV rows cannot starve the typing.
        for ($i = 0; $i -lt 60; $i++) {
            try {
                $line = $serial.ReadLine().TrimEnd("`r")
            } catch [TimeoutException] {
                break
            }

            $writer.WriteLine($line)

            # Remember the board's own clock from the CSV rows, so a label can
            # be stamped with it rather than with the laptop's time.
            if ($line -match '^(\d+),') { $lastTick = [int]$Matches[1] }

            # Only the interesting lines go on screen; the CSV rows would
            # scroll everything else away.
            if ($line -like "##*" -or $line -like "net:*" -or $line -like "WARNING*") {
                Write-Host $line -ForegroundColor Cyan
            }
        }

        while ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            switch ($key.Key) {
                'Enter' {
                    if ($typed.Trim()) {
                        $mark = "# MARK t=$lastTick $($typed.Trim())"
                        $writer.WriteLine($mark)
                        Write-Host $mark -ForegroundColor Yellow
                    }
                    $typed = ""
                }
                'Backspace' {
                    if ($typed.Length -gt 0) {
                        $typed = $typed.Substring(0, $typed.Length - 1)
                        Write-Host "`b `b" -NoNewline
                    }
                }
                default {
                    if ($key.KeyChar -and -not [char]::IsControl($key.KeyChar)) {
                        $typed += $key.KeyChar
                        Write-Host $key.KeyChar -NoNewline
                    }
                }
            }
        }
    }
} finally {
    $writer.Close()
    $serial.Close()
    Write-Host "`nsaved $Out"
}
