# Build (và chạy thử không giao diện) một BẢN SAO riêng của thư mục App, để nhiều người sửa các trang
# khác nhau cùng lúc mà không giẫm lên thư mục build của nhau. Bản sao dùng Core, ThirdParty và các thư
# viện đã build (build\x64\Release\lib) của cây chính.
#
#   tools\build-app-copy.ps1 -Work C:\tmp\agentX            build; lần đầu tự chép App\ sang C:\tmp\agentX\App
#   tools\build-app-copy.ps1 -Work C:\tmp\agentX -Shot      build xong chạy exe ở chế độ chụp ảnh: mô phỏng,
#                                                           dữ liệu ở thư mục tạm, cửa sổ ngoài màn hình, không
#                                                           giành focus; ảnh từng trang nằm ở <Work>\png
#   tools\build-app-copy.ps1 -Work C:\tmp\agentX -Refresh   chép đè lại App\ từ cây chính trước khi build
#                                                           (MẤT thay đổi trong bản sao)
#
# Sửa file trong <Work>\App; xong việc thì tự chép các file của mình về App\ của cây chính.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Work,
    [switch]$Shot,
    [switch]$Refresh
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$app = Join-Path $Work 'App'

if ($Refresh -or -not (Test-Path (Join-Path $app 'ShieldCanLaser.vcxproj'))) {
    New-Item -ItemType Directory -Force $app | Out-Null
    robocopy (Join-Path $root 'App') $app /E /XD obj 'Generated Files' /NFL /NDL /NJH /NJS /NP | Out-Null
    Write-Host "Copied App\ to $app"
}

$buildDir = Join-Path $Work 'build'
$log = Join-Path $Work 'build.log'
& $msbuild (Join-Path $app 'ShieldCanLaser.vcxproj') -restore -p:Configuration=Release -p:Platform=x64 `
    "-p:ScsRoot=$root" "-p:ScsBuildDir=$buildDir" -p:BuildProjectReferences=false -m -nologo -v:m 2>&1 |
    Tee-Object -FilePath $log |
    Where-Object { $_ -match '(error|fatal error) |: error|XamlCompiler|-> ' } |
    ForEach-Object { if ($_.Length -gt 500) { $_.Substring(0, 500) } else { $_ } } |
    Select-Object -First 60
$code = $LASTEXITCODE
Write-Host "BUILD exit=$code (full log: $log)"
if ($code -ne 0) { exit $code }

if ($Shot) {
    $exe = Join-Path $buildDir 'x64\Release\App\ShieldCanLaser.exe'
    $data = Join-Path $Work 'data'
    $png = Join-Path $Work 'png'
    if (Test-Path $png) { Get-ChildItem $png -Filter *.png | Remove-Item -Force }
    New-Item -ItemType Directory -Force $data, $png | Out-Null
    $p = Start-Process -FilePath $exe -ArgumentList "--root `"$data`" --shot `"$png`"" -PassThru
    if (-not $p.WaitForExit(120000)) { $p.Kill(); Write-Host 'SHOT: timed out after 120 s - process killed'; exit 3 }
    Write-Host "SHOT exit=$($p.ExitCode)"
    Get-ChildItem $png -Filter *.png | ForEach-Object { Write-Host ("  " + $_.FullName + "  " + $_.Length) }
    $appLog = Get-ChildItem (Join-Path $data 'Log') -Recurse -Filter app.log -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($appLog) {
        Write-Host "Run log ($($appLog.FullName)) - WARNING/ERROR lines:"
        Get-Content $appLog.FullName -Encoding UTF8 | Where-Object { $_ -match "`t(WARNING|ERROR)`t" } | Select-Object -Last 25 | ForEach-Object { Write-Host "  $_" }
    }
    exit $p.ExitCode
}
exit 0
