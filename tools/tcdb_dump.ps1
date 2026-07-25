# Dumps a tilecopy .tcdb (file-record flavor, version 1/2) and optionally
# re-hashes a source file in chunk_size blocks to compare against the stored
# chunk hashes. Diagnostic only; never writes anything.
#
#   powershell -File tools\tcdb_dump.ps1 -DbPath I:\temp\temp.odv.tcdb
#   powershell -File tools\tcdb_dump.ps1 -DbPath I:\temp\temp.odv.tcdb -Verify D:\temp.odv

param(
    [Parameter(Mandatory = $true)][string]$DbPath,
    [string]$Verify,
    [string]$Key = ''   # record key; '' is the single-file key
)

$ErrorActionPreference = 'Stop'
$fs = [IO.File]::OpenRead($DbPath)
$br = New-Object IO.BinaryReader($fs)

$magic = $br.ReadUInt32()
$version = $br.ReadUInt32()
$chunkSize = $br.ReadUInt64()
"db          : $DbPath"
"magic       : 0x{0:X8}" -f $magic
"version     : $version   (1 = plain, 2 = +usn, 3 = image)"
"chunk_size  : $chunkSize"
if ($magic -ne 0x42444354) { throw "not a tcdb" }
if ($version -eq 3) { throw "image database; this dumper handles file records only" }

if ($version -eq 2) {
    $dbOnly = $br.ReadByte(); $vol = $br.ReadUInt32(); $jid = $br.ReadUInt64()
    $next = $br.ReadInt64(); $exh = $br.ReadUInt64()
    "usn         : db_only=$dbOnly volume=0x{0:X8} journal=$jid next_usn=$next excludes=0x{1:X16}" -f $vol, $exh
}

$fileCount = $br.ReadUInt64()
"file_count  : $fileCount"
""

$target = $null
for ($i = 0; $i -lt [int]$fileCount; $i++) {
    $pathBytes = $br.ReadUInt32()
    $path = if ($pathBytes -gt 0) { [Text.Encoding]::UTF8.GetString($br.ReadBytes($pathBytes)) } else { '' }
    $size = $br.ReadUInt64()
    $wt = $br.ReadInt64()
    $chunkCount = $br.ReadUInt64()
    $hashes = $br.ReadBytes([int]$chunkCount * 32)
    $when = if ($wt -gt 0) { [DateTime]::FromFileTimeUtc($wt).ToString('o') } else { 'n/a' }
    "record[{0}] key='{1}'" -f $i, $path
    "  file_size         : $size"
    "  source_write_time : $wt  ($when UTC)"
    "  chunks            : $chunkCount"
    if ($path -eq $Key) { $target = [pscustomobject]@{ Size = $size; Wt = $wt; Count = $chunkCount; Hashes = $hashes } }
}
$br.Close(); $fs.Close()

if (-not $Verify) { return }
""
$src = Get-Item -LiteralPath $Verify -Force
"source      : $($src.FullName)"
"  length            : $($src.Length)"
"  last write        : $($src.LastWriteTimeUtc.ToFileTimeUtc())  ($($src.LastWriteTimeUtc.ToString('o')) UTC)"
if (-not $target) { throw "no record with key '$Key'" }
"  size match        : $($src.Length -eq [int64]$target.Size)"
"  write time match  : $($src.LastWriteTimeUtc.ToFileTimeUtc() -eq $target.Wt)   <- tilecopy skips the file when size and write time both match"
""

$sha = [Security.Cryptography.SHA256]::Create()
$buf = New-Object byte[] ([int]$chunkSize)
$in = [IO.File]::OpenRead($Verify)
$idx = 0; $diff = 0; $first = @()
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($true) {
    $n = 0
    while ($n -lt $buf.Length) {
        $r = $in.Read($buf, $n, $buf.Length - $n)
        if ($r -eq 0) { break }
        $n += $r
    }
    if ($n -eq 0) { break }
    $h = $sha.ComputeHash($buf, 0, $n)
    if ($idx -lt [int]$target.Count) {
        $same = $true
        for ($k = 0; $k -lt 32; $k++) { if ($h[$k] -ne $target.Hashes[$idx * 32 + $k]) { $same = $false; break } }
        if (-not $same) { $diff++; if ($first.Count -lt 10) { $first += $idx } }
    } else { $diff++ }
    $idx++
    if ($n -lt $buf.Length) { break }
}
$in.Close()
$sw.Stop()
"chunks read : $idx  (in $([int]$sw.Elapsed.TotalSeconds)s)"
"chunks differing from the database: $diff"
if ($diff -gt 0) { "first differing chunk indexes: $($first -join ', ')  (offset $([int64]$first[0] * [int64]$chunkSize))" }
