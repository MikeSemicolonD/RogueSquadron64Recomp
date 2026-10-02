# Post N64-mapped keystrokes to one game window (WM_KEYDOWN/WM_KEYUP with scancodes, which SDL reads).
# Targets the window of -ProcessId (default: the only running RogueSquadron64Recomp); never focuses it or uses global input, so the desktop keyboard is untouched.
#
#   pwsh tools/drive-input.ps1 -ProcessId 1234 -Keys "enter:100:1500,1,period"
#
# Each token is  name:holdMs:afterMs  (holdMs/afterMs optional; default 90/400). Bindings come from roguesq_input.json.
param(
  [string]$Keys = "",
  [int]$ProcessId = 0
)
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class DrvIn {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, p) => {
      uint wp; GetWindowThreadProcessId(h, out wp);
      if (wp != pid || !IsWindowVisible(h)) return true;
      StringBuilder c = new StringBuilder(64); GetClassName(h, c, 64);
      if (c.ToString() != "SDL_app") return true;
      found = h; return false;
    }, IntPtr.Zero);
    return found;
  }
  public static void Key(IntPtr h, ushort vk, ushort scan, bool ext, bool up) {
    uint l = 1u | ((uint)scan << 16) | (ext ? (1u << 24) : 0u);
    if (up) l |= (1u << 30) | (1u << 31);
    PostMessage(h, up ? 0x101u : 0x100u, (IntPtr)vk, (IntPtr)(int)l);
  }
}
"@
# name = (virtual key, scancode set 1, isExtended)
$sc = @{ 'enter'=@(0x0D,0x1C,$false);'space'=@(0x20,0x39,$false);'esc'=@(0x1B,0x01,$false);
  'up'=@(0x26,0x48,$true);'down'=@(0x28,0x50,$true);'left'=@(0x25,0x4B,$true);'right'=@(0x27,0x4D,$true);
  'w'=@(0x57,0x11,$false);'a'=@(0x41,0x1E,$false);'s'=@(0x53,0x1F,$false);'d'=@(0x44,0x20,$false);
  'q'=@(0x51,0x10,$false);'e'=@(0x45,0x12,$false);'i'=@(0x49,0x17,$false);'j'=@(0x4A,0x24,$false);'k'=@(0x4B,0x25,$false);'l'=@(0x4C,0x26,$false);
  'lshift'=@(0xA0,0x2A,$false);'lctrl'=@(0xA2,0x1D,$false);'f1'=@(0x70,0x3B,$false);'f5'=@(0x74,0x3F,$false);'f6'=@(0x75,0x40,$false);'f7'=@(0x76,0x41,$false);'f8'=@(0x77,0x42,$false);
  '1'=@(0x31,0x02,$false);'2'=@(0x32,0x03,$false);'3'=@(0x33,0x04,$false);'4'=@(0x34,0x05,$false);'5'=@(0x35,0x06,$false);'6'=@(0x36,0x07,$false);'7'=@(0x37,0x08,$false);'8'=@(0x38,0x09,$false);'9'=@(0x39,0x0A,$false);'0'=@(0x30,0x0B,$false);
  'period'=@(0xBE,0x34,$false);'backspace'=@(0x08,0x0E,$false); }

if ($ProcessId -eq 0) {
  $procs = @(Get-Process RogueSquadron64Recomp -ErrorAction SilentlyContinue)
  if ($procs.Count -ne 1) { Write-Output "PASS -ProcessId ($($procs.Count) game processes running)"; exit 1 }
  $ProcessId = $procs[0].Id
}
$h = [DrvIn]::Find([uint32]$ProcessId)
if ($h -eq [IntPtr]::Zero) { Write-Output "WINDOW NOT FOUND for pid $ProcessId"; exit 1 }
foreach ($tok in ($Keys -split ',')) {
  if (-not $tok.Trim()) { continue }
  $p = $tok.Trim() -split ':'; $name = $p[0].ToLower()
  $hold  = if ($p.Count -gt 1) { [int]$p[1] } else { 90 }
  $after = if ($p.Count -gt 2) { [int]$p[2] } else { 400 }
  if (-not $sc.ContainsKey($name)) { Write-Output "UNKNOWN KEY: $name"; continue }
  $k = $sc[$name]
  [DrvIn]::Key($h, [uint16]$k[0], [uint16]$k[1], [bool]$k[2], $false); Start-Sleep -Milliseconds $hold
  [DrvIn]::Key($h, [uint16]$k[0], [uint16]$k[1], [bool]$k[2], $true)
  Write-Output "pressed $name"; Start-Sleep -Milliseconds $after
}
