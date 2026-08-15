# Live watcher for the Gysahl planting instrumentation.
#
# Tails SaviorsPatch.log and shows ONLY the planting-relevant lines, colour
# coded, so the interesting events are readable while playing. The raw log is
# thousands of lines of frame/monitor spam per minute; this filters it down to
# roughly a handful of lines per planting.
#
# Run in a PowerShell window beside the game:
#     powershell -ExecutionPolicy Bypass -File "<this file>"
#
# Ctrl+C to stop. Safe to start/stop at any time; it does not touch the file.

$log = "C:\Program Files (x86)\Steam\steamapps\common\LIGHTNING RETURNS FINAL FANTASY XIII\SaviorsPatch.log"

Write-Host ""
Write-Host "  Gysahl planting watcher" -ForegroundColor White
Write-Host "  ----------------------------------------------------------" -ForegroundColor DarkGray
Write-Host "  GREEN  [fa-FIX]    the repair fired - seed should still appear" -ForegroundColor Green
Write-Host "  CYAN   planting    a planting started" -ForegroundColor Cyan
Write-Host "  RED    empty arg   the bug happening (repair should follow)" -ForegroundColor Red
Write-Host "  GRAY   everything else in the trace" -ForegroundColor DarkGray
Write-Host "  ----------------------------------------------------------" -ForegroundColor DarkGray
Write-Host ""

while (-not (Test-Path $log)) {
    Write-Host "  waiting for the game to create the log..." -ForegroundColor DarkYellow
    Start-Sleep -Seconds 2
}

$fixes = 0
$plants = 0

Get-Content -Path $log -Tail 0 -Wait -ErrorAction Continue | ForEach-Object {
    $line = $_
    if ($line -notmatch '\[fa') { return }
    # The VM interpreter trace is extremely high volume (the buffer oscillates
    # between the plot name and other frames' data dozens of times per
    # planting). Keep only the transitions that involve the plot name itself,
    # otherwise the events worth seeing scroll past in a wall of grey.
    if ($line -match '\[fa-vm\]' -and $line -notmatch 'pm_faoF03') { return }

    if ($line -match '\[fa-FIX\]') {
        $fixes++
        Write-Host ""
        Write-Host "  *** REPAIR #$fixes FIRED ***" -ForegroundColor Green
        Write-Host "  $line" -ForegroundColor Green
        Write-Host "  -> check in game: did the seed appear on the plot?" -ForegroundColor Green
        Write-Host ""
    }
    elseif ($line -match 'planting started') {
        $plants++
        Write-Host ""
        Write-Host "  --- planting #$plants ---" -ForegroundColor Cyan
    }
    elseif ($line -match "stringComp\(a=[^']*''") {
        # first argument came through empty - this is the bug itself
        Write-Host "  $line" -ForegroundColor Red
    }
    elseif ($line -match '-> fsh_fao_yasai1') {
        Write-Host "  $line" -ForegroundColor White
    }
    else {
        Write-Host "  $line" -ForegroundColor DarkGray
    }
}
