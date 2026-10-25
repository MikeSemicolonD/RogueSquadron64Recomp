# One-shot: launch the game and drive boot -> profile -> level -> craft -> hangar,
# NOTE: This assume we're not saving the profile on creation, on real hardware it saves when created
# landing on the (currently black) FrontEnd hangar cutscene. Single input run, no
# intermediate checks. Extra env can be forwarded for diagnostics.
#   pwsh tools/debug/repro-hangar.ps1 -Env "ROGUESQ_LOG_GBI=1"
param(
  [string]$Env = "",                 # semicolon-separated NAME=VALUE forwarded to the game
  [int]$BootMs = 11500,              # wait after launch before the first input
  [string]$Title = "Star Wars: Rogue Squadron 64 Recompiled"
)

$ErrorActionPreference = "Stop"
$root = "E:\Projects\RogueSquadron64Recomp"
$stamp = Get-Date -Format "HHmmss"
$log = "$root\dumps\hangar-session\repro_$stamp.txt"
"latest=$log" | Set-Content "$root\dumps\hangar-session\latest.txt"

Get-Process RogueSquadron64Recomp -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600

# Build the child launcher (env + redirected log) as a scriptblock string.
$envLines = "`$env:ROGUESQ_LOG_GAMESTATE='1'; `$env:ROGUESQ_LOG_MENU_FIX='1';"
foreach ($kv in ($Env -split ';')) {
  if ($kv.Trim() -and $kv.Contains('=')) {
    $n,$v = $kv.Split('=',2); $envLines += " `$env:$($n.Trim())='$($v.Trim())';"
  }
}
$child = "$envLines Set-Location '$root\build\Debug'; & '.\RogueSquadron64Recomp.exe' *>&1 | Out-File -Encoding utf8 '$log'"
$b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($child))
Start-Process powershell -ArgumentList "-NoProfile","-EncodedCommand",$b64
Write-Output "launched, log=$log"

Start-Sleep -Milliseconds $BootMs

# --- input driver (SendInput scancodes) ---
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SI2 {
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KI ki; public int pad; }
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk; public ushort scan; public uint flags; public uint time; public IntPtr extra; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] p, int cb);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  public static void Key(ushort scan, bool up){ INPUT[] a=new INPUT[1]; a[0].type=1; a[0].ki.scan=scan; a[0].ki.flags=8|(up?(uint)2:0); SendInput(1,a,Marshal.SizeOf(typeof(INPUT))); }
  public static IntPtr Focus(string t){ IntPtr h=FindWindow(null,t); if(h!=IntPtr.Zero){ ShowWindow(h,9); SetForegroundWindow(h);} return h; }
}
"@
# Default keys: Esc = Start, Enter = A.
$START=0x01; $A=0x1C
function Tap($scan,$hold=90){ [SI2]::Key([uint16]$scan,$false); Start-Sleep -Milliseconds $hold; [SI2]::Key([uint16]$scan,$true) }

$h = [SI2]::Focus($Title)
if ($h -eq [IntPtr]::Zero) { Write-Output "WINDOW NOT FOUND"; exit 1 }
Start-Sleep -Milliseconds 200

# Skip the intro cinematic to the title/save-select. Several spaced Starts.
1..4 | ForEach-Object { Tap $START; Start-Sleep -Milliseconds 1600 }
Start-Sleep -Milliseconds 500
# SELECT GAME -> ENTER NAME -> type 2 letters -> confirm name
Tap $A; Start-Sleep -Milliseconds 1600       # pick empty slot
Tap $A; Start-Sleep -Milliseconds 700        # letter
Tap $A; Start-Sleep -Milliseconds 700        # letter
Tap $START; Start-Sleep -Milliseconds 2000   # finish name -> ARE YOU SURE
Tap $A; Start-Sleep -Milliseconds 2000       # YES -> SELECT LEVEL
Tap $A; Start-Sleep -Milliseconds 1700       # pick level -> AVAILABLE CRAFT
Tap $A; Start-Sleep -Milliseconds 600        # select craft
Tap $A; Start-Sleep -Milliseconds 600        # confirm mission -> hangar
Tap $A;                                      # one more for good luck
Write-Output "sequence complete, log=$log"
