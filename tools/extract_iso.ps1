# Extract an Xbox 360 game ISO (XDVDFS) into a folder. No extra software needed.
# (PowerShell variables are case-insensitive, so the sector size is $SectorSize, not $Sector.)
# Usage: powershell -ExecutionPolicy Bypass -File extract_iso.ps1 -Iso "Skate. (USA).iso" -Out game
param([Parameter(Mandatory = $true)][string]$Iso, [string]$Out = "game")

$ErrorActionPreference = 'Stop'
$SectorSize = 2048
$Offsets = @(0x0, 0xFB20, 0x20600, 0x2080000, 0xFD90000)
$fs = [System.IO.File]::OpenRead((Resolve-Path $Iso))
$br = New-Object System.IO.BinaryReader($fs)

$base = -1
foreach ($o in $Offsets) {
  $fs.Position = $o + 32 * $SectorSize
  if ([System.Text.Encoding]::ASCII.GetString($br.ReadBytes(20)) -eq 'MICROSOFT*XBOX*MEDIA') { $base = $o; break }
}
if ($base -lt 0) { throw "Not an Xbox 360 game ISO (no XDVDFS volume found)." }
$rootSector = $br.ReadUInt32(); [void]$br.ReadUInt32()

$outRoot = [System.IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force $outRoot | Out-Null
$pending = New-Object System.Collections.Stack
$pending.Push(@(([int64]$base + [int64]$rootSector * $SectorSize), 0, ''))
$files = 0; $bytes = [int64]0; $buf = New-Object byte[] (8MB)

while ($pending.Count -gt 0) {
  $dirOff, $node, $prefix = $pending.Pop()
  $fs.Position = $dirOff + $node
  $left = $br.ReadUInt16(); $right = $br.ReadUInt16()
  if ($left -eq 0xFFFF -and $right -eq 0xFFFF) { continue }
  $sector = $br.ReadUInt32(); $length = $br.ReadUInt32(); $attr = $br.ReadByte(); $nlen = $br.ReadByte()
  $name = [System.Text.Encoding]::GetEncoding(28591).GetString($br.ReadBytes($nlen))
  if ($left) { $pending.Push(@($dirOff, ($left * 4), $prefix)) }
  if ($right) { $pending.Push(@($dirOff, ($right * 4), $prefix)) }
  $path = $prefix + $name
  $dst = Join-Path $outRoot $path
  if ($attr -band 0x10) {
    New-Item -ItemType Directory -Force $dst | Out-Null
    if ($length) { $pending.Push(@(([int64]$base + [int64]$sector * $SectorSize), 0, ($path + '\'))) }
    continue
  }
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  if ((Test-Path $dst) -and (Get-Item $dst).Length -eq $length) { $files++; $bytes += $length; continue }
  $fs.Position = [int64]$base + [int64]$sector * $SectorSize
  $o = [System.IO.File]::Create($dst)
  $left = [int64]$length
  while ($left -gt 0) {
    $n = $fs.Read($buf, 0, [int][math]::Min($left, $buf.Length))
    if ($n -le 0) { throw "Unexpected end of ISO while reading $path" }
    $o.Write($buf, 0, $n); $left -= $n
  }
  $o.Close(); $files++; $bytes += $length
  Write-Progress -Activity "Extracting $Iso" -Status $path
}
$fs.Close()
"Extracted $files files ({0:N2} GiB) to $outRoot" -f ($bytes / 1GB)
