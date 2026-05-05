# Resolve a list of RVAs against a PE+PDB pair using the local DbgHelp APIs.
# Usage: .\resolve-rva.ps1 53F49 10C77   (no 0x prefix — PowerShell auto-converts those to decimal)

[CmdletBinding()]
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$RvaList)

if (-not $RvaList -or $RvaList.Count -lt 1) {
    Write-Error "Usage: .\resolve-rva.ps1 <rva-hex> [<rva-hex> ...]   (no 0x prefix)"
    exit 1
}

$exe = (Resolve-Path .\build\Debug\RogueSquadron64Recomp.exe).Path
$pdbDir = (Resolve-Path .\build\Debug).Path

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

[StructLayout(LayoutKind.Sequential, Pack=1, CharSet=CharSet.Ansi)]
public struct SYMBOL_INFO {
    public uint   SizeOfStruct;
    public uint   TypeIndex;
    public ulong  Reserved0;
    public ulong  Reserved1;
    public uint   Index;
    public uint   Size;
    public ulong  ModBase;
    public uint   Flags;
    public ulong  Value;
    public ulong  Address;
    public uint   Register;
    public uint   Scope;
    public uint   Tag;
    public uint   NameLen;
    public uint   MaxNameLen;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=512)] public string Name;
}

[StructLayout(LayoutKind.Sequential, Pack=1, CharSet=CharSet.Ansi)]
public struct IMAGEHLP_LINE64 {
    public uint   SizeOfStruct;
    public IntPtr Key;
    public uint   LineNumber;
    public IntPtr FileName;
    public ulong  Address;
}

public static class Sym {
    [DllImport("dbghelp.dll", CharSet=CharSet.Ansi, SetLastError=true)]
    public static extern bool SymInitialize(IntPtr h, string searchPath, bool invadeProcess);
    [DllImport("dbghelp.dll", CharSet=CharSet.Ansi, SetLastError=true)]
    public static extern ulong SymLoadModuleEx(IntPtr h, IntPtr file, string image, string mod, ulong baseOfDll, uint sizeOfDll, IntPtr data, uint flags);
    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool SymFromAddr(IntPtr h, ulong addr, out ulong disp, ref SYMBOL_INFO sym);
    [DllImport("dbghelp.dll", SetLastError=true)]
    public static extern bool SymGetLineFromAddr64(IntPtr h, ulong addr, out uint lineDisp, ref IMAGEHLP_LINE64 line);
    [DllImport("dbghelp.dll")]
    public static extern uint SymSetOptions(uint options);
    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")]
    public static extern IntPtr LoadLibraryA(string name);
}
"@

# 0x10 = LOAD_LINES, 0x4 = DEFERRED_LOADS, 0x40000000 = UNDNAME
[void][Sym]::SymSetOptions(0x10 -bor 0x4 -bor 0x40000000)
$proc = [Sym]::GetCurrentProcess()
[void][Sym]::SymInitialize($proc, $pdbDir, $false)

$loadBase = [uint64]0x10000000
$base = [Sym]::SymLoadModuleEx($proc, [IntPtr]::Zero, $exe, "main", $loadBase, 0, [IntPtr]::Zero, 0)
if ($base -eq 0) {
    Write-Error "SymLoadModuleEx failed"
    exit 1
}

foreach ($a in $RvaList) {
    $rvaStr = "$a"  # force string
    if ($rvaStr.StartsWith("0x") -or $rvaStr.StartsWith("0X")) { $rvaStr = $rvaStr.Substring(2) }
    $rva = [Convert]::ToUInt64($rvaStr, 16)
    $addr = $base + $rva

    $sym = New-Object SYMBOL_INFO
    $sym.SizeOfStruct = 88   # offsetof(Name)
    $sym.MaxNameLen = 511
    [uint64]$disp = 0
    $okSym = [Sym]::SymFromAddr($proc, $addr, [ref]$disp, [ref]$sym)

    $line = New-Object IMAGEHLP_LINE64
    $line.SizeOfStruct = [System.Runtime.InteropServices.Marshal]::SizeOf([type][IMAGEHLP_LINE64])
    [uint32]$lineDisp = 0
    $okLine = [Sym]::SymGetLineFromAddr64($proc, $addr, [ref]$lineDisp, [ref]$line)

    Write-Output ""
    Write-Output "RVA 0x$($rva.ToString('X')) base=0x$($base.ToString('X')) addr=0x$($addr.ToString('X'))"
    if ($okSym) {
        Write-Output "  symbol: $($sym.Name) +0x$($disp.ToString('X'))"
    } else {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Output "  symbol: <none> err=$err"
    }
    if ($okLine) {
        $fname = [System.Runtime.InteropServices.Marshal]::PtrToStringAnsi($line.FileName)
        Write-Output "  line:   $fname : $($line.LineNumber)"
    }
}
