#Requires -Version 7.6.4
<#
.SYNOPSIS
Lists a local OptiScaler source tree for review. Does not export its contents.
.DESCRIPTION
Only root files and the OptiScaler/external subtrees are in scope. Links and
known build directories are recorded but not traversed. File contents are not
read: license, provenance and privacy review remain pending for every candidate.
The output directory must already exist, outside SourceRoot. Existing output
files are never replaced. Run against a quiescent tree, not during a build.
.EXAMPLE
./Inventory-Source.ps1 -SourceRoot ./source/OptiScaler -OutputManifest ./review/inventory.json
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $SourceRoot,
    [Parameter(Mandatory)][string] $OutputManifest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Reject linked ancestors as well as links encountered during enumeration.
function Assert-PlainDirectory([string] $Path, [string] $Label) {
    try { $directory = Get-Item -LiteralPath $Path -Force }
    catch { throw "$Label must be an existing directory." }
    if ($directory -isnot [System.IO.DirectoryInfo]) {
        throw "$Label must be an existing directory."
    }
    for ($ancestor = $directory; $null -ne $ancestor; $ancestor = $ancestor.Parent) {
        if ($ancestor.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "$Label must not use a linked directory."
        }
    }
    return $directory.FullName
}

$root = Assert-PlainDirectory $SourceRoot 'SourceRoot'
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputManifest)
$outputParent = Assert-PlainDirectory ([System.IO.Path]::GetDirectoryName($output)) 'Output directory'
$relativeOutput = [System.IO.Path]::GetRelativePath($root, $output)
if (-not [System.IO.Path]::IsPathRooted($relativeOutput) -and
    $relativeOutput -ne '..' -and $relativeOutput -notmatch '^\.\.[\\/]') {
    throw 'OutputManifest must be outside SourceRoot.'
}
if (Test-Path -LiteralPath $output) { throw 'OutputManifest already exists; it will not be overwritten.' }
if (-not (Test-Path -LiteralPath (Join-Path $root 'OptiScaler.sln') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $root 'OptiScaler') -PathType Container)) {
    throw 'SourceRoot must contain OptiScaler.sln and the OptiScaler directory.'
}

$entries = [System.Collections.Generic.List[object]]::new()
$sourceExtensions = @(
    '.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl',
    '.hlsl', '.hlsli', '.glsl', '.vert', '.frag', '.comp', '.rc', '.rc2', '.def',
    '.sln', '.vcxproj', '.filters', '.props', '.targets', '.cmake',
    '.ps1', '.bat', '.cmd', '.sh', '.py', '.md', '.txt', '.rst', '.ini', '.json',
    '.xml', '.yml', '.yaml', '.natvis'
)
$binaryExtensions = @('.lib', '.a', '.dll', '.so', '.dylib', '.exe', '.spv', '.cso', '.dxil')
$excludedExtensions = @(
    '.log', '.dmp', '.pdb', '.obj', '.pch', '.tlog', '.iobj', '.ipdb', '.exp',
    '.user', '.suo', '.tmp', '.bak', '.mp4', '.mkv', '.avi', '.mov'
)

function Get-Classification([string] $Relative, [bool] $Directory) {
    $name = [System.IO.Path]::GetFileName($Relative)
    if ($Directory) {
        # Do not exclude vendor lib/x64/Release: these may be required libraries.
        if ($name -in @('.git', '.vs', '.vscode', '__pycache__') -or
            $Relative -match '^(OptiScaler/)?(x64|x86|Debug|Release|ReleaseDebug|obj)$') {
            return @('excluded', 'build_or_private_directory')
        }
        if ($Relative -notmatch '/' -and $Relative -notin @('OptiScaler', 'external')) {
            return @('excluded', 'root_directory_out_of_scope')
        }
    }
    else {
        $extension = [System.IO.Path]::GetExtension($Relative).ToLowerInvariant()
        if ($extension -in $excludedExtensions -or
            $name -in @('.git', '.env', 'AGENTS.md', 'CLAUDE.md') -or
            $name -like '.env.*' -or $name -like '*.local.ini' -or
            $name -like '*.user.ini' -or $name -like '*.ini.local') {
            return @('excluded', 'build_or_private_file')
        }
        if ($extension -in $binaryExtensions) {
            return @('binary_dependency_pending', 'binary_kind_and_origin_unverified')
        }
    }
    if ($Relative -match '^(external|OptiScaler/include|OptiScaler/library)(/|$)') {
        return @('third_party_pending', 'dependency_material')
    }
    if ($Directory -or $extension -in $sourceExtensions -or
        $name -match '^(LICENSE|LICENCE|COPYING|NOTICE)(\..*)?$' -or
        $name -in @('.gitignore', '.gitattributes', '.gitmodules', '.clang-format', 'Makefile')) {
        return @('source_candidate_pending', 'source_or_build_material')
    }
    return @('unclassified_pending', 'unrecognized_file_type')
}

