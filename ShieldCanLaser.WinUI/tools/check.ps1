# Kiểm tra cú pháp nhanh (cl /Zs — không sinh file .obj) cho một hay nhiều file .cpp của Core,
# với đúng cờ biên dịch và đường dẫn include của Core.vcxproj.
#
#   powershell -ExecutionPolicy Bypass -File tools\check.ps1 Core\EtherCat\Foo.cpp Core\Data\Bar.cpp
#
# Mã thoát 0 = không có lỗi.
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$vm = 'C:\Program Files\VisionMaster4.4.0\Development\V4.x'

if (-not $Files -or $Files.Count -eq 0) { Write-Host 'usage: check.ps1 <file.cpp> [...]'; exit 2 }

$inc = @(
    "$root\Core",
    "$root\ThirdParty",
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

$flags = '/nologo /Zs /std:c++20 /utf-8 /EHs /W3 /permissive- /bigobj /wd4819 /wd4828 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE'
$cmd = "call `"$vcvars`" >nul 2>&1 && cl $flags $($inc -join ' ') $($src -join ' ')"
cmd /c $cmd
exit $LASTEXITCODE
