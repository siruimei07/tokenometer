param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools were not found.'
}

$installation = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    throw 'MSBuild was not found in the Visual Studio installation.'
}

# Some hosts expose both Path and PATH. MSBuild's native tool tasks reject that
# duplicate, so collapse them before launching the compiler.
$cleanPath = (cmd.exe /d /c 'echo %PATH%').Trim()
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $cleanPath, 'Process')

& $msbuild (Join-Path $PSScriptRoot 'Tokenometer.sln') `
    /restore /t:Build `
    /p:Configuration=$Configuration /p:Platform=x64 `
    /m:1 /nr:false /verbosity:minimal
exit $LASTEXITCODE
