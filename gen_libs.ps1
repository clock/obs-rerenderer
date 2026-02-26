$msvc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.42.34433\bin\Hostx64\x64'
$libs = 'C:\Users\15195\Documents\obs-rerenderer\obs_sdk_libs'
$obsBin = 'C:\Program Files\obs-studio\bin\64bit'

if (-not (Test-Path $libs)) { New-Item -ItemType Directory -Path $libs | Out-Null }

function Gen-Lib($dllPath, $dllName) {
    Write-Host "generating $dllName.lib..."

    # dump exports
    $raw = & "$msvc\dumpbin.exe" /exports $dllPath 2>$null
    $exports = $raw | Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)' } | ForEach-Object {
        # match[1] = just the export name, stopping before any ' = alias' or '(demangled)' part
        $null = $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)'
        $Matches[1]
    }
    Write-Host "  $($exports.Count) exports"

    # write .def file
    $def = @("LIBRARY $dllName", "EXPORTS") + $exports
    $def | Set-Content "$libs\$dllName.def" -Encoding ASCII

    # create import lib
    & "$msvc\lib.exe" /nologo /machine:x64 "/def:$libs\$dllName.def" "/out:$libs\$dllName.lib"
    Remove-Item "$libs\$dllName.def" -ErrorAction SilentlyContinue

    if (Test-Path "$libs\$dllName.lib") {
        $size = (Get-Item "$libs\$dllName.lib").Length
        Write-Host "  done -> $dllName.lib ($size bytes)"
    } else {
        Write-Host "  ERROR: lib not created"
    }
}

Gen-Lib "$obsBin\obs.dll" "obs"
Gen-Lib "$obsBin\obs-frontend-api.dll" "obs-frontend-api"

# write obs_sdk.props
$propsPath = 'C:\Users\15195\Documents\obs-rerenderer\obs_sdk.props'
$obsSource = 'C:\Users\15195\Documents\obs-rerenderer\example_sources\obs-studio'

# find obs-studio headers: prefer example_sources clone, fall back to noting it's needed
$includeDir = if (Test-Path "$obsSource\libobs") { "$obsSource\libobs" } else { "CLONE_OBS_STUDIO_FIRST" }

$props = @"
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Label="UserMacros">
    <OBS_INCLUDE>$includeDir</OBS_INCLUDE>
    <OBS_LIB>$libs</OBS_LIB>
    <OBS_INSTALL>C:\Program Files\obs-studio</OBS_INSTALL>
  </PropertyGroup>
</Project>
"@
$props | Set-Content $propsPath -Encoding UTF8
Write-Host "wrote obs_sdk.props (OBS_INCLUDE=$includeDir)"

# generate obsconfig.h if headers are available (cmake-generated, not in source tree)
if ($includeDir -ne "CLONE_OBS_STUDIO_FIRST") {
    $obsVer = (Get-Item "$obsBin\obs.dll").VersionInfo.FileVersion
    $obsconfig = @"
#pragma once

// generated for obs $obsVer windows stable build
// linux/wayland/pulseaudio defines intentionally omitted
// obs_data_path / plugin_path not needed on windows (resolved at runtime)

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
"@
    $obsconfig | Set-Content "$includeDir\obsconfig.h" -Encoding ASCII
    Write-Host "wrote obsconfig.h for obs $obsVer"
}

if ($includeDir -eq "CLONE_OBS_STUDIO_FIRST") {
    Write-Host ""
    Write-Host "NOTE: clone obs-studio headers next:"
    Write-Host "  git clone --depth=1 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git example_sources\obs-studio"
    Write-Host "  cd example_sources\obs-studio && git sparse-checkout set libobs"
    Write-Host "then re-run this script"
}
