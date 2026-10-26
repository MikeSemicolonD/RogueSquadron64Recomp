# Bumps the release version in VERSION (major.minor.patch, each part 0-99), which CMake, Gradle and the packaging scripts read.
# Usage: .\tools\release\bump-version.ps1 [-Part patch|minor|major] [-Set 1.2.3]
param(
    [ValidateSet("patch", "minor", "major")][string]$Part = "patch",
    [string]$Set = ""
)
$file = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "VERSION"
$old = (Get-Content $file -TotalCount 1).Trim()
if ($Set -ne "") {
    $new = $Set.Trim()
}
else {
    $p = $old.Split('.') | ForEach-Object { [int]$_ }
    switch ($Part) {
        "major" { $p = @(($p[0] + 1), 0, 0) }
        "minor" { $p = @($p[0], ($p[1] + 1), 0) }
        "patch" { $p = @($p[0], $p[1], ($p[2] + 1)) }
    }
    $new = $p -join '.'
}
if ($new -notmatch '^\d{1,2}\.\d{1,2}\.\d{1,2}$') { throw "VERSION must be major.minor.patch with each part 0-99, got '$new'" }
[System.IO.File]::WriteAllText($file, "$new`n")
"VERSION $old -> $new"
