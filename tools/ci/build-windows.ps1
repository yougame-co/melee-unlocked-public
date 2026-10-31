param([Parameter(Mandatory)][ValidateSet('validate','inputs','toolchain','dll','host','package','checks')][string]$Step)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$Repo = (Resolve-Path "$PSScriptRoot/../..").Path
$Work = Join-Path $env:RUNNER_TEMP 'yougame-ci'
$Logs = Join-Path $Work 'logs'
$Runtime = Join-Path $Work 'runtime'
$Mingw = Join-Path $env:RUNNER_TEMP 'winlibs/mingw64'
$DllBuild = Join-Path $Work 'build-game'
$HostBuild = Join-Path $Work 'build-host'
$Packages = Join-Path $Work 'packages'
$Out = Join-Path $Work 'out'
New-Item -ItemType Directory -Force $Logs | Out-Null

function Native([string]$Log, [string]$Program, [string[]]$Arguments) {
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $Logs $Log) -Append
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE); see $Log" }
}
function RequireFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file missing: $Path" }
}
function CopyInput([string]$RelativePath, [string]$Sha, [string]$Name) {
    if ($RelativePath -cnotmatch '^tools/ci/inputs/[a-zA-Z0-9_-]+\.(patch|json)$') { throw 'Inputs must be files directly under tools/ci/inputs/' }
    $Source = Join-Path $Repo $RelativePath
    RequireFile $Source
    if ((Get-Item -LiteralPath $Source).LinkType) { throw 'Input must be a regular repository file' }
    if ((Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash.ToLower() -cne $Sha.ToLower()) { throw "Source input SHA-256 mismatch: $Name" }
    New-Item -ItemType Directory -Force "$Work/inputs" | Out-Null
    Copy-Item -LiteralPath $Source -Destination "$Work/inputs/$Name"
}

Push-Location $Repo
Start-Transcript -Path (Join-Path $Logs "$Step.log") -Append | Out-Null
try {
    switch ($Step) {
        validate {
            # 0.9.82 changed nothing the headers are generated from (port/recomp, port/runtime/hle and the
            # decomp gitlink are identical to 0.9.81), so the approved 0.9.81 headers stay exact for it.
            if ($env:BUILD_VERSION -cnotin @('0.9.81', '0.9.82') -or $env:BUILD_VERSION -cne (Get-Content VERSION -Raw).Trim()) {
                throw 'This workflow and its approved headers are pinned to source versions 0.9.81 and 0.9.82'
            }
            if ($env:BUILD_KEY -cnotmatch '^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$') { throw 'Invalid build_key' }
            if ($env:WIN_NET -cnotmatch '^[a-z][a-z0-9_-]{0,31}$') { throw 'Invalid win_net' }
            if ($env:WIN_DIRECT -cnotin @('true','false')) { throw 'Invalid win_direct' }
            foreach ($Name in @('UPSTREAM_ZIP_SHA256','WINLIBS_SHA256','SOURCE_PATCH_SHA256','LAUNCH_SPEC_SHA256')) {
                $Value = [Environment]::GetEnvironmentVariable($Name)
                if ($Value -notmatch '^[a-fA-F0-9]{64}$') { throw "Invalid digest: $Name" }
            }
            $Headers = @{
                'hle_decls.h' = '65a8b1ea8119d18313601dff2efc55dea50607a6235bd5922bd2e160ea433f42'
                'guest_symbols.h' = '32f6aad8794ec63835dabc0be2907686036810b37e37ea4e0fdad26e5947c7f0'
            }
            foreach ($Name in $Headers.Keys) {
                if ((Get-FileHash "port/generated/$Name" -Algorithm SHA256).Hash.ToLower() -cne $Headers[$Name]) {
                    throw "Approved header differs: $Name"
                }
            }
            $Generated = @(Get-ChildItem port/generated -File)
            if ($Generated.Count -ne 2) { throw 'Only the two approved headers may be present in port/generated' }
            "artifact_name=yougame-windows-$($env:BUILD_VERSION)-$($env:BUILD_KEY)-$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)" >> $env:GITHUB_OUTPUT
        }
        inputs {
            CopyInput $env:SOURCE_PATCH_PATH $env:SOURCE_PATCH_SHA256 'yougame-online.patch'
            Native downloads.log python @('tools/ci/inputs.py','check-patch',"$Work/inputs/yougame-online.patch")
            # CI changes these build files itself; validate the patch against all other source paths.
            Native source-verify.log git @('apply','--reverse','--check','--exclude=port/CMakeLists.txt',
                '--exclude=.gitignore','--exclude=.gitattributes',"$Work/inputs/yougame-online.patch")
            CopyInput $env:LAUNCH_SPEC_PATH $env:LAUNCH_SPEC_SHA256 'yougame-native.json'
            Native source-verify.log python @('tools/ci/inputs.py','check-spec',"$Work/inputs/yougame-native.json")
            # The only initialized submodule is public doldecomp/melee, at the gitlink pin.
            Native submodule.log git @('-c','submodule.sourceport/extern/melee.url=https://github.com/doldecomp/melee.git',
                'submodule','update','--init','--','sourceport/extern/melee')
            $env:UPSTREAM_ZIP_URL = "https://github.com/hero88go/melee-unlocked/releases/download/v$($env:BUILD_VERSION)/MeleeUnlocked-$($env:BUILD_VERSION)-win64.zip"
            Native downloads.log python @('tools/ci/inputs.py','download','UPSTREAM_ZIP_URL','UPSTREAM_ZIP_SHA256',"$Work/upstream.zip")
            Native extract.log python @('tools/ci/inputs.py','extract-runtime',"$Work/upstream.zip",$Runtime)
        }
        toolchain {
            # Pinned UCRT/POSIX/SEH x86-64 release. Dispatch must supply a separately verified digest.
            $env:WINLIBS_URL = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-13.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-13.0.0-r3.7z'
            $Archive = Join-Path $env:RUNNER_TEMP 'winlibs-download/winlibs.7z'
            Native winlibs-download.log python @('tools/ci/inputs.py','download','WINLIBS_URL','WINLIBS_SHA256',$Archive)
            Native winlibs-extract.log 7z @('x',$Archive,"-o$($env:RUNNER_TEMP)/winlibs",'-y')
            foreach ($Name in @('gcc.exe','g++.exe','ninja.exe')) { RequireFile "$Mingw/bin/$Name" }
            Native versions.log "$Mingw/bin/gcc.exe" @('--version')
            Native versions.log cmake @('--version')
            Native versions.log python @('--version')
            # The newest Visual Studio with the x64 C++ tools (windows-latest moved to VS 2026). Like pc2, the
            # matching CMake generator (17 2022 or 18 2026) is pointed at that instance with CMAKE_GENERATOR_INSTANCE.
            $Vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
            $Vs = & $Vswhere -latest -version '[17.0,' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($LASTEXITCODE -ne 0 -or -not $Vs) { throw 'Visual Studio (2022 or newer) x64 C++ tools missing from runner' }
            Native versions.log $Vswhere @('-latest','-version','[17.0,','-products','*','-property','catalog_productDisplayVersion')
            $VsMajor = ((& $Vswhere -latest -version '[17.0,' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion) -split '\.')[0]
            $VsGen = switch ($VsMajor) { '17' { 'Visual Studio 17 2022' } '18' { 'Visual Studio 18 2026' } default { throw "Unsupported Visual Studio major version $VsMajor" } }
            Set-Content -LiteralPath "$Work/vs2022.txt" -Value $Vs
            Set-Content -LiteralPath "$Work/vs-generator.txt" -Value $VsGen
            "Visual Studio: $Vs" | Add-Content "$Logs/versions.log"
        }
        dll {
            Native prepare.log python @('tools/prepare_native_sources.py')
            $OldPath = $env:PATH
            try {
                $env:PATH = "$Mingw/bin;$OldPath"
                Native dll-configure.log cmake @('-S','sourceport/game','-B',$DllBuild,'-G','Ninja',
                    '-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_TOOLCHAIN_FILE=$Repo/sourceport/cmake/mingw-w64-x86_64.cmake",
                    "-DMELEE_MINGW_ROOT=$Mingw")
                Native dll-build.log cmake @('--build',$DllBuild,'--target','melee_game','--parallel','2')
            } finally { $env:PATH = $OldPath }
        }
        host {
            $Vs = (Get-Content "$Work/vs2022.txt" -Raw).Trim()
            $VsGen = (Get-Content "$Work/vs-generator.txt" -Raw).Trim()
            & "$Vs/Common7/Tools/Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
            (Get-Item (Get-Command cl.exe).Source).VersionInfo.FileVersion | Add-Content "$Logs/versions.log"
            $Sl = Join-Path $Repo 'port/third_party/streamline/bin/x64'
            New-Item -ItemType Directory -Force $Sl | Out-Null
            foreach ($Dll in @('sl.interposer.dll','sl.common.dll','sl.dlss.dll','nvngx_dlss.dll','sl.dlss_d.dll','nvngx_dlssd.dll','sl.dlss_g.dll','nvngx_dlssg.dll','sl.reflex.dll','sl.pcl.dll')) {
                if (Test-Path "$Runtime/$Dll") { Copy-Item "$Runtime/$Dll" $Sl -Force }
            }
            foreach ($Need in @('port/third_party/streamline/include/sl.h','port/third_party/streamline/lib/x64/sl.interposer.lib',"$Sl/sl.interposer.dll","$Sl/sl.common.dll")) { RequireFile $Need }
            Native host-configure.log cmake @('-S','.','-B',$HostBuild,'-G',$VsGen,'-A','x64',
                "-DCMAKE_GENERATOR_INSTANCE=$Vs",'-DMELEE_BUILD_EXPERIMENTAL_PORT=ON','-DMELEE_SOURCE_ONLY=ON',
                '-DMELEE_REQUIRE_STREAMLINE=ON','-DYOUGAME_AGENT_TEST_HOOK=ON')
            Native host-build.log cmake @('--build',$HostBuild,'--config','Release','--target','melee_source','--parallel','2')
            # Reuse the configured tree: the two hook-sensitive targets recompile; staging is saved first.
            New-Item -ItemType Directory -Force "$Work/staging-host" | Out-Null
            Copy-Item "$HostBuild/port/Release/melee_source.exe","$HostBuild/port/Release/melee_source.map" "$Work/staging-host/"
            Native host-prod-configure.log cmake @('-S','.','-B',$HostBuild,'-DYOUGAME_AGENT_TEST_HOOK=OFF')
            Native host-prod-build.log cmake @('--build',$HostBuild,'--config','Release','--target','melee_source','--parallel','2')
        }
        package {
            New-Item -ItemType Directory -Force $Packages | Out-Null
            foreach ($Variant in @('staging','prod')) {
                $HostFiles = if ($Variant -eq 'staging') { "$Work/staging-host" } else { "$HostBuild/port/Release" }
                Native "audit-$Variant.log" python @('tools/audit_source_provenance.py','--host-map',"$HostFiles/melee_source.map",
                    '--game-map',"$DllBuild/melee_game.map",'--sources',"$DllBuild/native-game-sources.txt",
                    '--host-exe',"$HostFiles/melee_source.exe",'--game-dll',"$DllBuild/melee_game.dll",'--out',"$Logs/audit-$Variant.json")
                $Stage = Join-Path $Work "stage-$Variant/MeleeUnlocked"
                New-Item -ItemType Directory -Force "$Stage/source","$Stage/deltas" | Out-Null
                Copy-Item "$Runtime/*.dll","$Runtime/dxr_pathtrace.dxil" "$Stage/"
                Copy-Item -Recurse "$Runtime/Sys","$Runtime/lang","$Runtime/licenses","$Runtime/Shaders" "$Stage/"
                Copy-Item "$HostFiles/melee_source.exe","$DllBuild/melee_game.dll","$DllBuild/melee_game.snapexcl" "$Stage/" -Force
                Copy-Item "$Runtime/README.txt" "$Stage/README-MeleeUnlocked.txt"
                Copy-Item "$Work/inputs/yougame-online.patch" "$Stage/source/"
                $Manifest = Get-Content port/sys_deltas/manifest.json -Raw | ConvertFrom-Json
                if (@($Manifest.files).Count -ne 2) { throw 'Expected both Sys delta targets' }
                foreach ($Name in (@('manifest.json') + @($Manifest.files | ForEach-Object { $_.delta }))) {
                    if ($Name -cnotin @('manifest.json','GameSetup_gui.dat.vcdiff','slpCSS.dat.vcdiff')) { throw 'Unexpected delta filename' }
                    Copy-Item -LiteralPath "port/sys_deltas/$Name" -Destination "$Stage/deltas/"
                }
                Native "package-$Variant.log" python @('tools/ci/pack-r2.py','stage-extras',$Stage,"$Work/inputs/yougame-native.json")
                & "$PSScriptRoot/srcreadme.ps1" -Stage $Stage -Tag "v$($env:BUILD_VERSION)"
                $CiSource = "`nCI build configuration and the two approved fact headers: https://github.com/yougame-co/melee-unlocked/tree/$($env:GITHUB_SHA)`nSee tools/ci/README.md at that commit. MELEE_SOURCE_ONLY=ON; no static recompilation.`n"
                [IO.File]::AppendAllText("$Stage/source/README.txt", $CiSource, [Text.UTF8Encoding]::new($false))
                $SpecPath = "$Stage/yougame-native.json"
                $Spec = Get-Content $SpecPath -Raw | ConvertFrom-Json
                if (@($Spec.args) -notcontains '--vanilla-only') { $Spec.args = @($Spec.args) + '--vanilla-only' }
                $Spec.net = $env:WIN_NET
                if ($env:WIN_DIRECT -eq 'true') { $Spec | Add-Member -NotePropertyName direct -NotePropertyValue $true -Force }
                else { $Spec.PSObject.Properties.Remove('direct') }
                [IO.File]::WriteAllText($SpecPath, ($Spec | ConvertTo-Json -Depth 16), [Text.UTF8Encoding]::new($false))
                $Suffix = if ($Variant -eq 'prod') { '-prod' } else { '' }
                $Zip = "$Packages/MeleeUnlocked-YouGame-$($env:BUILD_VERSION)-$($env:BUILD_KEY)$Suffix-win64.zip"
                Native "package-$Variant.log" python @('tools/ci/pack-r2.py','zip',$Stage,$Zip)
            }
        }
        checks {
            Native verify-package.log python @('tools/ci/verify-package.py',$Packages,'--net',$env:WIN_NET,'--direct',$env:WIN_DIRECT)
            New-Item -ItemType Directory -Force $Out | Out-Null
            foreach ($Zip in Get-ChildItem $Packages -Filter '*.zip') {
                $Hash = (Get-FileHash $Zip.FullName -Algorithm SHA256).Hash.ToLower()
                [IO.File]::WriteAllText("$Out/$($Zip.Name).sha256", "$Hash  $($Zip.Name)`n", [Text.UTF8Encoding]::new($false))
                Move-Item $Zip.FullName $Out
            }
            $Record = [ordered]@{ version=$env:BUILD_VERSION; build_key=$env:BUILD_KEY; commit=$env:GITHUB_SHA;
                run_id=$env:GITHUB_RUN_ID; run_attempt=$env:GITHUB_RUN_ATTEMPT; unsigned=$true;
                upstream_zip_sha256=$env:UPSTREAM_ZIP_SHA256; source_patch_sha256=$env:SOURCE_PATCH_SHA256;
                launch_spec_sha256=$env:LAUNCH_SPEC_SHA256; winlibs_sha256=$env:WINLIBS_SHA256;
                win_net=$env:WIN_NET; win_direct=($env:WIN_DIRECT -eq 'true') }
            $Record | ConvertTo-Json | Set-Content "$Logs/build-record.json" -Encoding utf8
        }
    }
} finally {
    Stop-Transcript | Out-Null
    Pop-Location
}
