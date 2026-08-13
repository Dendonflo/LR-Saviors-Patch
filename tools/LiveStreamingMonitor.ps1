# Live, read-only monitor for LRFF13.exe's internal asset-streaming counters.
#
# Background: static analysis of the binary (see ../tools/ghidra_output) found
# a set of global microsecond-accumulator counters that the game's own
# BgLoader/AsyncLoader code updates around its synchronous load steps, plus a
# 128-slot async job ring buffer with a live pending-count field. These are
# reset to 0 every time a new "MAPSET" (area load) begins and accumulate
# until it finishes, at which point the game itself logs a
# "[MAPSET LOG<n> id=<...> time=<...>]" debug line internally.
#
# This script attaches read-only (ReadProcessMemory, no writes, no injection,
# no code execution in the target) and polls those same counters plus the
# AsyncLoader queue depth, logging them with high-resolution timestamps so we
# can correlate real, felt stutters while playing with what the engine itself
# thinks is happening on the Loader thread.
#
# Addresses below are Ghidra static VAs (module preferred base 0x00400000).
# The PE is flagged DYNAMIC_BASE, so at runtime we compute:
#     actual = runtime_module_base + (ghidra_va - 0x00400000)

param(
    [int]$PollIntervalMs = 50,
    [string]$LogPath = "$PSScriptRoot\..\ghidra_output\live_monitor_log.csv"
)

$ProcName = "LRFF13"
$GhidraBase = 0x00400000

# RVA offsets (ghidra_va - GhidraBase) for the six load-stage accumulators
# found bracketing timeGetTime() calls inside BgLoader::prefetch_map and its
# callees (loader_decompiled.txt), all zeroed together in BgLoader_misc_ref
# (FUN_00496f20) at the start of each area/MAPSET load:
$Counters = [ordered]@{
    "sync_file_load_us"   = 0x0239a0ec - $GhidraBase  # main synchronous FUN_009ed080 wait
    "spinwait_sleep1_us"  = 0x0239a0f4 - $GhidraBase  # Sleep(1) polling loop total (Loader thread spin-wait)
    "subtask_wait_a_us"   = 0x0239a0fc - $GhidraBase
    "bg_unload_block_us"  = 0x0239a100 - $GhidraBase  # BgLoader::unload_block bracket
    "subtask_wait_b_us"   = 0x0239a0f8 - $GhidraBase
    "phys_setup_us"       = 0x0239a0f0 - $GhidraBase
}

# AsyncLoader object pointer (set once at init: DAT_024c3b30 = new AsyncLoader()).
# Ring buffer fields inside that object: +0x58 head(u16), +0x5a tail(u16), +0x5c pending count(i16).
$AsyncLoaderPtrRva = 0x024c3b30 - $GhidraBase

Add-Type @"
using System;
using System.Runtime.InteropServices;
namespace Native {
    [StructLayout(LayoutKind.Sequential)]
    public struct MODULEENTRY32 {
        public uint dwSize;
        public uint th32ModuleID;
        public uint th32ProcessID;
        public uint GlblcntUsage;
        public uint ProccntUsage;
        public IntPtr modBaseAddr;
        public uint modBaseSize;
        public IntPtr hModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string szModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szExePath;
    }

    public static class Win32 {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool ReadProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, int dwSize, out int lpNumberOfBytesRead);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr CreateToolhelp32Snapshot(uint dwFlags, uint th32ProcessID);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool Module32First(IntPtr hSnapshot, ref MODULEENTRY32 lpme);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);
    }
}
"@

