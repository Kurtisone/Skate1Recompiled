# Measure menu input latency: boot to the main menu, then press Down several times and time
# how long until the menu arrow column changes on screen.
param([int]$BootSeconds = 25, [int]$MenuSeconds = 12, [int]$Trials = 6, [string]$Exe = "skate1",
      [int]$X0 = 280, [int]$X1 = 360, [int]$Y0 = 180, [int]$Y1 = 480, [string]$Key = "Down")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W4 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; } }
"@
$vk = @{ Return = 0x0D; Down = 0x28; Up = 0x26 }
$dir = Join-Path $PSScriptRoot "..\out\build\win-amd64-relwithdebinfo"
if ($Exe -ne "skate1") { $dir = $Exe; $Exe = Split-Path $dir -Leaf; $dir = Split-Path $dir -Parent }
Set-Location $dir
$p = Start-Process ".\$Exe.exe" -PassThru
function Tap($k) { [W4]::keybd_event($vk[$k], 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [W4]::keybd_event($vk[$k], 0, 2, [UIntPtr]::Zero) }
function Grab($r) {
  $w = $X1 - $X0; $h = $Y1 - $Y0
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.Left + $X0, $r.Top + $Y0, 0, 0, $bmp.Size)
  $sy = 0L; $n = 0
  for ($y = 0; $y -lt $h; $y += 3) { for ($x = 0; $x -lt $w; $x += 3) { $c = $bmp.GetPixel($x, $y); if ($c.G -gt $c.R + 30 -and $c.G -gt $c.B + 60) { $sy += $y; $n++ } } }
  $sum = if ($n -gt 5) { [int]($sy / $n) } else { -1 }
  $bmp.Dispose(); return $sum
}
Start-Sleep -Seconds $BootSeconds
$p.Refresh(); [W4]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 300
Tap "Return"; Start-Sleep -Seconds $MenuSeconds
$r = New-Object W4+RECT; [W4]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
$results = @()
for ($t = 0; $t -lt $Trials; $t++) {
  $base = Grab $r
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [W4]::keybd_event($vk[$Key], 0, 0, [UIntPtr]::Zero)
  $ms = -1
  while ($sw.ElapsedMilliseconds -lt 2000) { $g = Grab $r; if ($g -ge 0 -and [math]::Abs($g - $base) -gt 15) { $ms = $sw.ElapsedMilliseconds; break } }
  [W4]::keybd_event($vk[$Key], 0, 2, [UIntPtr]::Zero)
  $results += $ms; "trial $t : $ms ms"
  Start-Sleep -Milliseconds 1500
}
Stop-Process -Id $p.Id -Force
"median: " + (($results | Sort-Object)[[int]($results.Count / 2)]) + " ms"
