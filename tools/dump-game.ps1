# Take a full-memory minidump of the running RogueSquadron64Recomp.exe.
# Usage: open PowerShell in the project root, run:  .\dump-game.ps1
# Works even if the game's GUI is frozen (Not Responding) — uses kernel API
# so it doesn't depend on the target's message pump.

$proc = Get-Process -Name RogueSquadron64Recomp -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Error "RogueSquadron64Recomp.exe not running."
    exit 1
}

$ts = Get-Date -Format "yyyyMMdd_HHmmss"
$path = Join-Path (Get-Location) "crash_${ts}_external.dmp"

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class DumpHelper {
    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool MiniDumpWriteDump(
        IntPtr hProcess, uint pid, IntPtr hFile,
        int dumpType, IntPtr expParam, IntPtr userParam, IntPtr callback);
}
"@

# 0x00000002 = MiniDumpWithFullMemory (captures rdram contents)
$file = [System.IO.File]::Create($path)
try {
    $ok = [DumpHelper]::MiniDumpWriteDump(
        $proc.Handle, [uint32]$proc.Id, $file.SafeFileHandle.DangerousGetHandle(),
        0x00000002, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($ok) {
        Write-Output "Wrote $path ($([math]::Round((Get-Item $path).Length / 1MB, 1)) MB)"
    } else {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Error "MiniDumpWriteDump failed (Win32 err=$err)"
    }
} finally {
    $file.Close()
}
