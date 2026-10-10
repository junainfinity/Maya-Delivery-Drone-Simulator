param([string]$DownloadFolder = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$folder = (Resolve-Path -LiteralPath $DownloadFolder).ProviderPath
$archiveName = 'Maya-Delivery-Drone-Simulator-v0.1-Windows.zip'
$expectedBytes = [long]13862928084
$expectedSHA256 = '7d11f5cb0309a389e1349db0790c1f0fb9bd79b509ad56f6396e6f53cc53636d'
$manifestPath = Join-Path $folder 'Maya-Windows-Download.manifest.json'
$destination = Join-Path $folder $archiveName
$pending = $destination + '.assembling'
function Hash-File([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
if (Test-Path -LiteralPath $destination) {
    $existing = Get-Item -LiteralPath $destination
    if ($existing.Length -eq $expectedBytes -and (Hash-File $destination) -eq $expectedSHA256) {
        Write-Output 'The complete Windows ZIP already exists and its SHA-256 is correct. Extract it completely, then run Play-MayaRobotics.cmd.'
        exit 0
    }
    throw 'A different ZIP already exists. Move it elsewhere first; this helper never overwrites it.'
}
if (Test-Path -LiteralPath $pending) { throw 'An .assembling file already exists. Move it elsewhere before retrying; this helper never overwrites it.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.format -cne 'ordered-raw-byte-parts-v1' -or $manifest.archive.name -cne $archiveName -or [long]$manifest.archive.bytes -ne $expectedBytes -or $manifest.archive.sha256 -cne $expectedSHA256 -or @($manifest.parts).Count -ne 7) { throw 'The download manifest does not describe this v0.1 Windows release.' }
$totalBytes = [long]0
$partPaths = @()
for ($partIndex = 0; $partIndex -lt 7; $partIndex++) {
    $part = $manifest.parts[$partIndex]
    $expectedName = '{0}.{1:000}' -f $archiveName, ($partIndex + 1)
    if ($part.name -cne $expectedName -or $part.sha256 -notmatch '^[a-f0-9]{64}$' -or [long]$part.bytes -le 0 -or [long]$part.bytes -ge 2147483648) { throw "Invalid part entry: $expectedName" }
    $path = Join-Path $folder $expectedName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Download the missing file: $expectedName" }
    if ((Get-Item -LiteralPath $path).Length -ne [long]$part.bytes -or (Hash-File $path) -cne $part.sha256) { throw "Incomplete or changed download: $expectedName. Download that part again." }
    $partPaths += $path
    $totalBytes += [long]$part.bytes
    Write-Output "Verified part $($partIndex + 1)/7"
}
if ($totalBytes -ne $expectedBytes) { throw 'Part sizes do not add up to the original ZIP.' }
$outputStream = $null
$createdPending = $false
try {
    $outputStream = [IO.File]::Open($pending, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $createdPending = $true
    foreach ($path in $partPaths) {
        $inputStream = [IO.File]::OpenRead($path)
        try { $inputStream.CopyTo($outputStream, 1048576) } finally { $inputStream.Dispose() }
    }
    $outputStream.Dispose()
    $outputStream = $null
    if ((Get-Item -LiteralPath $pending).Length -ne $expectedBytes -or (Hash-File $pending) -cne $expectedSHA256) { throw 'The assembled ZIP failed its original release checksum.' }
    Move-Item -LiteralPath $pending -Destination $destination
    $createdPending = $false
    Write-Output 'Windows ZIP assembled and its original SHA-256 verified.'
    Write-Output 'Extract the complete ZIP to a writable folder. Run Play-MayaRobotics.cmd and read How-to-Play.md.'
} finally {
    if ($null -ne $outputStream) { $outputStream.Dispose() }
    if ($createdPending -and (Test-Path -LiteralPath $pending)) { Remove-Item -LiteralPath $pending -Force }
}
