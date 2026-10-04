# PowerShell script to deploy matthew.level-solver.geode to %LOCALAPPDATA%\GeometryDash\geode\mods\
$targetDir = "$env:LOCALAPPDATA\GeometryDash\geode\mods"
if (!(Test-Path $targetDir)) {
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
}

$candidates = @(
    "build\matthew.level-solver.geode",
    "build\level-solver.geode",
    "dist\matthew.level-solver.geode",
    "matthew.level-solver.geode"
)

$found = $false
foreach ($c in $candidates) {
    if (Test-Path $c) {
        Copy-Item -Path $c -Destination "$targetDir\matthew.level-solver.geode" -Force
        Write-Host "Successfully deployed $c to $targetDir\matthew.level-solver.geode"
        $found = $true
        break
    }
}

if (-not $found) {
    $geodeFile = Get-ChildItem -Path . -Filter "matthew.level-solver.geode" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($geodeFile) {
        Copy-Item -Path $geodeFile.FullName -Destination "$targetDir\matthew.level-solver.geode" -Force
        Write-Host "Successfully deployed $($geodeFile.FullName) to $targetDir\matthew.level-solver.geode"
    } else {
        Write-Warning "Could not find matthew.level-solver.geode in workspace. Build the mod first."
    }
}
