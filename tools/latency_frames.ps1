# Boot to the main menu, tap a key, and save the menu column every 100 ms for 2 s.
param([int]$BootSeconds = 25, [int]$MenuSeconds = 12, [string]$Key = "Down", [string]$OutDir = "$PSScriptRoot\..\work\snaps\latency")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W5 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; } }
"@
$vk = @{ Return = 0x0D; Down = 0x28; Up = 0x26 }
New-Item -ItemType Directory -Force $OutDir | Out-Null
Set-Location (Join-Path $PSScriptRoot "..\out\build\win-amd64-relwithdebinfo")
$p = Start-Process .\skate1.exe -PassThru
Start-Sleep -Seconds $BootSeconds
$p.Refresh(); [W5]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300
[W5]::keybd_event(0x0D, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W5]::keybd_event(0x0D, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Seconds $MenuSeconds
$r = New-Object W5+RECT; [W5]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
[W5]::keybd_event($vk[$Key], 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W5]::keybd_event($vk[$Key], 0, 2, [UIntPtr]::Zero)
$frames = @()
while ($sw.ElapsedMilliseconds -lt 2000) {
  $bmp = New-Object System.Drawing.Bitmap 260, 300
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.Left + 280, $r.Top + 180, 0, 0, $bmp.Size)
  $frames += ,@($sw.ElapsedMilliseconds, $bmp)
  Start-Sleep -Milliseconds 80
}
Stop-Process -Id $p.Id -Force
foreach ($f in $frames) { $f[1].Save((Join-Path $OutDir ("t{0:D4}.png" -f $f[0]))) }
"saved $($frames.Count) frames"