# Robustly find the module base address via Toolhelp32 snapshot instead of
# Process.MainModule, which can silently return null when the host PowerShell
# process and the target game process differ in bitness (64-bit PS reading a
# 32-bit game), a known .NET/PowerShell quirk. SNAPMODULE32 explicitly exists
# to handle exactly this WOW64 case.
function Get-ModuleBase([int]$targetProcessId) {
    $TH32CS_SNAPMODULE = 0x8
    $TH32CS_SNAPMODULE32 = 0x10
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $snap = [Native.Win32]::CreateToolhelp32Snapshot($TH32CS_SNAPMODULE -bor $TH32CS_SNAPMODULE32, [uint32]$targetProcessId)
        if ($snap -ne [IntPtr]::Zero -and $snap.ToInt64() -ne -1) {
            $me = New-Object Native.MODULEENTRY32
            $me.dwSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type]"Native.MODULEENTRY32")
            $ok = [Native.Win32]::Module32First($snap, [ref]$me)
            [Native.Win32]::CloseHandle($snap) | Out-Null
            if ($ok) { return $me.modBaseAddr.ToInt64() }
        }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Read-Dword($handle, [long]$addr) {
    $buf = New-Object byte[] 4
    $n = 0
    [Native.Win32]::ReadProcessMemory($handle, [IntPtr]$addr, $buf, 4, [ref]$n) | Out-Null
    if ($n -ne 4) { return $null }
    return [BitConverter]::ToUInt32($buf, 0)
}

function Read-Int16($handle, [long]$addr) {
    $buf = New-Object byte[] 2
    $n = 0
    [Native.Win32]::ReadProcessMemory($handle, [IntPtr]$addr, $buf, 2, [ref]$n) | Out-Null
    if ($n -ne 2) { return $null }
    return [BitConverter]::ToInt16($buf, 0)
}

Write-Host "Waiting for $ProcName.exe to start..."
$proc = $null
while (-not $proc) {
    $proc = Get-Process -Name $ProcName -ErrorAction SilentlyContinue
    if (-not $proc) { Start-Sleep -Milliseconds 500 }
}
$base = Get-ModuleBase $proc.Id
if (-not $base) {
    Write-Error "Could not determine module base address for PID $($proc.Id) after retries. Try running this PowerShell window as Administrator."
    exit 1
}
Write-Host "Found process PID $($proc.Id). Module base: 0x$($base.ToString('X'))"
$handle = $proc.Handle

New-Item -ItemType Directory -Force -Path (Split-Path $LogPath) | Out-Null
"timestamp_ms,sync_file_load_us,spinwait_sleep1_us,subtask_wait_a_us,bg_unload_block_us,subtask_wait_b_us,phys_setup_us,queue_pending" | Out-File -FilePath $LogPath -Encoding utf8

Write-Host "Logging every ${PollIntervalMs}ms to $LogPath"
Write-Host "Walk around the stuttery areas now. Press Ctrl+C to stop."

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$lastVals = @{}
foreach ($k in $Counters.Keys) { $lastVals[$k] = -1 }
$lastQueue = -1

while ($true) {
    if ($proc.HasExited) { Write-Host "Process exited."; break }

    $vals = @{}
    foreach ($k in $Counters.Keys) {
        $addr = $base + $Counters[$k]
        $vals[$k] = Read-Dword $handle $addr
    }

    $queuePending = $null
    $loaderPtr = Read-Dword $handle ($base + $AsyncLoaderPtrRva)
    if ($loaderPtr -and $loaderPtr -ne 0) {
        $queuePending = Read-Int16 $handle ($loaderPtr + 0x5c)
    }

    $changed = $false
    foreach ($k in $Counters.Keys) {
        if ($vals[$k] -ne $lastVals[$k]) { $changed = $true }
    }
    if ($queuePending -ne $lastQueue) { $changed = $true }

    if ($changed) {
        $t = $sw.ElapsedMilliseconds
        $line = "$t," + (($Counters.Keys | ForEach-Object { $vals[$_] }) -join ",") + ",$queuePending"
        $line | Out-File -FilePath $LogPath -Append -Encoding utf8
        Write-Host ("[{0,7}ms] sync_load={1,6}us  spinwait={2,6}us  bg_unload={3,6}us  queue_pending={4}" -f `
            $t, $vals["sync_file_load_us"], $vals["spinwait_sleep1_us"], $vals["bg_unload_block_us"], $queuePending)
        foreach ($k in $Counters.Keys) { $lastVals[$k] = $vals[$k] }
        $lastQueue = $queuePending
    }

    Start-Sleep -Milliseconds $PollIntervalMs
}
