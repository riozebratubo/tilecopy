# Dumps a tilecopy .tcdb of the *image* flavor (version 3, written by
# --drive/--partition runs to a .vhdx) and reports whether the NEXT run will
# take the incremental path or rewrite the whole image. Diagnostic only;
# never writes anything.
#
#   powershell -File tools\tcdb_image_dump.ps1 -DbPath E:\backup\disk2.vhdx.tcdb
#   powershell -File tools\tcdb_image_dump.ps1 -DbPath E:\backup\disk2.vhdx.tcdb -NoChunkScan
#
# -Vhdx defaults to the db path with ".tcdb" stripped, which is what tilecopy
# uses when --db is not given.

param(
    [Parameter(Mandatory = $true)][string]$DbPath,
    [string]$Vhdx,
    [switch]$NoChunkScan   # skip the per-chunk pass (128 MiB of hashes on a 4 TB source)
)

$ErrorActionPreference = 'Stop'

if (-not $Vhdx) {
    $Vhdx = if ($DbPath.ToLower().EndsWith('.tcdb')) { $DbPath.Substring(0, $DbPath.Length - 5) } else { '' }
}

$fs = [IO.File]::OpenRead($DbPath)
$br = New-Object IO.BinaryReader($fs)

$magic     = $br.ReadUInt32()
$version   = $br.ReadUInt32()
$chunkSize = $br.ReadUInt64()
if ($magic -ne 0x42444354) { $br.Close(); $fs.Close(); throw "not a tcdb (magic 0x{0:X8})" -f $magic }
if ($version -ne 3) { $br.Close(); $fs.Close(); throw "version $version is a file-record database; use tcdb_dump.ps1" }

# version 3 layout, packed: kind u8, db_only u8, source_id[16], source_size u64,
# dest_size u64, dest_write_time i64, chunk_count u64, then chunk_count * 32 bytes.
$kind       = $br.ReadByte()
$dbOnly     = $br.ReadByte()
$sourceId   = $br.ReadBytes(16)
$sourceSize = $br.ReadUInt64()
$destSize   = $br.ReadUInt64()
$destWt     = $br.ReadInt64()
$chunkCount = $br.ReadUInt64()

function Human([UInt64]$b) {
    $u = @('B','KiB','MiB','GiB','TiB'); $v = [double]$b; $i = 0
    while ($v -ge 1024 -and $i -lt 4) { $v /= 1024; $i++ }
    if ($i -eq 0) { "$b B" } else { '{0:N1} {1}' -f $v, $u[$i] }
}
function WhenOf([Int64]$ft) { if ($ft -gt 0) { [DateTime]::FromFileTimeUtc($ft).ToString('o') } else { 'n/a' } }

$kindText = switch ($kind) { 1 { 'whole disk (--drive)' } 2 { 'single partition (--partition)' } default { "unknown ($kind)" } }
$guid = New-Object Guid (, $sourceId)

"db              : $DbPath"
"version         : 3 (image record)"
"chunk_size      : $chunkSize  ($(Human ([UInt64]$chunkSize)))"
"kind            : $kindText"
"db_only         : $dbOnly   (1 = hashes came from --make-db; forces a full copy next run)"
"source_id       : $guid"
"source_size     : $sourceSize  ($(Human $sourceSize))"
"chunk_count     : $chunkCount"
"dest_size       : $destSize  ($(Human $destSize))"
"dest_write_time : $destWt  ($(WhenOf $destWt) UTC)"
""

if ($chunkCount -ne [Math]::Floor(($sourceSize + $chunkSize - 1) / $chunkSize)) {
    "WARNING: chunk_count does not match source_size/chunk_size; tilecopy will reject this database."
}

# ---- chunk census ----------------------------------------------------------
# A chunk of 32 zero bytes is kUnwrittenChunk (a hole: source was all zeros or
# unallocated and nothing was written to the VHDX). Everything else is a real
# SHA-256, i.e. a chunk that holds data in the image.
if (-not $NoChunkScan -and $chunkCount -gt 0) {
    $holes = [UInt64]0
    $data  = [UInt64]0
    $blockChunks = 262144                       # 8 MiB of hashes per read
    $left = [UInt64]$chunkCount
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($left -gt 0) {
        $n = [int][Math]::Min([UInt64]$blockChunks, $left)
        $buf = $br.ReadBytes($n * 32)
        if ($buf.Length -ne $n * 32) { "WARNING: database is truncated; scanned what was there."; $left = 0; break }
        for ($i = 0; $i -lt $buf.Length; $i += 32) {
            # first 8 bytes non-zero => certainly a real hash; the full check
            # only runs on the ~1-in-2^64 case and on true holes.
            if ([BitConverter]::ToUInt64($buf, $i) -ne 0) { $data++; continue }
            $zero = $true
            for ($k = $i + 8; $k -lt $i + 32; $k += 8) {
                if ([BitConverter]::ToUInt64($buf, $k) -ne 0) { $zero = $false; break }
            }
            if ($zero) { $holes++ } else { $data++ }
        }
        $left -= [UInt64]$n
    }
    $sw.Stop()
    "chunks with data: $data  ($(Human ([UInt64]($data * $chunkSize))))"
    "chunks as holes : $holes  ($(Human ([UInt64]($holes * $chunkSize))))"
    "                  (scanned in $([int]$sw.Elapsed.TotalSeconds)s)"
    "                  a full (non-incremental) run rewrites the 'with data' figure;"
    "                  compare it against what the run reported as written."
    ""
}
$br.Close(); $fs.Close()

# ---- will the next run be incremental? -------------------------------------
if (-not $Vhdx) { return }
"destination     : $Vhdx"
if (-not (Test-Path -LiteralPath $Vhdx)) {
    "  the destination does not exist -> next run: full copy into a new image."
    return
}
$f = Get-Item -LiteralPath $Vhdx -Force
$actualSize = [UInt64]$f.Length
$actualWt   = $f.LastWriteTimeUtc.ToFileTimeUtc()
"  actual size       : $actualSize  ($(Human $actualSize))"
"  actual write time : $actualWt  ($($f.LastWriteTimeUtc.ToString('o')) UTC)"
""

$reasons = @()
if ($dbOnly -ne 0)          { $reasons += "the database was written by --make-db (db_only=1)" }
if ($destSize -eq 0)        { $reasons += "dest_size is 0: the recorded run had failed chunk(s) or a failed flush, so tilecopy discarded the destination identity" }
elseif ($destSize -ne $actualSize) { $reasons += "size differs: recorded $destSize, actual $actualSize" }
if ($destWt -ne $actualWt -and $destSize -ne 0) {
    $delta = $actualWt - $destWt
    $reasons += ("write time differs: recorded $destWt, actual $actualWt (actual is {0:N3}s {1})" -f ([Math]::Abs($delta) / 1e7), $(if ($delta -gt 0) { 'newer' } else { 'older' }))
}

if ($reasons.Count -eq 0) {
    "VERDICT: the next run takes the incremental path (only differing chunks are written)."
} else {
    "VERDICT: the next run DELETES the .vhdx and rewrites every chunk that holds data."
    foreach ($r in $reasons) { "  - $r" }
    ""
    "A write time that is only a fraction of a second newer than the recorded one means"
    "NTFS stamped the VHDX after tilecopy read the timestamp back (post-detach cleanup),"
    "which makes every run a full copy no matter how little the source changed."
}
