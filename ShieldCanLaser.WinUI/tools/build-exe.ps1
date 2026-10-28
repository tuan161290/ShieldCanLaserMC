# Biên dịch + liên kết một exe console từ danh sách file nguồn, cùng cờ và include với Core.vcxproj.
# Dùng để chạy thử nhanh một nhóm file mà không cần build cả solution.
#
#   powershell -ExecutionPolicy Bypass -File tools\build-exe.ps1 -Out C:\tmp\t\test.exe Core\Base\Text.cpp my_main.cpp
#
# -CoreLibs: liên kết thêm Core.lib + Soem.lib đã build sẵn (build\x64\Release\lib) và các thư viện hệ
#            thống/Npcap/VisionMaster chúng cần — khi đó chỉ đưa vào các file CHƯA có trong Core.lib.
# File .obj nằm cạnh exe (mỗi người dùng một thư mục -Out riêng để không giẫm nhau).
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$Libs = @(),
    [switch]$CoreLibs,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$vm = 'C:\Program Files\VisionMaster4.4.0\Development\V4.x'

$outDir = Split-Path -Parent $Out
New-Item -ItemType Directory -Force $outDir | Out-Null

$inc = @(
    "$root\Core",
    "$root\ThirdParty",
    "$root\Tests\CoreTests",
    "$root\ThirdParty\soem\include",
    "$root\ThirdParty\soem\osal",
    "$root\ThirdParty\soem\osal\win32",
    "$root\ThirdParty\soem\oshw\win32",
    "$root\ThirdParty\soem\oshw\win32\wpcap\Include",
    "$vm\Includes"
) | ForEach-Object { "/I `"$_`"" }

$src = $Files | ForEach-Object {
    $p = if ([System.IO.Path]::IsPathRooted($_)) { $_ } else { Join-Path $root $_ }
    "`"$p`""
}

$linkArgs = @($Libs | ForEach-Object { "`"$_`"" })
if ($CoreLibs) {
    $linkArgs += "`"$root\build\x64\Release\lib\Core.lib`""
    $linkArgs += "`"$root\build\x64\Release\lib\Soem.lib`""
    $linkArgs += "/LIBPATH:`"$root\ThirdParty\soem\oshw\win32\wpcap\Lib\x64`""
    $linkArgs += "/LIBPATH:`"$vm\Libraries\win64\C`""
    $linkArgs += 'wpcap.lib Packet.lib ws2_32.lib winmm.lib iphlpapi.lib iMVS-6000PlatformSDK.lib delayimp.lib'
    $linkArgs += '/DELAYLOAD:wpcap.dll /DELAYLOAD:Packet.dll /DELAYLOAD:iMVS-6000PlatformSDK.dll /IGNORE:4199'
}

$flags = '/nologo /std:c++20 /utf-8 /EHs /W3 /permissive- /bigobj /wd4819 /wd4828 /MD /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE'
$link = "/link /OUT:`"$Out`" $($linkArgs -join ' ')"
$cmd = "call `"$vcvars`" >nul 2>&1 && cl $flags /Fo`"$outDir\\`" $($inc -join ' ') $($src -join ' ') $link"
cmd /c $cmd
exit $LASTEXITCODE
