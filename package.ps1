param(
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$releaseName = 'Tokenometer-0.1.0-win-x64'
$repositoryRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$releaseDirectory = Join-Path $repositoryRoot 'src\Tokenometer\bin\x64\Release'
$assetsPath = Join-Path $repositoryRoot 'src\Tokenometer\obj\project.assets.json'
$distDirectory = Join-Path $repositoryRoot 'dist'
$zipPath = Join-Path $distDirectory ($releaseName + '.zip')
$checksumPath = Join-Path $distDirectory 'SHA256SUMS.txt'
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$stage = Join-Path $temporaryRoot ($releaseName + '-' + [guid]::NewGuid().ToString('N'))

function Assert-OrdinaryTree([string]$Path) {
    $rootItem = Get-Item -LiteralPath $Path -Force
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release root is a reparse point: $Path"
    }
    Get-ChildItem -LiteralPath $Path -Recurse -Force | ForEach-Object {
        if (($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Release input contains a reparse point: $($_.FullName)"
        }
    }
}

function Copy-PackageNotices([string]$Destination) {
    if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
        throw 'NuGet assets are missing. Run a restore before packaging.'
    }

    $assets = Get-Content -LiteralPath $assetsPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $targetGraphs = @($assets.targets.PSObject.Properties | ForEach-Object { $_.Value })
    $packages = @($assets.libraries.PSObject.Properties |
        Where-Object { $_.Value.type -eq 'package' } |
        Sort-Object Name)
    if ($packages.Count -eq 0) {
        throw 'The resolved NuGet graph is empty.'
    }

    $inventory = New-Object System.Collections.Generic.List[string]
    foreach ($package in $packages) {
        $separator = $package.Name.LastIndexOf('/')
        if ($separator -le 0) {
            throw "Unexpected NuGet package identity: $($package.Name)"
        }

        $id = $package.Name.Substring(0, $separator)
        $version = $package.Name.Substring($separator + 1)
        $packagePath = Join-Path (Join-Path $repositoryRoot 'packages') $package.Value.path
        if (-not (Test-Path -LiteralPath $packagePath -PathType Container)) {
            throw "Restored package is missing: $id $version"
        }

        $notices = @(Get-ChildItem -LiteralPath $packagePath -Recurse -File -Force |
            Where-Object { $_.Name -match '(?i)(license|notice|third.?party)' })
        $inventoryNote = ''
        if ($notices.Count -eq 0) {
            $targetEntries = @($targetGraphs | ForEach-Object {
                $_.PSObject.Properties | Where-Object { $_.Name -eq $package.Name }
            })
            if ($targetEntries.Count -eq 0) {
                throw "Resolved target metadata is missing for $id $version"
            }
            $assetKinds = @($targetEntries | ForEach-Object {
                $_.Value.PSObject.Properties.Name
            } | Select-Object -Unique)
            if ($assetKinds -contains 'runtime' -or
                $assetKinds -contains 'runtimeTargets' -or
                $assetKinds -contains 'native') {
                throw "A redistributed package has no local LICENSE or NOTICE file: $id $version"
            }

            $nuspec = @(Get-ChildItem -LiteralPath $packagePath -File -Filter '*.nuspec')
            if ($nuspec.Count -ne 1) {
                throw "A build-only package has no unambiguous license metadata: $id $version"
            }
            $packageDestination = Join-Path (Join-Path $Destination $id) $version
            New-Item -ItemType Directory -Path $packageDestination -Force | Out-Null
            Copy-Item -LiteralPath $nuspec[0].FullName `
                -Destination (Join-Path $packageDestination 'PACKAGE.nuspec')
            $inventoryNote = ' (build-only; license URL is preserved in PACKAGE.nuspec)'
        }

        $packageDestination = Join-Path (Join-Path $Destination $id) $version
        foreach ($notice in $notices) {
            if (($notice.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Package notice is a reparse point: $($notice.FullName)"
            }
            $relative = $notice.FullName.Substring($packagePath.Length).TrimStart('\')
            $noticeTarget = Join-Path $packageDestination $relative
            New-Item -ItemType Directory -Path (Split-Path $noticeTarget -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $notice.FullName -Destination $noticeTarget
        }
        $inventory.Add("$id $version$inventoryNote")
    }

    [IO.File]::WriteAllLines(
        (Join-Path (Split-Path $Destination -Parent) 'DEPENDENCIES.txt'),
        $inventory,
        (New-Object Text.UTF8Encoding($false)))
}

if (-not $SkipBuild) {
    & (Join-Path $repositoryRoot 'build.ps1') -Configuration Release
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed with exit code $LASTEXITCODE"
    }
}

$executable = Join-Path $releaseDirectory 'Tokenometer.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'Release Tokenometer.exe was not found.'
}
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
    throw 'NuGet restore output was not found.'
}

$selfTest = Start-Process -FilePath $executable -ArgumentList '--self-test-storage' `
    -WindowStyle Hidden -Wait -PassThru
if ($selfTest.ExitCode -ne 0) {
    throw "Release self-test failed with exit code $($selfTest.ExitCode)"
}

$stagePath = [IO.Path]::GetFullPath($stage)
if (-not $stagePath.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not (Split-Path $stagePath -Leaf).StartsWith($releaseName + '-', [StringComparison]::Ordinal)) {
    throw 'Unexpected staging path.'
}

try {
    Assert-OrdinaryTree $releaseDirectory
    New-Item -ItemType Directory -Path $stagePath -Force | Out-Null
    Assert-OrdinaryTree $stagePath
    & robocopy.exe $releaseDirectory $stagePath /E /XF '*.pdb' `
        /NFL /NDL /NJH /NJS /NP | Out-Null
    $copyExitCode = $LASTEXITCODE
    if ($copyExitCode -ge 8) {
        throw "robocopy failed with exit code $copyExitCode"
    }

    foreach ($document in @('LICENSE', 'README.md', 'SECURITY.md', 'THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $repositoryRoot $document) -Destination $stagePath
    }
    $licensesDirectory = Join-Path $stagePath 'LICENSES'
    New-Item -ItemType Directory -Path $licensesDirectory -Force | Out-Null
    Copy-PackageNotices $licensesDirectory

    $forbidden = @(
        Get-ChildItem -LiteralPath $stagePath -Recurse -File -Force |
        Where-Object {
            $_.Name -match '(?i)\.(pdb|dmp|db|db-wal|db-shm|db-journal|sqlite|sqlite3|jsonl|log|pem|pfx|key)$' -or
            $_.Name -match '(?i)^\.env(?:\.|$)' -or
            $_.Name -match '(?i)^(auth|cookie)(?:[._-]|$)'
        })
    if ($forbidden.Count -ne 0) {
        throw "Forbidden release file: $($forbidden[0].FullName)"
    }

    $binary = [IO.File]::ReadAllBytes((Join-Path $stagePath 'Tokenometer.exe'))
    $ascii = [Text.Encoding]::ASCII.GetString($binary)
    $unicode = [Text.Encoding]::Unicode.GetString($binary)
    if ($ascii.IndexOf($repositoryRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $unicode.IndexOf($repositoryRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw 'Tokenometer.exe contains the absolute repository path.'
    }

    New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null
    Assert-OrdinaryTree $distDirectory
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    Compress-Archive -Path (Join-Path $stagePath '*') `
        -DestinationPath $zipPath -CompressionLevel Optimal

    $hash = Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
    $checksumLine = "$($hash.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($zipPath))"
    [IO.File]::WriteAllText($checksumPath, $checksumLine + [Environment]::NewLine,
        (New-Object Text.UTF8Encoding($false)))
    Write-Output $zipPath
    Write-Output $checksumLine
}
finally {
    if (Test-Path -LiteralPath $stagePath) {
        $resolvedStage = [IO.Path]::GetFullPath($stagePath)
        if ($resolvedStage.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path $resolvedStage -Leaf).StartsWith($releaseName + '-', [StringComparison]::Ordinal)) {
            $stageItem = Get-Item -LiteralPath $resolvedStage -Force
            if (($stageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Write-Warning "Refusing to recursively remove reparse-point staging path: $resolvedStage"
            }
            else {
                Remove-Item -LiteralPath $resolvedStage -Recurse -Force
            }
        }
    }
}