function Read-DirectoryMetadata([string] $Directory) {
    try { $items = @(Get-ChildItem -LiteralPath $Directory -Force) }
    catch { throw 'Directory enumeration failed; no complete inventory was produced.' }
    foreach ($item in $items) {
        $relative = [System.IO.Path]::GetRelativePath($root, $item.FullName).Replace('\', '/')
        $isDirectory = $item -is [System.IO.DirectoryInfo]
        $isLink = [bool]($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)
        if ($isLink) { $classification = @('excluded', 'reparse_point_not_followed') }
        else { $classification = Get-Classification $relative $isDirectory }
        $traverse = $isDirectory -and $classification[0] -ne 'excluded'
        $entries.Add([ordered]@{
            path = $relative
            kind = $(if ($isLink) { 'link' } elseif ($isDirectory) { 'directory' } else { 'file' })
            bytes = $(if (-not $isDirectory -and -not $isLink) { $item.Length } else { $null })
            classification = $classification[0]
            reason = $classification[1]
            reviewStatus = $(if ($classification[0] -eq 'excluded') { 'excluded' } else { 'pending' })
            childrenScanned = $(if ($isDirectory -or $isLink) { $traverse } else { $null })
        })
        if ($traverse) { Read-DirectoryMetadata $item.FullName }
    }
}

Read-DirectoryMetadata $root
$sorted = @($entries | Sort-Object { $_.path })
$counts = [ordered]@{}
foreach ($group in ($sorted | Group-Object { $_.classification } | Sort-Object Name)) {
    $counts[$group.Name] = $group.Count
}
$manifest = [ordered]@{
    schemaVersion = 1
    status = 'inventory_only'
    releaseApproved = $false
    scope = @('root files', 'OptiScaler/', 'external/')
    contentRead = $false
    privacyReview = 'not_performed'
    licenseReview = 'pending'
    limits = @(
        'Excluded directories and links are listed without their children.',
        'Relative filenames are included; this manifest itself requires review before sharing.',
        'Binary classifications do not distinguish static from import libraries.',
        'Inventory is not an export, a build-completeness check, or a filesystem snapshot.'
    )
    summary = [ordered]@{
        fileCount = @($sorted | Where-Object { $_.kind -eq 'file' }).Count
        directoryCount = @($sorted | Where-Object { $_.kind -eq 'directory' }).Count
        linkCount = @($sorted | Where-Object { $_.kind -eq 'link' }).Count
        entryCountsByClassification = $counts
    }
    entries = $sorted
}
$json = $manifest | ConvertTo-Json -Depth 8
# CreateNew enforces non-overwrite even if a file appears after the early check.
$stream = $null
$writer = $null
try {
    $stream = [System.IO.File]::Open($output, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $writer = [System.IO.StreamWriter]::new($stream, [System.Text.UTF8Encoding]::new($false))
    $writer.WriteLine($json)
}
catch { throw 'Could not create or finish OutputManifest; no existing file was replaced. Check for a partial new file.' }
finally {
    if ($null -ne $writer) { $writer.Dispose() }
    elseif ($null -ne $stream) { $stream.Dispose() }
}
Write-Output "Inventory written: $($manifest.summary.fileCount) files; releaseApproved=false."
