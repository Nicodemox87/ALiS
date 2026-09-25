# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Plugin,
    [string]$CloudCompareDirectory = 'C:\Program Files\CloudCompare'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pluginPath = [System.IO.Path]::GetFullPath($Plugin)
$hostDirectory = [System.IO.Path]::GetFullPath($CloudCompareDirectory)
foreach ($required in @($pluginPath, (Join-Path $hostDirectory 'CloudCompare.exe'), (Join-Path $hostDirectory 'Qt5Core.dll'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Compatibility check input missing: $required" }
}
if (-not [Environment]::Is64BitProcess) { throw 'Run this check in 64-bit PowerShell.' }
$header = [System.IO.File]::ReadAllBytes($pluginPath)
$peOffset = [BitConverter]::ToInt32($header, 0x3c)
if ($peOffset -lt 0 -or $peOffset + 6 -gt $header.Length -or [BitConverter]::ToUInt16($header, $peOffset + 4) -ne 0x8664) {
    throw 'ALiS must be a Windows x64 plugin.'
}
$binaryText = [System.Text.Encoding]::ASCII.GetString($header)
if ($binaryText -match '(?i)Qt[56](?:Core|Gui|Widgets)_conda\.dll|Qt6(?:Core|Gui|Widgets)\.dll') {
    throw 'Wrong Qt ABI: use the official Qt 5.15.2 build, not an Orange/Conda or Qt 6 build. Nothing was installed.'
}
foreach ($module in @('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'CCPluginAPI.dll')) {
    if ($binaryText.IndexOf($module, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "Expected native dependency $module is absent. Nothing was installed."
    }
}
$changelogPath = Join-Path $hostDirectory 'CHANGELOG.md'
$changelog = if (Test-Path -LiteralPath $changelogPath) { Get-Content -LiteralPath $changelogPath -TotalCount 10 } else { @() }
if (-not ($changelog -match '^v2\.13\.2\b')) { throw "The target must be CloudCompare 2.13.2 stable: $hostDirectory" }

# Resolve every imported symbol against the actual host DLLs in an isolated
# PowerShell process, without replacing or launching the host executable.
if (-not ('ALiS.NativeLoader' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace ALiS {
    public static class NativeLoader {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        public static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        public static extern bool SetDllDirectory(string directory);
        [DllImport("kernel32.dll", SetLastError=true)]
        public static extern bool FreeLibrary(IntPtr module);
    }
}
'@
}
$loaded = [System.Collections.Generic.List[IntPtr]]::new()
try {
    if (-not [ALiS.NativeLoader]::SetDllDirectory($hostDirectory)) { throw 'Could not configure the host DLL search directory.' }
    foreach ($name in @('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'CCCoreLib.dll', 'QCC_DB_LIB.dll', 'QCC_GL_LIB.dll', 'CCPluginAPI.dll')) {
        $handle = [ALiS.NativeLoader]::LoadLibraryEx((Join-Path $hostDirectory $name), [IntPtr]::Zero, 8)
        if ($handle -eq [IntPtr]::Zero) { throw "Host dependency $name could not load (Windows error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))." }
        $loaded.Add($handle)
    }
    $handle = [ALiS.NativeLoader]::LoadLibraryEx($pluginPath, [IntPtr]::Zero, 8)
    if ($handle -eq [IntPtr]::Zero) {
        $nativeError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "ALiS cannot load against this CloudCompare installation (Windows error $nativeError; 126 = missing DLL, 127 = incompatible API symbol, 193 = wrong architecture). Nothing was installed."
    }
    $loaded.Add($handle)
    Write-Host '[ALiS] Native x64 / Qt 5 plugin loads against the selected CloudCompare 2.13.2 runtime.' -ForegroundColor Green
} finally {
    for ($index = $loaded.Count - 1; $index -ge 0; $index--) { [void][ALiS.NativeLoader]::FreeLibrary($loaded[$index]) }
    [void][ALiS.NativeLoader]::SetDllDirectory($null)
}
