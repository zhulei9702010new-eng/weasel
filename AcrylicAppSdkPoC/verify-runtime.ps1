#requires -Version 5.1
# CI-only checks. This reads PE metadata; it NEVER loads/executes an inspected DLL.
[CmdletBinding(DefaultParameterSetName = 'Prepare')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Prepare')]
    [ValidateSet('x64', 'x86')]
    [string]$Architecture,
    [Parameter(Mandatory = $true, ParameterSetName = 'Installer')]
    [switch]$VerifyInstaller
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputRoot = Join-Path $repo 'output'
$helperName = 'WeaselAcrylicAppSdk.dll'
$bootstrapName = 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
$requiredExports = @(
    'WeaselAcrylicAppSdkGetLifetimePolicyVersion',
    'WeaselAcrylicAppSdkGetLastStage',
    'WeaselAcrylicAppSdkGetLastHresult',
    'WeaselAcrylicAppSdkGetLastMessage',
    'WeaselAcrylicAppSdkAttach',
    'WeaselAcrylicAppSdkIsWindowActive',
    'WeaselAcrylicAppSdkSetWindowTheme',
    'WeaselAcrylicAppSdkDetach',
    'WeaselAcrylicAppSdkRequestThreadShutdown',
    'WeaselAcrylicAppSdkInitialize',
    'WeaselAcrylicAppSdkIsActive',
    'WeaselAcrylicAppSdkSetDarkMode',
    'WeaselAcrylicAppSdkShutdown',
    'WeaselAcrylicAppSdkShutdownThread'
)

function Get-PeMachine([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { return 0 }
    $offset = [BitConverter]::ToInt32($bytes, 60)
    if ($offset -lt 64 -or [long]$offset + 26 -gt $bytes.Length) { return 0 }
    if ([BitConverter]::ToUInt32($bytes, $offset) -ne 17744) { return 0 }
    $machine = [BitConverter]::ToUInt16($bytes, $offset + 4)
    $flags = [BitConverter]::ToUInt16($bytes, $offset + 22)
    $magic = [BitConverter]::ToUInt16($bytes, $offset + 24)
    if (($flags -band 0x2000) -eq 0) { return 0 } # must be a DLL
    if (($machine -eq 0x014c -and $magic -ne 0x010b) -or
        ($machine -eq 0x8664 -and $magic -ne 0x020b)) { return 0 }
    return [int]$machine
}
function Expected-Machine([string]$Arch) {
    if ($Arch -eq 'x86') { return 0x014c }
    return 0x8664
}
function Runtime-Directory([string]$Arch) {
    if ($Arch -eq 'x86') { return (Join-Path $outputRoot 'acrylic\x86') }
    return $outputRoot
}
function Invoke-Dumpbin([string]$Mode, [string]$Path) {
    $lines = @(& dumpbin /nologo $Mode $Path)
    if ($LASTEXITCODE -ne 0) { throw "dumpbin $Mode failed: $Path" }
    return ($lines -join "`n")
}
function Assert-Exports([string]$Path, [string[]]$Expected) {
    $text = Invoke-Dumpbin '/exports' $Path
    # Exact export names, not substring matches: _Attach@8 alone is NOT Attach.
    $names = @([regex]::Matches($text,
        '(?m)^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([^\s=]+)') |
        ForEach-Object { $_.Groups[1].Value })
    foreach ($symbol in $Expected) {
        if ($names -cnotcontains $symbol) { throw "Missing undecorated export $symbol in $Path" }
    }
}
function Check-File([string]$Path, [int]$Machine) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing runtime DLL: $Path" }
    if ((Get-PeMachine $Path) -ne $Machine) { throw "Wrong PE machine/type: $Path" }
    return [pscustomobject]@{
        Name = [IO.Path]::GetFileName($Path)
        Bytes = (Get-Item -LiteralPath $Path).Length
        Machine = ('0x{0:X4}' -f $Machine)
        SHA256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

if (-not $VerifyInstaller) {
    # This step runs immediately after each platform build, BEFORE restoring
    # the other platform. No parallel writes to the shared native NuGet assets.
    $files = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter project.assets.json -Recurse -File)
    if ($files.Count -ne 1) { throw "Expected one current helper assets file, got $($files.Count)" }
    $assets = Get-Content -LiteralPath $files[0].FullName -Raw | ConvertFrom-Json
    foreach ($pin in @('Microsoft.WindowsAppSDK/1.8.260804001', 'Microsoft.Windows.CppWinRT/2.0.250303.1')) {
        if (@($assets.libraries.PSObject.Properties.Name) -notcontains $pin) {
            throw "Restore did not contain the pinned dependency: $pin"
        }
    }
    $machine = Expected-Machine $Architecture
    $directory = Runtime-Directory $Architecture
    $helper = Join-Path $directory $helperName
    $bootstrap = Join-Path $directory $bootstrapName
    [void](Check-File $helper $machine)
    $sources = @()
    foreach ($library in $assets.libraries.PSObject.Properties) {
        if ($library.Value.type -ne 'package') { continue }
        foreach ($file in $library.Value.files) {
            if ($file -notmatch '(^|/)Microsoft\.WindowsAppRuntime\.Bootstrap\.dll$') { continue }
            foreach ($folder in $assets.packageFolders.PSObject.Properties.Name) {
                $source = Join-Path (Join-Path $folder $library.Value.path) $file
                if ((Test-Path -LiteralPath $source -PathType Leaf) -and
                    (Get-PeMachine $source) -eq $machine) { $sources += $source }
            }
        }
    }
    $sources = @($sources | Sort-Object -Unique)
    if ($sources.Count -eq 0) { throw "$Architecture Bootstrap DLL missing from current resolved assets" }
    $hashes = @($sources | ForEach-Object {
        (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash
    } | Select-Object -Unique)
    if ($hashes.Count -ne 1) { throw "Conflicting $Architecture Bootstrap DLLs in restore" }
    Copy-Item -LiteralPath $sources[0] -Destination $bootstrap -Force
    $records = @((Check-File $helper $machine), (Check-File $bootstrap $machine))
    Assert-Exports $helper $requiredExports
    Assert-Exports $bootstrap @('MddBootstrapInitialize2')
    $imports = Invoke-Dumpbin '/imports' $helper
    if ($imports -match 'MddBootstrapInitialize') { throw 'Unexpected static bootstrap initializer import' }
    $manifest = [ordered]@{
        Architecture = $Architecture
        Commit = $env:GITHUB_SHA
        WindowsAppSDK = '1.8.260804001'
        CppWinRT = '2.0.250303.1'
        BootstrapSource = $sources[0]
        RequiredHelperExports = $requiredExports
        Files = $records
    }
    $manifestPath = Join-Path $outputRoot "acrylic-runtime-$Architecture.json"
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-Host "PASS: $Architecture runtime pair; exact exports and PE architecture verified."
    $records | Format-Table -AutoSize
    return
}

# Check what is EMBEDDED, not just which files happen to exist in output.
$installers = @(Get-ChildItem -LiteralPath (Join-Path $outputRoot 'archives') -Filter 'weasel*.exe' -File)
if ($installers.Count -ne 1) { throw "Expected one current installer, got $($installers.Count)" }
$sevenZip = Join-Path $outputRoot '7z.exe'
if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) { throw 'Bundled 7z.exe not found' }
$extractRoot = Join-Path ([IO.Path]::GetTempPath()) ('weasel-acrylic-installer-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($extractRoot)
& $sevenZip x -y ("-o" + $extractRoot) $installers[0].FullName
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract installer for PE/hash verification' }

foreach ($arch in @('x64', 'x86')) {
    $machine = Expected-Machine $arch
    $directory = Runtime-Directory $arch
    $manifestPath = Join-Path $outputRoot "acrylic-runtime-$arch.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Missing $arch verification manifest" }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.Architecture -cne $arch -or $manifest.Commit -cne $env:GITHUB_SHA) {
        throw "Stale/wrong-architecture $arch manifest"
    }
    foreach ($name in @($helperName, $bootstrapName)) {
        $matches = @(Get-ChildItem -LiteralPath $extractRoot -Filter $name -File -Recurse |
            Where-Object {
                $isX86 = $_.FullName.Replace('/', '\').EndsWith(
                    ('\acrylic\x86\' + $name), [StringComparison]::OrdinalIgnoreCase)
                if ($arch -eq 'x86') { $isX86 } else { -not $isX86 }
            })
        if ($matches.Count -ne 1) { throw "Expected one embedded $arch/$name; got $($matches.Count)" }
        $embedded = $matches[0].FullName
        $embeddedInfo = Check-File $embedded $machine
        $buildInfo = Check-File (Join-Path $directory $name) $machine
        $record = @($manifest.Files | Where-Object { $_.Name -ceq $name })
        if ($record.Count -ne 1 -or $embeddedInfo.SHA256 -cne $buildInfo.SHA256 -or
            $embeddedInfo.SHA256 -cne $record[0].SHA256) {
            throw "Installer/build/manifest hash mismatch: $arch/$name"
        }
        if ($name -ceq $helperName) { Assert-Exports $embedded $requiredExports }
        else { Assert-Exports $embedded @('MddBootstrapInitialize2') }
        Write-Host "PASS embedded $arch/$name | $($embeddedInfo.Machine) | $($embeddedInfo.SHA256)"
    }
}
Write-Host "PASS: installer carries matching x64 + x86 pairs at their separate destinations."
Write-Host "Inspection directory (runner temp): $extractRoot"
