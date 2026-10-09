# Launch skate1.exe, take screenshots of its window at the given times (seconds), then close it.
param([int[]]$At = @(20, 40, 60), [string]$OutDir = "$PSScriptRoot\..\work\snaps")

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Win { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; } }
"@
New-Item -ItemType Directory -Force $OutDir | Out-Null
$bin = Join-Path $PSScriptRoot '..\out\build\win-amd64-relwithdebinfo'
Set-Location $bin
$p = Start-Process .\skate1.exe -PassThru
$start = Get-Date
foreach ($t in $At) {
  while (((Get-Date) - $start).TotalSeconds -lt $t -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
  if ($p.HasExited) { "exited before ${t}s"; break }
  $p.Refresh(); $h = $p.MainWindowHandle
  [Win]::SetForegroundWindow($h) | Out-Null
  $r = New-Object Win+RECT; [Win]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $file = Join-Path $OutDir "snap_${t}s.png"; $bmp.Save($file); "saved $file"
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
