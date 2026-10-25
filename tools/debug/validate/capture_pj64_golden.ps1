# Capture hardware-truth RDRAM goldens from Project64 4.0 Dev via its JS debugger API.
# Uses a portable copy of PJ64 in build\pj64 (copy "C:\Program Files (x86)\Project64 4.0 Dev" there,
# set "Autorun Scripts=pj64_rs64_dump.js" under [Debugger] in build\pj64\Config\Project64.cfg,
# keep Force Interpreter CPU=1 so events.onexec fires). Dumps land in dumps\pj64\rdram_<tag>.bin.
#
#   .\tools\debug\validate\capture_pj64_golden.ps1                 # 45 s run, prints the script console
#   .\tools\debug\validate\capture_pj64_golden.ps1 -Seconds 120
param(
    [int]$Seconds = 45,
    [string]$Rom = "F:\ROMS\n64\Star Wars - Rogue Squadron (USA).n64",
    [string]$Pj64Dir = "build\pj64"
)
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$dir = Join-Path $root $Pj64Dir
Copy-Item (Join-Path $PSScriptRoot "pj64_rs64_dump.js") (Join-Path $dir "Scripts\pj64_rs64_dump.js") -Force
New-Item -ItemType Directory -Force (Join-Path $root "dumps\pj64") | Out-Null
Add-Type -AssemblyName UIAutomationClient; Add-Type -AssemblyName UIAutomationTypes
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Pj64Win { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,int msg,IntPtr w,IntPtr l); }
'@
$p = Start-Process -FilePath (Join-Path $dir "Project64.exe") -ArgumentList ('"' + $Rom + '"') -WorkingDirectory $dir -PassThru
Start-Sleep -Seconds $Seconds
[Pj64Win]::PostMessage($p.MainWindowHandle, 0x111, [IntPtr]4213, [IntPtr]::Zero) | Out-Null   # Debugger > Scripts...
Start-Sleep -Seconds 2
$A = [System.Windows.Automation.AutomationElement]; $T = [System.Windows.Automation.TreeScope]
$pidCond = New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $p.Id)
foreach ($w in $A::RootElement.FindAll($T::Children, $pidCond)) {
    if ($w.Current.Name -eq 'Scripts') {
        $ed = $w.FindFirst($T::Descendants, (New-Object System.Windows.Automation.PropertyCondition($A::AutomationIdProperty, '1271')))
        "script console:`n$($ed.Current.Name)"
    }
}
Stop-Process -Id $p.Id -Force
Get-ChildItem (Join-Path $root "dumps\pj64") -Filter *.bin | ForEach-Object { "{0,9} {1}" -f $_.Length, $_.Name }
