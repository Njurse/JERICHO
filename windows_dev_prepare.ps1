$openal_ver = '1.23.1'
$sdl2_ver = '2.30.2'

$windows_premake_url = 'https://github.com/premake/premake-core/releases/download/v5.0.0-beta1/premake-5.0.0-beta1-windows.zip'
$windows_jpeg_url = 'https://www.ijg.org/files/jpegsr9d.zip'
$windows_openal_url = 'https://github.com/kcat/openal-soft/releases/download/' + $openal_ver + '/openal-soft-' + $openal_ver + '-bin.zip'
$windows_sdl2_url = 'https://github.com/libsdl-org/SDL/releases/download/release-' + $sdl2_ver + '/SDL2-devel-' + $sdl2_ver + '-VC.zip'

# SHA-256 pins (same as .github/workflows/build.yml); bump with the versions above
$ErrorActionPreference = 'Stop'
function Assert-Sha256([string]$file, [string]$want) {
  $got = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
  if ($got -ne $want) { throw "checksum mismatch for ${file}: got $got, want $want" }
}

$project_folder = '.\\src_rebuild'
$dependency_folder = $project_folder + '\\dependencies'

# Download required dependencies
echo $windows_premake_url
echo $windows_sdl2_url
echo $windows_openal_url
echo $windows_jpeg_url

Invoke-WebRequest -Uri $windows_premake_url -OutFile PREMAKE.zip
Assert-Sha256 PREMAKE.zip 'd8f958162b202fe88a2fa1897d3c5342dd137f19d1265de8802880126a808a65'
Expand-Archive PREMAKE.zip -DestinationPath $project_folder

Invoke-WebRequest -Uri $windows_sdl2_url -OutFile SDL2.zip
Assert-Sha256 SDL2.zip 'fc5d6c096a6b82f86613060dfef553a09b9e08afcb401fefac4b9ca221265cda'
Expand-Archive SDL2.zip -DestinationPath $dependency_folder

Invoke-WebRequest -Uri $windows_openal_url -OutFile OPENAL.zip
Assert-Sha256 OPENAL.zip 'ea8bc36fd7fa05f64e13400d20886de753a227202a4ea3781913489a26b36fc6'
Expand-Archive OPENAL.zip -DestinationPath $dependency_folder

Invoke-WebRequest -Uri $windows_jpeg_url -OutFile JPEG.zip
Assert-Sha256 JPEG.zip '5dc857551201d749dc0f48d26aa4cde9f6b53fe65080cbed9cb6641479119433'
Expand-Archive JPEG.zip -DestinationPath $dependency_folder

# Generate project files
$windows_jpeg_dir = '.\\dependencies\\jpeg-9d'
$windows_openal_dir = '.\\dependencies\\openal-soft-' + $openal_ver + '-bin'
$windows_sdl2_dir = '.\\dependencies\\SDL2-' + $sdl2_ver

$env:SDL2_DIR = $windows_sdl2_dir
$env:OPENAL_DIR = $windows_openal_dir
$env:JPEG_DIR = $windows_jpeg_dir

Set-Location -Path $project_folder
Rename-Item -Path ($windows_jpeg_dir + '\\jconfig.vc') -NewName "jconfig.h"

& .\\premake5 vs2022

# Open solution
& .\\project_vs2022_windows\\JERICHO.sln