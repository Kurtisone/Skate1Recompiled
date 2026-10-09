# Launch skate1.exe, press Start (Enter, with mnk_mode on) after a delay, screenshot, and leave it running.
param([int]$Delay = 35, [int[]]$SnapAfter = @(5, 20), [string]$OutDir = "$PSScriptRoot\..\work\snaps")

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Win2 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; } }
"@
New-Item -ItemType Directory -Force $OutDir | Out-Null
function Snap($p, $name) {
  $p.Refresh(); $h = $p.MainWindowHandle
  $r = New-Object Win2+RECT; [Win2]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $f = Join-Path $OutDir $name; $bmp.Save($f); "saved $f"
}

Set-Location (Join-Path $PSScriptRoot '..\out\build\win-amd64-relwithdebinfo')
$p = Start-Process .\skate1.exe -PassThru
Start-Sleep -Seconds $Delay
if ($p.HasExited) { "exited before pressing Start"; exit 1 }
$p.Refresh(); [Win2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
Snap $p 'before_start.png'
[Win2]::keybd_event(0x0D, 0x1C, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 150
[Win2]::keybd_event(0x0D, 0x1C, 2, [UIntPtr]::Zero)
"pressed Start at ${Delay}s"
$t0 = Get-Date
foreach ($s in $SnapAfter) {
  while (((Get-Date) - $t0).TotalSeconds -lt $s -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
  if ($p.HasExited) { "exited $([int]((Get-Date) - $t0).TotalSeconds)s after Start"; exit 1 }
  Snap $p "after_start_${s}s.png"
}
"still running (pid $($p.Id))"
