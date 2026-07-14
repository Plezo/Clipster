# Build, package and publish a Clipster release from your own machine -
# same assets the release workflow produces, without waiting for CI.
#
#   .\scripts\release-local.ps1              # build + package + upload
#   .\scripts\release-local.ps1 -SkipBuild   # package what's already built
#   .\scripts\release-local.ps1 -NoUpload    # just produce the files
#
# Uploading needs the GitHub CLI once:  winget install -e --id GitHub.cli
# then `gh auth login`. The installer needs Inno Setup 6:
#   winget install -e --id JRSoftware.InnoSetup
#
# The version comes from project(VERSION ...) in CMakeLists.txt - bump it
# there first. Publishing the release creates tag v<version>; the CI
# release workflow sees the assets already exist and skips the rebuild.

param(
  [switch]$SkipBuild,
  [switch]$NoUpload
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

$ver = (Select-String -Path CMakeLists.txt -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
$tag = "v$ver"
Write-Host "Packaging Clipster $tag" -ForegroundColor Cyan

if (-not $SkipBuild) {
  cmake --build --preset windows-release --parallel
  if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
  ctest --preset windows
  if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}

$src = 'build\windows\apps\gui\RelWithDebInfo'
if (-not (Test-Path "$src\Clipster.exe")) {
  throw "No build at $src - run without -SkipBuild first"
}

$out = 'build\release-stage'
$stage = "$out\Clipster"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$src\*" $stage -Recurse
Get-ChildItem $stage -Recurse -Filter *.pdb | Remove-Item
Copy-Item README.md, LICENSE $stage

$zip = "$out\Clipster-$tag-windows-x64.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
Write-Host "Created $zip"

$assets = @()
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
  $cmd = Get-Command iscc.exe -ErrorAction SilentlyContinue
  if ($cmd) { $iscc = $cmd.Source }
}
if (Test-Path $iscc) {
  & $iscc "/DAppVersion=$ver" "/DStageDir=$root\$stage" "/O$root\$out" `
    "/FClipster-Setup-$tag" installer\clipster.iss
  if ($LASTEXITCODE -ne 0) { throw 'Installer build failed' }
  $assets += "$out\Clipster-Setup-$tag.exe"
  Write-Host "Created $out\Clipster-Setup-$tag.exe"
} else {
  Write-Warning 'Inno Setup not found - skipping installer. winget install -e --id JRSoftware.InnoSetup'
}
$assets += $zip

if ($NoUpload) {
  Write-Host "Done (no upload). Assets in $out" -ForegroundColor Green
  exit 0
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
  Write-Warning ('GitHub CLI not found. Either install it (winget install -e --id GitHub.cli, then gh auth login) ' +
    "and re-run, or upload manually: GitHub -> Releases -> Draft a new release -> tag $tag -> attach:")
  $assets | ForEach-Object { Write-Host "  $_" }
  exit 1
}

# Update the release if the tag already exists, otherwise create it
# (which also creates the tag on the current commit).
gh release view $tag *> $null
if ($LASTEXITCODE -eq 0) {
  gh release upload $tag @assets --clobber
} else {
  gh release create $tag @assets --title $tag --generate-notes
}
if ($LASTEXITCODE -ne 0) { throw 'Upload failed' }
Write-Host "Published $tag" -ForegroundColor Green
