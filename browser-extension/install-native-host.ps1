param(
 [string]$ExtensionDirectory = (Join-Path $PSScriptRoot 'msime-reading'),
 [string]$HostExecutable = 'C:\Program Files\metasequoiaime\server\MetasequoiaImeReadingHost.exe'
)
$ErrorActionPreference = 'Stop'
$ExtensionDirectory = (Resolve-Path -LiteralPath $ExtensionDirectory).Path
if (!(Test-Path -LiteralPath $HostExecutable)) {throw '请先安装包含 MetasequoiaImeReadingHost.exe 的 MSIME 版本。'}
$extension = Get-Content -LiteralPath (Join-Path $ExtensionDirectory 'manifest.json') -Raw | ConvertFrom-Json
$hash = [Security.Cryptography.SHA256]::Create().ComputeHash([Convert]::FromBase64String($extension.key))
$extensionId = -join ($hash[0..15] | ForEach-Object { [char](97+($_ -shr 4)); [char](97+($_ -band 15)) })
$hostDirectory = Join-Path $env:LOCALAPPDATA 'metasequoiaime\browser-reading'
New-Item -ItemType Directory -Path $hostDirectory -Force | Out-Null
$manifestPath = Join-Path $hostDirectory 'org.metasequoiaime.reading.json'
@{
 name='org.metasequoiaime.reading';description='MSIME reading translation';path=$HostExecutable;type='stdio';
 allowed_origins=@("chrome-extension://$extensionId/")
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
foreach($browser in @('Google\Chrome','Microsoft\Edge','BraveSoftware\Brave-Browser','Vivaldi')) {
 $key = "HKCU:\Software\$browser\NativeMessagingHosts\org.metasequoiaime.reading"
 New-Item -Path $key -Force | Out-Null
 Set-Item -LiteralPath $key -Value $manifestPath
}
Write-Output "Native host registered. Extension ID: $extensionId"
Write-Output "Load unpacked extension directory: $ExtensionDirectory"
