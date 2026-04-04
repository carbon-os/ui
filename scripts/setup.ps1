param(
    [string] $Version     = "146.0.3856.97",
    [string] $Destination = (Join-Path $PSScriptRoot "..\webview2_runtime\$Version")
)

$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"

$PackageId = "WebView2.Runtime.X64"
$NupkgUrl  = "https://www.nuget.org/api/v2/package/$PackageId/$Version"
$NupkgPath = Join-Path $env:TEMP "webview2_runtime_$Version.nupkg"
$ZipPath   = Join-Path $env:TEMP "webview2_runtime_$Version.zip"
$ExpandDir = Join-Path $env:TEMP "webview2_runtime_$Version"

try {
    Write-Host "  Downloading $PackageId $Version ..."
    Invoke-WebRequest -Uri $NupkgUrl -OutFile $NupkgPath -UseBasicParsing

    Write-Host "  Extracting package ..."
    if (Test-Path $ZipPath)   { Remove-Item $ZipPath -Force }
    if (Test-Path $ExpandDir) { Remove-Item $ExpandDir -Recurse -Force }

    Rename-Item -Path $NupkgPath -NewName $ZipPath
    Expand-Archive -Path $ZipPath -DestinationPath $ExpandDir -Force

    $Candidates = @(
        "WebView2",
        "content\WebView2",
        "contentFiles\any\any\WebView2"
    )

    $RuntimeSource = $null
    foreach ($c in $Candidates) {
        $p = Join-Path $ExpandDir $c
        if (Test-Path (Join-Path $p "msedgewebview2.exe")) {
            $RuntimeSource = $p
            break
        }
    }

    if (-not $RuntimeSource) {
        Write-Host ""
        Write-Host "  ERROR: could not find runtime binaries. Package contents (depth 3):"
        Get-ChildItem $ExpandDir -Recurse -Depth 3 |
            Select-Object -ExpandProperty FullName |
            ForEach-Object { Write-Host "    $_" }
        throw "Runtime folder not found - see listing above."
    }

    Write-Host "  Runtime found at: $RuntimeSource"
    Write-Host "  Staging to: $Destination"

    if (-not (Test-Path $Destination)) {
        New-Item -ItemType Directory -Path $Destination | Out-Null
    }

    Copy-Item -Path (Join-Path $RuntimeSource "*") -Destination $Destination -Recurse -Force

    Write-Host "  Done."
}
finally {
    if (Test-Path $NupkgPath) { Remove-Item $NupkgPath -Force }
    if (Test-Path $ZipPath)   { Remove-Item $ZipPath   -Force }
    if (Test-Path $ExpandDir)  { Remove-Item $ExpandDir -Recurse -Force }
}