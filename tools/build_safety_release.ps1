param([Parameter(Mandatory=$true)][string]$QtRoot)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$nativeRoot=Join-Path $repo 'output/native-build'
$src=Join-Path $nativeRoot 'CloudCompare'
$build=Join-Path $nativeRoot 'build'
$cmake='cmake'
function Checked([string]$exe,[string[]]$items) { & $exe @items; if($LASTEXITCODE){throw "Command failed: $exe ($LASTEXITCODE)"} }
New-Item -ItemType Directory -Path $nativeRoot -Force | Out-Null
Checked git @('clone','--branch','v2.13.2','--depth','1','https://github.com/CloudCompare/CloudCompare.git',$src)
$commit=(& git -C $src rev-parse HEAD).Trim()
if($commit -ne '49dbbb662f296c7780aae717897c85b3cb3764ed'){throw 'Unapproved host source revision'}
Checked git @('-C',$src,'submodule','update','--init','--recursive','libs/qCC_db/extern/CCCoreLib')
New-Item -ItemType Directory -Path "$src/plugins/private" -Force | Out-Null
New-Item -ItemType Junction -Path "$src/plugins/private/qArchaeoLiDAR" -Target "$repo/plugin" | Out-Null
Checked $cmake @('-S',$src,'-B',$build,'-G','Visual Studio 17 2022','-A','x64',"-DCMAKE_PREFIX_PATH=$QtRoot","-DQT5_ROOT_PATH=$QtRoot",'-DALIS_BUILD_TESTS=ON','-DBUILD_TESTING=ON','-DPLUGIN_STANDARD_ALIS=ON','-DOPTION_BUILD_CCVIEWER=OFF')
$targets=@('ALIS_PLUGIN','ALiS_prepare_block','ALiS_annotation_tests','ALiS_feature_safety_tests','ALiS_processing_tests','ALiS_session_tests','ALiS_persistence_tests','ALiS_ground_filter_tests','ALiS_logic_tests','ALiS_workspace_logic_tests')
Checked $cmake (@('--build',$build,'--config','Release','--target')+$targets+@('--parallel','4'))
$dllDirs=Get-ChildItem -LiteralPath $build -Filter '*.dll' -Recurse | ForEach-Object DirectoryName | Select-Object -Unique
$env:PATH="$QtRoot/bin;"+($dllDirs -join ';')+';'+$env:PATH
$env:QT_QPA_PLATFORM='offscreen'
Checked ctest @('--test-dir',$build,'-C','Release','--output-on-failure','-R','^ALiS[.]')
$env:ALIS_NATIVE_DIR="$build/plugins/private/qArchaeoLiDAR/Release"
"ALIS_NATIVE_DIR=$env:ALIS_NATIVE_DIR" | Out-File -LiteralPath $env:GITHUB_ENV -Append -Encoding utf8
