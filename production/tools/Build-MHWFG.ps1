#Requires -Version 7.6.4
<#
.SYNOPSIS
Builds the MHWFG production OptiScaler DLL from a prepared source tree.
.DESCRIPTION
Produces build artifacts only. No NVIDIA or SDK runtime file is collected, no
distribution package is assembled, and no network, Git, download or deployment
step is performed. Every output (bin, obj, log) is written below
-OutputDirectory and the source tree is never cleaned, but the build manages its
own output there: the full rebuild removes and recreates the previous
intermediate and output artifacts, and build-mhwfg.log is overwritten on each
run. Use a dedicated output directory that holds no files you want to keep.

The MSBuild command line is fixed to Release|x64 with MHWFGProduction=true and
the upstream Pre/Post build events disabled, so version metadata keeps coming
from the existing headers in the tree. A full Rebuild is used so the production
compiler macro can be verified from the real compile line in the log instead of
from the echoed arguments.
.PARAMETER SourceRoot
Directory that already contains OptiScaler.sln and the OptiScaler project with
its complete prepared dependency tree.
.PARAMETER OutputDirectory
Dedicated directory for bin, obj and the build log. Created when missing. It
must be outside SourceRoot. The rebuild cleans and recreates its own artifacts
below it and the log is overwritten on each run, so keep no user files there.
.PARAMETER MSBuildPath
Optional explicit MSBuild.exe. When omitted, vswhere from an installed Visual
Studio is used to locate MSBuild. Nothing is downloaded or installed.
.EXAMPLE
pwsh ./production/tools/Build-MHWFG.ps1 -SourceRoot ./source/OptiScaler -OutputDirectory ./out/mhwfg
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $SourceRoot,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $MSBuildPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Failure paths print one clear reason and leave a non-zero process exit code.
function Stop-Build([string] $Message) {
    Write-Host "[MHWFG] ERROR: $Message"
    exit 1
}

function Resolve-MSBuild([string] $Explicit) {
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) {
        if (-not (Test-Path -LiteralPath $Explicit -PathType Leaf)) {
            Stop-Build "MSBuildPath does not exist: $Explicit"
        }
        $item = Get-Item -LiteralPath $Explicit -Force
        if ($item.Name -ine 'MSBuild.exe') {
            Stop-Build "MSBuildPath must point to MSBuild.exe: $Explicit"
        }
        return $item.FullName
    }

    $programFilesX86 = ${env:ProgramFiles(x86)}
    if ([string]::IsNullOrWhiteSpace($programFilesX86)) {
        Stop-Build 'ProgramFiles(x86) is not defined; pass -MSBuildPath explicitly.'
    }
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        Stop-Build 'vswhere.exe was not found. Install Visual Studio 2022 (or Build Tools) with the MSBuild component, or pass -MSBuildPath. Nothing is downloaded or installed by this script.'
    }
    $candidates = @(& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' -nologo -utf8) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { $_.Trim() }
    $candidates = @($candidates)
    if ($candidates.Count -eq 0) {
        Stop-Build 'vswhere did not report an MSBuild.exe from an installed Visual Studio instance. Install the MSBuild component or pass -MSBuildPath. Nothing is downloaded or installed by this script.'
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Get-Item -LiteralPath $candidate -Force).FullName
        }
    }
    Stop-Build "vswhere reported MSBuild paths that do not exist: $($candidates -join '; ')"
}

# --- Inputs -------------------------------------------------------------
try { $sourceItem = Get-Item -LiteralPath $SourceRoot -Force -ErrorAction Stop }
catch { Stop-Build "SourceRoot does not exist: $SourceRoot" }
if ($sourceItem -isnot [System.IO.DirectoryInfo]) {
    Stop-Build "SourceRoot must be a directory: $SourceRoot"
}
$source = $sourceItem.FullName

$solution = Join-Path $source 'OptiScaler.sln'
if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) {
    Stop-Build "SourceRoot must contain OptiScaler.sln: $source"
}
$projectDir = Join-Path $source 'OptiScaler'
if (-not (Test-Path -LiteralPath $projectDir -PathType Container)) {
    Stop-Build "SourceRoot must contain the OptiScaler project directory: $source"
}
if (-not (Test-Path -LiteralPath (Join-Path $projectDir 'OptiScaler.vcxproj') -PathType Leaf)) {
    Stop-Build "SourceRoot must contain OptiScaler/OptiScaler.vcxproj: $source"
}

