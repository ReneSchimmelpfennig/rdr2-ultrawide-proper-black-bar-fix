# Build the .asi with cl.exe alone, no CMake.
#
# The Visual Studio installation lost its CMake component and is no longer
# registered with the Visual Studio Installer, so `cmake --build` fails and the
# generated MSBuild projects fail too -- they re-run CMake through ZERO_CHECK on
# every build. Repairing Visual Studio is the real fix; this is the bypass.
#
# Nothing about the plugin needs CMake: a handful of translation units plus
# MinHook. The flags mirror the CMake build exactly -- /MT so the .asi needs no
# VC redistributable, C++20 for std::format in the logger, and NOMINMAX because
# without it windows.h turns min and max into macros and framing.h stops
# compiling.
#
# Everything goes through response files. The version define contains quotation
# marks that no amount of PowerShell escaping delivers intact, and a response
# file is read verbatim. Paths inside are quoted and use forward slashes: half of
# them contain spaces, and a trailing backslash before the closing quote would be
# read as an escape.

param([switch]$Tests)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$msvc = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC" -Directory |
        Sort-Object Name -Descending | Select-Object -First 1
$cl = Join-Path $msvc.FullName "bin\Hostx64\x64\cl.exe"
$link = Join-Path $msvc.FullName "bin\Hostx64\x64\link.exe"

$sdkRoot = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer = (Get-ChildItem "$sdkRoot\Include" -Directory | Sort-Object Name -Descending |
           Select-Object -First 1).Name

# The quotes must enclose the whole argument, switch included. Writing
# /I"C:/path" splits the token at the quote and cl.exe reads the path as a source
# file; "/IC:/path" is what it expects.
function Q([string]$path) { return '"' + ($path -replace '\\', '/') + '"' }
function QArg([string]$prefix, [string]$path) {
    return '"' + $prefix + ($path -replace '\\', '/') + '"'
}

$includes = @(
    (QArg "/I" "$root\src"),
    (QArg "/I" "$root\build\_deps\minhook-src\include"),
    (QArg "/I" "$($msvc.FullName)\include"),
    (QArg "/I" "$sdkRoot\Include\$sdkVer\ucrt"),
    (QArg "/I" "$sdkRoot\Include\$sdkVer\um"),
    (QArg "/I" "$sdkRoot\Include\$sdkVer\shared")
)
$libs = @(
    (QArg "/LIBPATH:" "$($msvc.FullName)\lib\x64"),
    (QArg "/LIBPATH:" "$sdkRoot\Lib\$sdkVer\ucrt\x64"),
    (QArg "/LIBPATH:" "$sdkRoot\Lib\$sdkVer\um\x64")
)

$out = "$root\build\direct"
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\*.obj" -ErrorAction SilentlyContinue

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern "VERSION\s+(\d+\.\d+\.\d+)" |
            Select-Object -First 1).Matches[0].Groups[1].Value

$cppSources = Get-ChildItem "$root\src" -Filter "*.cpp" | ForEach-Object { Q $_.FullName }
$cSources = @(
    (Q "$root\build\_deps\minhook-src\src\buffer.c"),
    (Q "$root\build\_deps\minhook-src\src\hook.c"),
    (Q "$root\build\_deps\minhook-src\src\trampoline.c"),
    (Q "$root\build\_deps\minhook-src\src\hde\hde64.c")
)

Write-Host "MSVC $($msvc.Name), SDK $sdkVer, version $version"

$common = @("/nologo", "/c", "/O2", "/MT", "/DNDEBUG", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
            (QArg "/Fo" "$out/"))

$cRsp = "$out\minhook.rsp"
($common + $includes + @("/TC") + $cSources) -join "`r`n" | Set-Content $cRsp -Encoding ASCII
& $cl "@$cRsp"
if ($LASTEXITCODE -ne 0) { throw "MinHook failed to compile" }

$cppRsp = "$out\plugin.rsp"
# Escaped quotes, or the response file eats them as grouping and PLUGIN_VERSION
# expands to a bare 0.3.0 rather than to a string literal.
$versionDefine = '/DPLUGIN_VERSION=\"' + $version + '\"'
($common + $includes + @("/std:c++20", "/EHsc", $versionDefine) + $cppSources) -join "`r`n" |
    Set-Content $cppRsp -Encoding ASCII
& $cl "@$cppRsp"
if ($LASTEXITCODE -ne 0) { throw "plugin sources failed to compile" }

$objects = Get-ChildItem $out -Filter "*.obj" | ForEach-Object { Q $_.FullName }
$linkRsp = "$out\link.rsp"
(@("/nologo", "/DLL", "/MACHINE:X64") + $libs +
 @((QArg "/OUT:" "$root\build\Release\RDR2UltrawideCutsceneFix.asi")) + $objects +
 @("kernel32.lib", "user32.lib", "version.lib", "psapi.lib")) -join "`r`n" |
    Set-Content $linkRsp -Encoding ASCII
& $link "@$linkRsp"
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Get-Item "$root\build\Release\RDR2UltrawideCutsceneFix.asi" |
    Select-Object Name, Length, LastWriteTime | Format-List

# The tests, which CMake used to build and nothing else did.
#
# Without this they simply stopped being run: build\Release\scanner_test.exe was
# still there, still passing, and four days stale -- which is worse than having
# no tests at all, because it looks like evidence.
#
# Only the sources the test actually pulls in. Linking all of src would drag in
# dllmain and MinHook for no reason.
if ($Tests) {
    $testOut = "$root\build\direct-tests"
    New-Item -ItemType Directory -Force $testOut | Out-Null
    Remove-Item "$testOut\*.obj" -ErrorAction SilentlyContinue

    $testSources = @((Q "$root\tests\scanner_test.cpp"), (Q "$root\src\mem.cpp"),
                     (Q "$root\src\log.cpp"), (Q "$root\src\dump.cpp"), (Q "$root\src\hunt.cpp"))
    $testRsp = "$testOut\tests.rsp"
    (@("/nologo", "/c", "/O2", "/MT", "/DNDEBUG", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX",
       "/std:c++20", "/EHsc", (QArg "/Fo" "$testOut/")) + $includes + $testSources) -join "`r`n" |
        Set-Content $testRsp -Encoding ASCII
    & $cl "@$testRsp"
    if ($LASTEXITCODE -ne 0) { throw "tests failed to compile" }

    $testObjects = Get-ChildItem $testOut -Filter "*.obj" | ForEach-Object { Q $_.FullName }
    $testLinkRsp = "$testOut\link.rsp"
    (@("/nologo", "/MACHINE:X64", "/SUBSYSTEM:CONSOLE") + $libs +
     @((QArg "/OUT:" "$testOut\scanner_test.exe")) + $testObjects +
     @("kernel32.lib", "user32.lib", "version.lib", "psapi.lib")) -join "`r`n" |
        Set-Content $testLinkRsp -Encoding ASCII
    & $link "@$testLinkRsp"
    if ($LASTEXITCODE -ne 0) { throw "tests failed to link" }

    & "$testOut\scanner_test.exe"
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}
