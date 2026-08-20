param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

function Invoke-CapturedCommand {
    param(
        [string]$Command,
        [string[]]$Arguments
    )
    $resolved = Get-Command $Command -ErrorAction SilentlyContinue
    if ($null -eq $resolved) {
        return @{
            available = $false
            path = $null
            output = $null
        }
    }
    $output = & $resolved.Source @Arguments 2>&1 | Out-String
    return @{
        available = $true
        path = $resolved.Source
        output = $output.Trim()
    }
}

$visualStudio = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $visualStudio = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
}

$report = [ordered]@{
    schema = 1
    timestamp_utc = (Get-Date).ToUniversalTime().ToString("o")
    computer_name = $env:COMPUTERNAME
    os = [ordered]@{
        caption = (Get-CimInstance Win32_OperatingSystem).Caption
        version = [System.Environment]::OSVersion.Version.ToString()
        build = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion").CurrentBuildNumber
    }
    cpu = (Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
    gpu = (Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterRAM)
    visual_studio = $visualStudio
    windows_sdk_dir = $env:WindowsSdkDir
    windows_sdk_version = $env:WindowsSDKVersion
    vulkan_sdk = $env:VULKAN_SDK
    tools = [ordered]@{
        python = Invoke-CapturedCommand "python" @("--version")
        scons = Invoke-CapturedCommand "scons" @("--version")
        cl = Invoke-CapturedCommand "cl" @()
        vulkaninfo = Invoke-CapturedCommand "vulkaninfo" @("--summary")
        nvidia_smi = Invoke-CapturedCommand "nvidia-smi" @(
            "--query-gpu=name,driver_version,pci.bus_id,memory.total",
            "--format=csv,noheader"
        )
    }
}

$parent = Split-Path -Parent $OutputPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$report | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 $OutputPath
$report | ConvertTo-Json -Depth 8
