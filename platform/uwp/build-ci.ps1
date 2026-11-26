
$ErrorActionPreference = "Stop"
$workDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $workDir "..\..")).Path
$borealisRepo = "https://github.com/thedavidweng/borealis.git"
$borealisRef = "winrt-dev"
$borealisCommit = "789b230cb4e50ff2098ae872a6bfe3f1d835f25b"

function Ensure-GitCheckout {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Repo,
        [Parameter(Mandatory = $true)][string]$Ref,
        [Parameter(Mandatory = $true)][string]$Commit
    )

    $newCheckout = $false

    if (-not (Test-Path $Path)) {
        Write-Host "Cloning $Path..."
        & git clone --no-checkout $Repo $Path
        if ($LASTEXITCODE -ne 0) { Write-Error "git clone failed for $Path"; exit 1 }
        $newCheckout = $true
    }

    if (-not (Test-Path (Join-Path $Path ".git"))) {
        Write-Error "$Path exists but is not a git checkout"
        exit 1
    }

    Push-Location $Path
    try {
        if (-not $newCheckout) {
            $changes = & git status --porcelain
            if ($changes) {
                Write-Error "$Path has local changes; clean it before building"
                exit 1
            }
        }

        $origin = (& git config --get remote.origin.url).Trim()
        if ($origin -ne $Repo) {
            & git remote set-url origin $Repo
            if ($LASTEXITCODE -ne 0) { Write-Error "failed to set origin for $Path"; exit 1 }
        }

        Write-Host "Fetching $Path $Ref at $Commit..."
        & git fetch --depth 1 origin $Commit
        if ($LASTEXITCODE -ne 0) { Write-Error "git fetch failed for $Path"; exit 1 }

        & git checkout --detach $Commit
        if ($LASTEXITCODE -ne 0) { Write-Error "git checkout failed for $Path at $Commit"; exit 1 }

        $actualCommit = (& git rev-parse HEAD).Trim()
        if ($actualCommit -ne $Commit) {
            Write-Error "$Path checkout mismatch: expected $Commit but got $actualCommit"
            exit 1
        }
    }
    finally {
        Pop-Location
    }
}

if (-not ($env:VCPKG_ROOT)) {
    $env:VCPKG_ROOT = $env:VCPKG_INSTALLATION_ROOT
}
if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
    Write-Error "No vcpkg.exe"
    exit 1
}

Write-Host "VCPKG_ROOT: $env:VCPKG_ROOT"

if (-not (Test-Path '.\libs\mpv\lib\mpv.lib')) {
    $mpvUrl = 'https://github.com/ikas-mc/wiliwili-uwp-poc/releases/download/0.4/x64-uwp-mpv.zip'
    $mpvHash = '82f8ce29700bf2c586d64c991f602b78c3e9a560c9094f07572db7f7e3ab7cef'
    Write-Host "Downloading mpv..."
    & curl.exe -L -o '.\x64-uwp-mpv.zip' $mpvUrl

    $actualHash = (Get-FileHash '.\x64-uwp-mpv.zip' -Algorithm SHA256).Hash.ToLower()
    if ($actualHash -ne $mpvHash) {
        Write-Error "x64-uwp-mpv.zip hash mismatch: expected $mpvHash but got $actualHash"
        exit 1
    }

    Expand-Archive '.\x64-uwp-mpv.zip' -DestinationPath '.\libs\mpv\' -Force
}
if (-not (Test-Path '.\libs\mpv\lib\mpv.lib')) {
    Write-Error "Failed to install x64-uwp-mpv.zip"
    exit 1
}

Push-Location $workDir
try {
    Ensure-GitCheckout -Path "borealis" -Repo $borealisRepo -Ref $borealisRef -Commit $borealisCommit

    Push-Location $repoRoot
    try {
        foreach ($sub in @("library/libpdr", "library/pystring", "library/mongoose")) {
            $subPath = Join-Path $repoRoot $sub
            if (-not (Test-Path (Join-Path $subPath ".git"))) {
                & git submodule update --init --depth 1 -- $sub
                if ($LASTEXITCODE -ne 0) { Write-Error "submodule update failed for $sub"; exit 1 }
            }
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    Pop-Location
}

Set-Location $workDir

# update build version
if ($env:VERSION_BUILD_NUMBER) {
    $appxManifestPath = Convert-Path ".\wiliwili-uwp\package.appxManifest"
    [xml]$manifest = Get-Content -Path $appxManifestPath
    $version = $manifest.Package.Identity.Version
    $versionParts = $version -split '\.'
    if ($versionParts.Length -eq 4) {
        $versionParts[3] = $env:VERSION_BUILD_NUMBER
        $manifest.Package.Identity.Version = $versionParts -join "."
    }
    else {
        Write-Error "Version format error: $version"
        exit 1
    }
    Write-Host "package new version: $($manifest.Package.Identity.Version)"
    $manifest.Save($appxManifestPath)
}

$cmakeArgs = @("--preset=uwp-release")
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsVer = (& $vswhere -latest -products * -property installationVersion | Select-Object -First 1)
    Write-Host "Detected Visual Studio installationVersion: $vsVer"
    if ($vsVer -match '^18\.') {
        $cmakeArgs += @("-G", "Visual Studio 18 2026")
    } elseif ($vsVer -match '^17\.') {
        $cmakeArgs += @("-G", "Visual Studio 17 2022")
    }
}

Write-Host "Running cmake $($cmakeArgs -join ' ')..."
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Error "cmake failed"; exit 1 }

Write-Host "Running msbuild..."
& msbuild build\wiliwili-uwp.vcxproj /m /p:configuration="release" /p:platform="x64" /p:AppxBundlePlatforms="x64" /p:UapAppxPackageBuildMode="SideloadOnly" /p:PackageOptionalProjectsInIdeBuilds=False
if ($LASTEXITCODE -ne 0) { Write-Error "msbuild failed"; exit 1 }

Write-Host "Build completed successfully."
