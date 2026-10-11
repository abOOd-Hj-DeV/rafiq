param(
    [string]$Sdk = "$env:LOCALAPPDATA\Android\Sdk",
    [string]$JavaHome = "C:\Program Files\Android\Android Studio\jbr",
    [string]$Output = ""
)
$ErrorActionPreference = "Stop"
if (-not $Output) { $Output = Join-Path $PSScriptRoot "..\..\build-red-demo-android" }
$Output = [IO.Path]::GetFullPath($Output)
$tools = Join-Path $Sdk "build-tools\35.0.0"
$android = Join-Path $Sdk "platforms\android-35\android.jar"
function Invoke-Checked([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "$program exited with $LASTEXITCODE" }
}
New-Item -ItemType Directory -Force -Path $Output, "$Output\classes", "$Output\dex" | Out-Null
Invoke-Checked "$JavaHome\bin\javac.exe" @("-encoding", "UTF-8", "-source", "8", "-target", "8", "-bootclasspath", $android, "-d", "$Output\classes", "$PSScriptRoot\MainActivity.java")
$classes = @(Get-ChildItem "$Output\classes" -Recurse -Filter "*.class" | ForEach-Object { $_.FullName })
Invoke-Checked "$JavaHome\bin\java.exe" (@("-cp", "$tools\lib\d8.jar", "com.android.tools.r8.D8", "--min-api", "23", "--lib", $android, "--output", "$Output\dex") + $classes)
Invoke-Checked "$tools\aapt.exe" @("package", "-f", "-M", "$PSScriptRoot\AndroidManifest.xml", "-I", $android, "-F", "$Output\unsigned.apk")
Invoke-Checked "$JavaHome\bin\jar.exe" @("uf", "$Output\unsigned.apk", "-C", "$Output\dex", "classes.dex")
Invoke-Checked "$tools\zipalign.exe" @("-f", "4", "$Output\unsigned.apk", "$Output\aligned.apk")
if (-not (Test-Path "$Output\demo-signing.jks")) {
    Invoke-Checked "$JavaHome\bin\keytool.exe" @("-genkeypair", "-keystore", "$Output\demo-signing.jks", "-storepass", "android", "-keypass", "android", "-alias", "demo", "-dname", "CN=K230 Demo", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000")
}
Invoke-Checked "$JavaHome\bin\java.exe" @("-jar", "$tools\lib\apksigner.jar", "sign", "--ks", "$Output\demo-signing.jks", "--ks-pass", "pass:android", "--out", "$Output\k230-red-demo.apk", "$Output\aligned.apk")
Invoke-Checked "$JavaHome\bin\java.exe" @("-jar", "$tools\lib\apksigner.jar", "verify", "--verbose", "$Output\k230-red-demo.apk")
Write-Output "Built: $Output\k230-red-demo.apk"
