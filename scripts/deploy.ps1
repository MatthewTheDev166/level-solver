# PowerShell script to deploy matthew.level-solver.geode to Geode mods directories
$targetDirs = @(
    "C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods",
    "$env:LOCALAPPDATA\GeometryDash\geode\mods"
)

# Find newest matthew.level-solver.geode across build-artifact, dist, and build
$foundFiles = Get-ChildItem -Path . -Filter "matthew.level-solver.geode" -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch "node_modules|\.git" } |
    Sort-Object LastWriteTime -Descending

$sourceFile = $null
if ($foundFiles.Count -gt 0) {
    $sourceFile = $foundFiles[0].FullName
}

if (-not $sourceFile) {
    Write-Error "Could not find matthew.level-solver.geode in workspace. Build or download the mod first."
    exit 1
}

foreach ($dir in $targetDirs) {
    if (!(Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    Copy-Item -Path $sourceFile -Destination "$dir\matthew.level-solver.geode" -Force
    Write-Host "Successfully deployed $sourceFile to $dir\matthew.level-solver.geode"
}
