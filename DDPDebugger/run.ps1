# Builds and launches DDP Debugger with a known-good JDK (17+), regardless of whatever
# JAVA_HOME/PATH happen to be set to in the calling shell. Only affects this script's own
# process -- it does not change your shell's or system's persistent environment variables.

$ErrorActionPreference = "Stop"

$candidateJdkRoots = @(
    "C:\Program Files\Eclipse Adoptium",
    "C:\Program Files\Java",
    "C:\Program Files\Zulu",
    "C:\Program Files\Microsoft\jdk-21",
    "C:\Program Files\BellSoft"
)

function Get-JavaMajorVersion($javaExePath) {
    # `java -version` writes to stderr, and PowerShell 5.1 turns a native command's redirected
    # stderr into a terminating NativeCommandError under $ErrorActionPreference = "Stop". Use
    # Process directly so reading that stderr doesn't blow up the script.
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $javaExePath
    $psi.Arguments = "-version"
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $psi.UseShellExecute = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    $output = $proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    if ($output -match 'version "1\.(\d+)') { return [int]$Matches[1] }   # old "1.8.0_xxx" style
    if ($output -match 'version "(\d+)')     { return [int]$Matches[1] }  # "17", "21.0.12.1", etc.
    return 0
}

function Find-Jdk17Plus {
    # Prefer the calling shell's own JAVA_HOME, if it's already good enough.
    if ($env:JAVA_HOME) {
        $candidate = $env:JAVA_HOME.Trim('"')
        $javaExe = Join-Path $candidate "bin\java.exe"
        if (Test-Path $javaExe) {
            if ((Get-JavaMajorVersion $javaExe) -ge 17) { return $candidate }
        }
    }

    foreach ($root in $candidateJdkRoots) {
        if (-not (Test-Path $root)) { continue }
        $dirs = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending
        foreach ($dir in $dirs) {
            $javaExe = Join-Path $dir.FullName "bin\java.exe"
            if ((Test-Path $javaExe) -and (Get-JavaMajorVersion $javaExe) -ge 17) {
                return $dir.FullName
            }
        }
    }
    return $null
}

$jdkHome = Find-Jdk17Plus
if (-not $jdkHome) {
    Write-Error "Could not find a JDK 17+ install. Get Eclipse Temurin 21 from https://adoptium.net/ and re-run this script."
    exit 1
}

Write-Host "Using JDK: $jdkHome"
$env:JAVA_HOME = $jdkHome
$env:PATH = "$jdkHome\bin;$env:PATH"

Push-Location $PSScriptRoot
try {
    & .\mvnw.cmd javafx:run
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
