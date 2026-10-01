<#
Builds the ready-to-play folder "Down in the Dumps" from this source code and your original CDs.

The build tools (LLVM-MinGW, CMake, Ninja, Python with capstone) are downloaded once into the folder
.tools next to this source code: pinned versions, checked with SHA-256, nothing is installed and
nothing on the system is changed. Delete .tools and build to remove them again.

Usage: double-click build.cmd, or
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Game <folder with the ISO images>]
Without -Game the script uses the folder ISOs next to this source code, or asks for the folder.
#>
param([string]$Game = "")

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"         # downloads are much faster without the progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root ".tools"
$cache = Join-Path $tools "downloads"

$parts = @(
    @{ Name = "llvm-mingw"; Sha256 = "b9b68a4d276e16fa25802aaba458e4638f64b3884c290aaccdc2d87083b6ca35"
       Url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-x86_64.zip" },
    @{ Name = "cmake"; Sha256 = "4d52ebab7193a698651639ed80d8d04fd903358843572cf44c7fd234cb7c26ab"
       Url = "https://github.com/Kitware/CMake/releases/download/v4.4.3/cmake-4.4.3-windows-x86_64.zip" },
    @{ Name = "ninja"; Sha256 = "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"
       Url = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip" },
    @{ Name = "python"; Sha256 = "d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf"
       Url = "https://www.python.org/ftp/python/3.13.15/python-3.13.15-embed-amd64.zip" },
    @{ Name = "capstone"; Sha256 = "4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1"
       Url = "https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/capstone-5.0.7-py3-none-win_amd64.whl" }
)

function Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }

# SHA-256 of a file (.NET directly: works in every PowerShell, whatever modules it finds)
function Sha256($file) {
    $stream = [IO.File]::OpenRead($file)
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace "-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
    }
}

# unpacks a zip archive (tar.exe of Windows 10/11 is much faster than Expand-Archive)
function Unpack($zip, $dest) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (Test-Path $tar) {
        & $tar -xf $zip -C $dest
        if ($LASTEXITCODE -ne 0) { throw "could not unpack $zip" }
    } else {
        $copy = "$zip.zip"
        Copy-Item $zip $copy -Force
        Expand-Archive -Path $copy -DestinationPath $dest -Force
        Remove-Item $copy
    }
}

# downloads a part once, checks it, and unpacks it into .tools\<name>
function Fetch($part) {
    $dest = Join-Path $tools $part.Name
    if (Test-Path (Join-Path $dest ".done")) { return $dest }
    New-Item -ItemType Directory -Force $cache | Out-Null
    $file = Join-Path $cache ([IO.Path]::GetFileName($part.Url))
    if (-not (Test-Path $file) -or (Sha256 $file) -ne $part.Sha256) {
        Write-Host "downloading $($part.Url)"
        Invoke-WebRequest -Uri $part.Url -OutFile $file -UseBasicParsing
    }
    $hash = (Sha256 $file)
    if ($hash -ne $part.Sha256) {
        Remove-Item $file
        throw "$([IO.Path]::GetFileName($file)): wrong SHA-256 $hash (expected $($part.Sha256))"
    }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Write-Host "unpacking $($part.Name)"
    Unpack $file $dest
    New-Item -ItemType File (Join-Path $dest ".done") | Out-Null
    Remove-Item $file                                # unpacked: the archive is no longer needed
    return $dest
}

# the folder of a part that contains a file (the archives have their own top folder or none)
function Find-Dir($base, $file) {
    $hit = Get-ChildItem -Path $base -Recurse -Filter $file -File | Select-Object -First 1
    if (-not $hit) { throw "$file not found in $base" }
    return $hit.DirectoryName
}

try {
    # ---- game data
    if (-not $Game) {
        $isos = Join-Path $root "ISOs"
        if (Test-Path $isos) {
            $Game = $isos
        } else {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = "Down in the Dumps: choose the folder with the ISO images of your three original CDs"
            if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw "no game folder chosen" }
            $Game = $dialog.SelectedPath
        }
    }
    $Game = (Resolve-Path $Game).Path
    Write-Host "game data: $Game"

    # ---- build tools
    Step "build tools (downloaded once into .tools)"
    $dirs = @{}
    foreach ($part in $parts) { $dirs[$part.Name] = Fetch $part }
    $llvm = Find-Dir $dirs["llvm-mingw"] "clang++.exe"
    $cmake = Find-Dir $dirs["cmake"] "cmake.exe"
    $ninja = Find-Dir $dirs["ninja"] "ninja.exe"
    $pythonDir = $dirs["python"]
    $python = Join-Path $pythonDir "python.exe"
    # capstone for the embedded Python: its unpacked wheel goes on the module path (._pth, relative)
    $pth = Get-ChildItem -Path $pythonDir -Filter "python*._pth" | Select-Object -First 1
    if ($pth -and -not (Select-String -Path $pth.FullName -Pattern "capstone" -SimpleMatch -Quiet)) {
        Add-Content -Path $pth.FullName -Value "..\capstone"
    }
    & $python -c "import capstone"
    if ($LASTEXITCODE -ne 0) { throw "Python cannot load capstone" }
    $env:PATH = "$llvm;$cmake;$ninja;$pythonDir;$env:PATH"

    # ---- build
    $build = Join-Path $root "build"
    Step "configure (generates the game code from your DID.EXE)"
    & cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        "-DBLUB_GAME_DATA=$($Game.Replace('\', '/'))" "-DPython3_EXECUTABLE=$($python.Replace('\', '/'))"
    if ($LASTEXITCODE -ne 0) { throw "configuring failed (see above)" }
    Step "compile (takes a few minutes the first time)"
    & cmake --build $build --target blub
    if ($LASTEXITCODE -ne 0) { throw "compiling failed (see above)" }

    # ---- play folder
    $out = Join-Path $root "Down in the Dumps"
    Step "play folder"
    & $python (Join-Path $root "tools\package.py") --build $build --out $out
    if ($LASTEXITCODE -ne 0) { throw "packaging failed (see above)" }
    $isosInside = Join-Path $out "ISOs"
    $ini = Join-Path $out "blub.ini"
    if ($Game -ne (Resolve-Path $isosInside).Path -and -not (Test-Path $ini)) {
        # the ISOs stay where they are: the settings point there
        [IO.File]::WriteAllText($ini, "[game]`r`ngame_dir = $Game`r`n", (New-Object Text.UTF8Encoding $false))
    }

    Write-Host ""
    Write-Host "Done: $(Join-Path $out 'blub.exe')" -ForegroundColor Green
    Write-Host "If Windows blocks the program (Smart App Control), see README.txt in that folder."
} catch {
    Write-Host ""
    Write-Host "Error: $_" -ForegroundColor Red
    exit 1
}
