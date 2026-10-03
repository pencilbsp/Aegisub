#!/usr/bin/env powershell

param (
  [Parameter(Position = 0, Mandatory = $true)]
  [ValidateNotNullOrEmpty()]
  [string]$BuildRoot,
  [Parameter(Position = 1, Mandatory = $true)]
  [ValidateNotNullOrEmpty()]
  [string]$SourceRoot,
  [Parameter(Position = 2)]
  [ValidateSet('x64', 'arm64')]
  [string]$Architecture = 'x64',
  [ValidateNotNullOrEmpty()]
  [string]$DepCtrlVersion = "0.8.1"
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

try {
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
} catch {
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
}

$InstallerDir = Join-Path $SourceRoot "packages\win_installer" | Resolve-Path
$DepsDir = Join-Path $BuildRoot "installer-deps"
if (!(Test-Path $DepsDir)) {
	New-Item -ItemType Directory -Path $DepsDir | Out-Null
}

$Env:BUILD_ROOT = $BuildRoot
$Env:SOURCE_ROOT = $SourceRoot

$GitHeaders = @{}
if (Test-Path 'Env:GITHUB_TOKEN') {
	$GitHeaders = @{ 'Authorization' = 'Bearer ' + $Env:GITHUB_TOKEN }
}

function Download-FileWithRetry {
	param(
		[Parameter(Mandatory = $true)][string]$Url,
		[Parameter(Mandatory = $true)][string]$OutFile,
		[hashtable]$Headers = @{},
		[int]$MaxRetries = 5,
		[int]$DelaySeconds = 3
	)

	$curl = Get-Command "curl.exe" -ErrorAction SilentlyContinue
	if ($curl) {
		$curlArgs = @("-fsSL", "--retry", "$MaxRetries", "--retry-delay", "$DelaySeconds", "-o", $OutFile)
		foreach ($key in $Headers.Keys) {
			$curlArgs += "-H"
			$curlArgs += "$key`: $($Headers[$key])"
		}
		$curlArgs += $Url
		& $curl.Source @curlArgs
		if ($LASTEXITCODE -eq 0 -and (Test-Path $OutFile)) {
			return
		}
		Write-Warning "curl.exe failed with exit code $LASTEXITCODE, falling back to Invoke-WebRequest..."
	}

	for ($i = 1; $i -le $MaxRetries; $i++) {
		try {
			if ($Headers.Count -gt 0) {
				Invoke-WebRequest $Url -OutFile $OutFile -Headers $Headers -UseBasicParsing
			} else {
				Invoke-WebRequest $Url -OutFile $OutFile -UseBasicParsing
			}
			return
		} catch {
			if ($i -eq $MaxRetries) {
				throw
			}
			Write-Warning "Download from $Url failed (attempt $i/$MaxRetries): $_. Retrying in $DelaySeconds seconds..."
			Start-Sleep -Seconds $DelaySeconds
		}
	}
}

function Fetch-JsonWithRetry {
	param(
		[Parameter(Mandatory = $true)][string]$Url,
		[hashtable]$Headers = @{},
		[int]$MaxRetries = 5,
		[int]$DelaySeconds = 3
	)
	for ($i = 1; $i -le $MaxRetries; $i++) {
		try {
			if ($Headers.Count -gt 0) {
				return (Invoke-WebRequest $Url -Headers $Headers -UseBasicParsing | ConvertFrom-Json)
			} else {
				return (Invoke-WebRequest $Url -UseBasicParsing | ConvertFrom-Json)
			}
		} catch {
			if ($i -eq $MaxRetries) {
				throw
			}
			Write-Warning "Request to $Url failed (attempt $i/$MaxRetries): $_. Retrying in $DelaySeconds seconds..."
			Start-Sleep -Seconds $DelaySeconds
		}
	}
}

# DependencyControl
$DepCtrlDir = Join-Path $DepsDir "DependencyControl"

function Get-CachedDepCtrlVersion {
	param([Parameter(Mandatory = $true)][string]$Root)
	$module = Join-Path $Root "automation\include\l0\DependencyControl.moon"
	if (!(Test-Path -LiteralPath $module -PathType Leaf)) {
		return $null
	}
	$marker = Select-String -LiteralPath $module -Pattern 'version:\s*"([^"]+)".*--\s*@\{l0\.DependencyControl:version\}' |
		Select-Object -First 1
	if (!$marker) {
		return $null
	}
	$marker.Matches[0].Groups[1].Value
}

if ((Get-CachedDepCtrlVersion $DepCtrlDir) -ne $DepCtrlVersion) {
	$depCtrlUrl = "https://github.com/TypesettingTools/DependencyControl/releases/download/v$DepCtrlVersion/DependencyControl-v$DepCtrlVersion.zip"
	$depCtrlStagingDir = Join-Path $DepsDir ("DependencyControl-{0}" -f [guid]::NewGuid().ToString("N"))
	$depCtrlZip = Join-Path $depCtrlStagingDir "DependencyControl.zip"

	try {
		New-Item -ItemType Directory -Path $depCtrlStagingDir | Out-Null
		Download-FileWithRetry -Url $depCtrlUrl -OutFile $depCtrlZip
		7z x $depCtrlZip "-o$depCtrlStagingDir"
		if ($LASTEXITCODE -ne 0) {
			throw "Failed to extract DependencyControl (7z exited with code $LASTEXITCODE)"
		}

		$stagedVersion = Get-CachedDepCtrlVersion $depCtrlStagingDir
		if (!$stagedVersion) {
			throw "DependencyControl archive did not contain a versioned automation\include\l0\DependencyControl.moon"
		}
		if ($stagedVersion -ne $DepCtrlVersion) {
			throw "DependencyControl archive contains version $stagedVersion, expected $DepCtrlVersion"
		}

		Remove-Item -LiteralPath $depCtrlZip
		if (Test-Path -LiteralPath $DepCtrlDir) {
			Remove-Item -LiteralPath $DepCtrlDir -Recurse -Force
		}
		Rename-Item -LiteralPath $depCtrlStagingDir -NewName (Split-Path $DepCtrlDir -Leaf)
		Write-Host "DependencyControl v$DepCtrlVersion has been downloaded to $DepCtrlDir"
	} finally {
		if (Test-Path -LiteralPath $depCtrlStagingDir) {
			Remove-Item -LiteralPath $depCtrlStagingDir -Recurse -Force -ErrorAction SilentlyContinue
		}
	}
} else {
	Write-Host "DependencyControl v$DepCtrlVersion already cached at $DepCtrlDir"
}

# Avisynth
# $AviSynthDir = Join-Path $DepsDir "AviSynthPlus64"
# if (!(Test-Path $AviSynthDir)) {
# 	$avsReleases = Fetch-JsonWithRetry -Url "https://api.github.com/repos/AviSynth/AviSynthPlus/releases/latest" -Headers $GitHeaders
# 	$avsUrl = $avsReleases.assets[0].browser_download_url
# 	$avsArchive = Join-Path $DepsDir "AviSynthPlus.7z"
# 	Download-FileWithRetry -Url $avsUrl -OutFile $avsArchive
# 	7z x $avsArchive "-o$DepsDir"
# 	Rename-Item (Join-Path $DepsDir (Get-ChildItem -Path $DepsDir -Filter "AviSynthPlus_*" -Directory).Name) $AviSynthDir
# 	Remove-Item $avsArchive
# }

# VSFilter
$VSFilterDir = Join-Path $DepsDir "VSFilter"
$VSFilterDll = Join-Path $VSFilterDir "x64\VSFilter.dll"
if ($Architecture -eq 'x64' -and !(Test-Path $VSFilterDll)) {
	$vsFilterStagingDir = Join-Path $DepsDir ("VSFilter-{0}" -f [guid]::NewGuid().ToString("N"))
	$vsFilterArchive = Join-Path $vsFilterStagingDir "VSFilter.7z"
	try {
		New-Item -ItemType Directory -Path $vsFilterStagingDir | Out-Null
		$vsFilterReleases = Fetch-JsonWithRetry -Url "https://api.github.com/repos/pinterf/xy-VSFilter/releases/latest" -Headers $GitHeaders
		$vsFilterAsset = $vsFilterReleases.assets | Where-Object { $_.name -like "*.7z" } | Select-Object -First 1
		if (!$vsFilterAsset) {
			$vsFilterAsset = $vsFilterReleases.assets[0]
		}
		$vsFilterUrl = $vsFilterAsset.browser_download_url
		Download-FileWithRetry -Url $vsFilterUrl -OutFile $vsFilterArchive
		7z x $vsFilterArchive "-o$vsFilterStagingDir"
		if ($LASTEXITCODE -ne 0) {
			throw "Failed to extract VSFilter (7z exited with code $LASTEXITCODE)"
		}
		Remove-Item -LiteralPath $vsFilterArchive -Force
		if (Test-Path -LiteralPath $VSFilterDir) {
			Remove-Item -LiteralPath $VSFilterDir -Recurse -Force
		}
		Rename-Item -LiteralPath $vsFilterStagingDir -NewName (Split-Path $VSFilterDir -Leaf)
		Write-Host "VSFilter has been downloaded and extracted to $VSFilterDir"
	} finally {
		if (Test-Path -LiteralPath $vsFilterStagingDir) {
			Remove-Item -LiteralPath $vsFilterStagingDir -Recurse -Force -ErrorAction SilentlyContinue
		}
	}
}

# VC++ redistributable
$RedistDir = Join-Path $DepsDir "VC_redist"
$RedistName = "VC_redist.$Architecture.exe"
$RedistPath = Join-Path $RedistDir $RedistName
if (!(Test-Path $RedistPath)) {
	New-Item -ItemType Directory -Path $RedistDir -Force | Out-Null
	Download-FileWithRetry -Url "https://aka.ms/vs/17/release/$RedistName" -OutFile $RedistPath
}

# Dictionaries
$DictionariesDir = Join-Path $DepsDir "dictionaries"
if (!(Test-Path $DictionariesDir)) {
	New-Item -ItemType Directory -Path $DictionariesDir | Out-Null
	Download-FileWithRetry -Url "https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.aff" -OutFile (Join-Path $DictionariesDir "en_US.aff")
	Download-FileWithRetry -Url "https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.dic" -OutFile (Join-Path $DictionariesDir "en_US.dic")
}

# Installer localization
$LangsDir = Join-Path $DepsDir "innosetup-langs"
if (!(Test-Path $LangsDir)) {
	New-Item -ItemType Directory -Path $LangsDir | Out-Null
	$LangBaseUrl = "https://raw.github.com/jrsoftware/issrc/is-6_7_3/Files/Languages/Unofficial"
	$Languages = @(
		'Greek', 'Basque', 'Galician', 'Indonesian',
		'SerbianCyrillic', 'SerbianLatin', 'ChineseSimplified', 'ChineseTraditional'
	)
	foreach ($lang in $Languages) {
		Download-FileWithRetry -Url "$LangBaseUrl/$lang.isl" -OutFile (Join-Path $LangsDir "$lang.isl")
	}
}

# Aegisub localization
meson compile -C $BuildRoot aegisub-gmo
if(!$?) { Exit $LASTEXITCODE }

# Invoke InnoSetup
$IssUrl = Join-Path $InstallerDir "aegisub_depctrl.iss"
if ($Architecture -eq 'arm64') {
	iscc /DARM64 $IssUrl
} else {
	iscc $IssUrl
}
if(!$?) { Exit $LASTEXITCODE }