$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if ([string]::IsNullOrWhiteSpace($output)) {
    Stop-Build "OutputDirectory could not be resolved: $OutputDirectory"
}
$output = $output.TrimEnd([char[]]@([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar))
if (-not [System.IO.Path]::IsPathFullyQualified($output)) {
    Stop-Build "OutputDirectory must resolve to a fully qualified path: $OutputDirectory"
}

# Rebuild runs Clean against IntDir/OutDir, so build output is kept out of the source tree.
$relativeToSource = [System.IO.Path]::GetRelativePath($source, $output)
if ($relativeToSource -eq '.' -or
    (-not [System.IO.Path]::IsPathRooted($relativeToSource) -and $relativeToSource -ne '..' -and $relativeToSource -notmatch '^\.\.[\\/]')) {
    Stop-Build "OutputDirectory must be outside SourceRoot: $output"
}

$msbuild = Resolve-MSBuild $MSBuildPath

# --- Output layout ------------------------------------------------------
$binDir = Join-Path $output 'bin'
$objDir = Join-Path $output 'obj'
New-Item -ItemType Directory -Force -Path $binDir, $objDir | Out-Null
$separator = [System.IO.Path]::DirectorySeparatorChar
$intDir = $objDir + $separator
$outDir = $binDir + $separator
$logPath = Join-Path $output 'build-mhwfg.log'

# --- Build --------------------------------------------------------------
$arguments = @(
    $solution
    '/nologo'
    '/m:1'
    '/t:Rebuild'
    '/p:Configuration=Release'
    '/p:Platform=x64'
    '/p:MHWFGProduction=true'
    '/p:PreBuildEventUseInBuild=false'
    '/p:PostBuildEventUseInBuild=false'
    "/p:IntDir=$intDir"
    "/p:OutDir=$outDir"
)

$header = @(
    'MHWFG portable production build'
    "started          : $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')"
    "powershell       : $($PSVersionTable.PSVersion)"
    "source root      : $source"
    "output directory : $output"
    "msbuild          : $msbuild"
    "arguments        : $($arguments -join ' ')"
    ''
)
Set-Content -LiteralPath $logPath -Value $header -Encoding utf8

& $msbuild @arguments *>&1 | Tee-Object -FilePath $logPath -Append
$code = $LASTEXITCODE

# --- Verification -------------------------------------------------------
$dllPath = Join-Path $binDir 'OptiScaler.dll'
$logText = Get-Content -LiteralPath $logPath -Raw
$failures = [System.Collections.Generic.List[string]]::new()

if ($code -ne 0) { $failures.Add("MSBuild exited with code $code") }
if ($logText -match 'error C\d+|error LNK\d|error MSB\d') {
    $failures.Add('the build log contains compiler or linker errors')
}
if ($logText -match '(?m)^\s*PreBuildEvent\s*:') {
    $failures.Add('PreBuildEvent ran even though PreBuildEventUseInBuild=false')
}
if ($logText -match '(?m)^\s*PostBuildEvent\s*:') {
    $failures.Add('PostBuildEvent ran even though PostBuildEventUseInBuild=false')
}
$macroLines = @($logText -split '\r?\n' |
    Where-Object { $_ -match 'CL\.exe' -and $_ -match 'dllmain\.cpp' -and $_ -match 'MHWFG_PRODUCTION' })
if ($macroLines.Count -eq 0) {
    $failures.Add('no CL.exe line compiling dllmain.cpp with MHWFG_PRODUCTION was found in the build log')
}
if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
    $failures.Add("the expected DLL was not produced: $dllPath")
}
else {
    $dll = Get-Item -LiteralPath $dllPath -Force
    if ($dll.Length -le 0) { $failures.Add("the produced DLL is empty: $dllPath") }
}

$summary = [System.Collections.Generic.List[string]]::new()
$summary.Add('')
$summary.Add("finished          : $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')")
$summary.Add("msbuild exit code : $code")
$summary.Add("production macro  : $(if ($macroLines.Count -gt 0) { 'MHWFG_PRODUCTION on the dllmain.cpp CL.exe line' } else { 'not found' })")
if (Test-Path -LiteralPath $dllPath -PathType Leaf) {
    $dll = Get-Item -LiteralPath $dllPath -Force
    $summary.Add("output dll        : $($dll.FullName)")
    $summary.Add("output dll bytes  : $($dll.Length)")
    $summary.Add("output dll sha256 : $((Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash)")
}
Add-Content -LiteralPath $logPath -Value $summary

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Host "[MHWFG] ERROR: $failure" }
    Write-Host "[MHWFG] Build verification failed. Log: $logPath"
    if ($code -ne 0) { exit $code }
    exit 1
}

Write-Output "MHWFG production build verified: $dllPath"
Write-Output "Log: $logPath"
exit 0
