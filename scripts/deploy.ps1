# PowerShell script to deploy matthew.level-solver.geode to Geode mods directories
$targetDirs = @(
    "C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods",
    "$env:LOCALAPPDATA\GeometryDash\geode\mods"
)

$candidates = @(
    "dist\matthew.level-solver.geode",
    "build\matthew.level-solver.geode",
    "build\level-solver.geode",
    "matthew.level-solver.geode"
)

$sourceFile = $null
foreach ($c in $candidates) {
    if (Test-Path $c) {
        $sourceFile = (Get-Item $c).FullName
        break
    }
}

if (-not $sourceFile) {
    $found = Get-ChildItem -Path . -Filter "matthew.level-solver.geode" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) {
        $sourceFile = $found.FullName
    }
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
