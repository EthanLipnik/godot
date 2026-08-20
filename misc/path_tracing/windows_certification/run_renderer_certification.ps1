param(
    [Parameter(Mandatory = $true)]
    [string]$GodotRoot,
    [Parameter(Mandatory = $true)]
    [string]$BundleRoot,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$ReplayExecutable,
    [int]$Jobs = 12
)

$ErrorActionPreference = "Stop"
$bundleRoot = (Resolve-Path $BundleRoot).Path
$godotRoot = (Resolve-Path $GodotRoot).Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

& python (Join-Path $bundleRoot "validate_bundle.py") $bundleRoot
if ($LASTEXITCODE -ne 0) {
    throw "The transferred certification bundle failed integrity validation."
}

$manifest = Get-Content (Join-Path $bundleRoot "bundle_manifest.json") -Raw | ConvertFrom-Json
if ($manifest.working_tree_dirty_at_preparation) {
    throw "The bundle was prepared from a dirty worktree and is ineligible for certification."
}
$actualRevision = (& git -C $godotRoot rev-parse HEAD | Out-String).Trim()
if ($actualRevision -ne $manifest.repository_revision) {
    throw "Checkout revision $actualRevision does not match bundle revision $($manifest.repository_revision)."
}
if ((& git -C $godotRoot status --porcelain | Out-String).Trim()) {
    throw "Windows certification requires a clean checkout."
}

& (Join-Path $bundleRoot "probe_environment.ps1") -OutputPath (Join-Path $OutputDirectory "environment.json")
& (Join-Path $bundleRoot "build_editor.ps1") -GodotRoot $godotRoot -Jobs $Jobs

$editor = Join-Path $godotRoot "bin\godot.windows.editor.dev.x86_64.exe"
if (-not (Test-Path $editor)) {
    throw "Expected Windows editor output is missing: $editor"
}
$testLog = Join-Path $OutputDirectory "path_tracing_tests.log"
& $editor --headless --test "--test-case=*PathTracing*" 2>&1 | Tee-Object -FilePath $testLog
if ($LASTEXITCODE -ne 0) {
    throw "Focused path-tracing tests failed. See $testLog"
}

$status = [ordered]@{
    schema = 1
    repository_revision = $actualRevision
    environment_recorded = $true
    editor_built = $true
    focused_tests_passed = $true
    vulkan_replay_executed = $false
    renderer_component_passed = $false
    m3_passed = $false
    blocker = "vulkan_replay_executable_missing"
}
if (-not $ReplayExecutable) {
    $status | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $OutputDirectory "certification_status.json")
    throw "Build/tests passed, but M3 Vulkan replay is not implemented. Supply -ReplayExecutable after the backend runner exists."
}
if (-not (Test-Path $ReplayExecutable)) {
    throw "Vulkan replay executable not found: $ReplayExecutable"
}

$rendererReport = Join-Path $OutputDirectory "vulkan_renderer_report.json"
& $ReplayExecutable `
    --capture (Join-Path $bundleRoot "reference.scene_capture.ptc") `
    --expected (Join-Path $bundleRoot "expected_results.json") `
    --output $rendererReport
if ($LASTEXITCODE -ne 0) {
    throw "Vulkan renderer replay failed with exit code $LASTEXITCODE."
}
& python (Join-Path $bundleRoot "validate_renderer_report.py") $rendererReport
if ($LASTEXITCODE -ne 0) {
    throw "Vulkan renderer report failed acceptance validation."
}
$status.vulkan_replay_executed = $true
$status.renderer_component_passed = $true
$status.blocker = "metal_vulkan_parity_and_physical_vision_pro_evidence_pending"
$status | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 (Join-Path $OutputDirectory "certification_status.json")
$status | ConvertTo-Json -Depth 6
