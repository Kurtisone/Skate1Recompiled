# Screenshot the already-running skate1 window (optionally tapping/holding keys first).
# Usage: snap_running.ps1 -Script "0.5:snap,Space,0.3:snap" ; same step syntax as play_keys (wait:action).
param([string]$Script = "0:snap", [string]$OutDir = "$PSScriptRoot\..\work\snaps\live")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W6 { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public struct RECT { public int Left, Top, Right, Bottom; } }
"@
$vk = @{ Return = 0x0D; Space = 0x20; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; W = 0x57; A = 0x41; S = 0x53; D = 0x44; Shift = 0x10; Q = 0x51; R = 0x52; Escape = 0x1B }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$p = Get-Process skate1 -ErrorAction Stop | Select-Object -First 1
$h = $p.MainWindowHandle
if ([W6]::GetForegroundWindow() -ne $h) { [W6]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [W6]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero) }
[W6]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
$n = (Get-ChildItem $OutDir -Filter *.png -ErrorAction SilentlyContinue).Count
foreach ($step in $Script.Split(',')) {
  $wait, $action = if ($step.Contains(':')) { $step.Split(':') } else { 0, $step }
  Start-Sleep -Milliseconds ([int]([double]$wait * 1000))
  if ($action -eq 'snap') {
    $r = New-Object W6+RECT; [W6]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $f = Join-Path $OutDir ("live_{0:D3}.png" -f $n++); $bmp.Save($f); "saved $f"
  } else {
    $hold = 0.15; if ($action.Contains('*')) { $action, $hs = $action.Split('*'); $hold = [double]$hs }
    [W6]::keybd_event([byte]$vk[$action], 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds ([int]($hold * 1000))
    [W6]::keybd_event([byte]$vk[$action], 0, 2, [UIntPtr]::Zero)
  }
}
