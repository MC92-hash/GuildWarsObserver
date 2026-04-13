<#
.SYNOPSIS
    GWObserver Upload Manager - wrapper for upload_to_r2.py with logging,
    Discord webhook alerts, scheduled task management, and status tracking.

.DESCRIPTION
    Replaces setup_scheduled_task.bat with a full-featured automation wrapper.

    Modes:
      -Run          Execute an upload with full logging (default)
      -Install      Create a Windows Scheduled Task for recurring uploads
      -Uninstall    Remove the scheduled task
      -Status       Show health dashboard and recent history
      -TestWebhook  Send a test message to the Discord webhook

.EXAMPLE
    .\Upload-Manager.ps1 -Run
    .\Upload-Manager.ps1 -Install
    .\Upload-Manager.ps1 -Status
    .\Upload-Manager.ps1 -TestWebhook
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Install')]
    [switch]$Install,

    [Parameter(ParameterSetName = 'Uninstall')]
    [switch]$Uninstall,

    [Parameter(ParameterSetName = 'Run')]
    [switch]$Run,

    [Parameter(ParameterSetName = 'Status')]
    [switch]$Status,

    [Parameter(ParameterSetName = 'TestWebhook')]
    [switch]$TestWebhook,

    [Parameter()]
    [string]$ConfigFile
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$TaskName = 'GWObserver-R2-Upload'

# ── Configuration ─────────────────────────────────────────────────────────────

function Read-EnvFile([string]$Path) {
    $config = @{}
    if (-not (Test-Path $Path)) { return $config }
    foreach ($line in Get-Content $Path -Encoding UTF8) {
        $line = $line.Trim()
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        $idx = $line.IndexOf('=')
        if ($idx -gt 0) {
            $key = $line.Substring(0, $idx).Trim()
            $val = $line.Substring($idx + 1).Trim()
            $config[$key] = $val
        }
    }
    return $config
}

function Read-ManagerConfig {
    $envPath = $(if ($ConfigFile) { $ConfigFile } else { Join-Path $ScriptDir 'upload_manager.env' })
    $cfg = Read-EnvFile $envPath

    # Apply defaults for optional keys
    if (-not $cfg['PYTHON_EXE'])            { $cfg['PYTHON_EXE'] = 'python' }
    if (-not $cfg['LOG_DIR'])               { $cfg['LOG_DIR'] = Join-Path $ScriptDir 'logs' }
    if (-not $cfg['LOG_RETENTION_DAYS'])     { $cfg['LOG_RETENTION_DAYS'] = '30' }
    if (-not $cfg['TASK_INTERVAL_MINUTES'])  { $cfg['TASK_INTERVAL_MINUTES'] = '60' }
    if (-not $cfg['NOTIFY_ON_SUCCESS'])      { $cfg['NOTIFY_ON_SUCCESS'] = 'true' }
    if (-not $cfg['STATUS_FILE_PATH'])       { $cfg['STATUS_FILE_PATH'] = Join-Path $ScriptDir 'upload_status.json' }
    return $cfg
}

# ── Logging ───────────────────────────────────────────────────────────────────

function Write-Log([string]$Message, [string]$Level = 'INFO') {
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $line = "[$ts] [$Level]  $Message"

    # Console output
    switch ($Level) {
        'ERROR' { Write-Host $line -ForegroundColor Red }
        'WARN'  { Write-Host $line -ForegroundColor Yellow }
        default { Write-Host $line }
    }

    # File output
    $logDir = $script:Config['LOG_DIR']
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
    $logFile = Join-Path $logDir ("upload_{0}.log" -f (Get-Date -Format 'yyyy-MM-dd'))
    Add-Content -Path $logFile -Value $line -Encoding UTF8
}

function Invoke-LogRotation {
    $logDir = $script:Config['LOG_DIR']
    if (-not (Test-Path $logDir)) { return }
    $retention = [int]$script:Config['LOG_RETENTION_DAYS']
    $cutoff = (Get-Date).AddDays(-$retention)
    $old = Get-ChildItem -Path $logDir -Filter 'upload_*.log' | Where-Object { $_.LastWriteTime -lt $cutoff }
    if ($old) {
        $old | Remove-Item -Force
        Write-Log "Purged $($old.Count) log file(s) older than $retention days"
    }
}

# ── Status File ───────────────────────────────────────────────────────────────

function New-DefaultStatus {
    return @{
        schema_version      = 1
        last_run            = $null
        last_run_status     = $null
        last_success        = $null
        consecutive_failures = 0
        total_uploads       = 0
        total_runs          = 0
        r2_storage_bytes    = 0
        r2_object_count     = 0
        last_error          = $null
        history             = @()
    }
}

function Read-StatusFile {
    $path = $script:Config['STATUS_FILE_PATH']
    if (Test-Path $path) {
        try {
            $raw = Get-Content $path -Raw -Encoding UTF8 | ConvertFrom-Json
            # Convert to hashtable for easier manipulation
            $ht = @{}
            foreach ($prop in $raw.PSObject.Properties) { $ht[$prop.Name] = $prop.Value }
            # Ensure history is a mutable list
            if ($ht['history'] -is [array]) { $ht['history'] = [System.Collections.ArrayList]@($ht['history']) }
            else { $ht['history'] = [System.Collections.ArrayList]@() }
            return $ht
        } catch {
            Write-Log "Warning: could not parse status file, resetting" 'WARN'
        }
    }
    $default = New-DefaultStatus
    $default['history'] = [System.Collections.ArrayList]@()
    return $default
}

function Write-StatusFile([hashtable]$StatusData) {
    $path = $script:Config['STATUS_FILE_PATH']
    # Trim history to 48 entries
    while ($StatusData['history'].Count -gt 48) {
        $StatusData['history'].RemoveAt(0)
    }
    $json = $StatusData | ConvertTo-Json -Depth 5
    # Atomic write: temp file + rename
    $tmp = "$path.tmp"
    Set-Content -Path $tmp -Value $json -Encoding UTF8
    Move-Item -Path $tmp -Destination $path -Force
}

# ── Discord Webhook ───────────────────────────────────────────────────────────

function Send-DiscordWebhook([hashtable]$Embed) {
    $url = $script:Config['DISCORD_WEBHOOK_URL']
    if (-not $url) {
        Write-Log "Discord webhook URL not configured, skipping notification" 'WARN'
        return
    }
    $json = @{ embeds = @($Embed) } | ConvertTo-Json -Depth 10
    # PowerShell 5.1 ConvertTo-Json escapes surrogate pairs as separate
    # \uD83D\uDD25 sequences instead of proper \uD83D\uDD25 pairs.
    # Fix by decoding all \uXXXX escapes into real UTF-8 characters,
    # then sending as raw UTF-8 bytes.
    $decoded = [regex]::Replace($json, '\\u([0-9A-Fa-f]{4})', {
        param($m) [char]([convert]::ToInt32($m.Groups[1].Value, 16))
    })
    $bodyBytes = [System.Text.Encoding]::UTF8.GetBytes($decoded)
    try {
        Invoke-RestMethod -Uri $url -Method Post -ContentType 'application/json; charset=utf-8' -Body $bodyBytes -UserAgent 'GWObserver-UploadManager' | Out-Null
        Write-Log "Discord notification sent"
    } catch {
        Write-Log "Failed to send Discord notification: $_" 'ERROR'
    }
}

function Format-Bytes([long]$Bytes) {
    if ($Bytes -ge 1GB) { return "{0:N2} GB" -f ($Bytes / 1GB) }
    if ($Bytes -ge 1MB) { return "{0:N1} MB" -f ($Bytes / 1MB) }
    return "{0:N0} KB" -f ($Bytes / 1KB)
}

# Emoji constants — literal UTF-8 characters for PowerShell 5.1 compatibility
$script:E_CLIPBOARD = '📋'
$script:E_FIRE      = '🔥'
$script:E_CLOCK     = '🕐'
$script:E_OUTBOX    = '📤'
$script:E_SKIP      = '⏭'
$script:E_PROHIBIT  = '🚫'
$script:E_WARN      = '⚠'
$script:E_CLOUD     = '☁'
$script:E_CROSS     = '❌'
$script:E_YELLOW    = '🟡'
$script:E_CHECK     = '✅'
$script:E_GEAR      = '⚙'
$script:E_TUBE      = '🧪'

function New-FailureEmbed([hashtable]$Report, [hashtable]$StatusData) {
    $errorText = ($Report['errors'] | ForEach-Object { $_.error }) -join "`n"
    if ($errorText.Length -gt 1000) { $errorText = $errorText.Substring(0, 997) + '...' }
    $streak = [int]$StatusData['consecutive_failures']
    $fields = @(
        @{ name = "$($script:E_CLIPBOARD) Error Details"; value = "``````$(if ($errorText) { $errorText } else { 'Unknown error' })``````"; inline = $false }
        @{ name = "$($script:E_FIRE) Streak"; value = "$streak consecutive failure$(if ($streak -ne 1) { 's' })"; inline = $true }
    )
    if ($StatusData['last_success']) {
        $fields += @{ name = "$($script:E_CLOCK) Last Success"; value = $StatusData['last_success']; inline = $true }
    }
    if ($Report['bucket_stats']) {
        $sizeStr = Format-Bytes $Report['bucket_stats']['total_size_bytes']
        $bucketObjs = $Report['bucket_stats']['total_objects']
        $fields += @{ name = "$($script:E_CLOUD) R2 Storage"; value = "$bucketObjs matches | $sizeStr"; inline = $false }
    }
    return @{
        title       = "$($script:E_CROSS) Upload Failed"
        description = "The upload pipeline encountered errors and could not complete."
        color       = 15158332  # red
        fields      = $fields
        timestamp   = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')
        footer      = @{ text = "$($script:E_GEAR) GWObserver Upload Manager" }
    }
}

function New-RecoveryEmbed([hashtable]$Report, [int]$PrevFailures) {
    $fields = @(
        @{ name = "$($script:E_OUTBOX) Uploaded"; value = "$($Report['uploaded']) match(es)"; inline = $true }
        @{ name = "$($script:E_SKIP) Skipped"; value = "$($Report['skipped'])"; inline = $true }
    )
    if ([int]$Report['rejected_corrupt'] -gt 0) {
        $fields += @{ name = "$($script:E_PROHIBIT) Rejected"; value = "$($Report['rejected_corrupt']) corrupt"; inline = $true }
    }
    if ($Report['bucket_stats']) {
        $sizeStr = Format-Bytes $Report['bucket_stats']['total_size_bytes']
        $bucketObjs = $Report['bucket_stats']['total_objects']
        $fields += @{ name = "$($script:E_CLOUD) R2 Storage"; value = "$bucketObjs matches | $sizeStr"; inline = $false }
    }
    return @{
        title       = "$($script:E_YELLOW) Upload Recovered"
        description = "Back online after **$PrevFailures** consecutive failure(s)."
        color       = 16776960  # yellow
        fields      = $fields
        timestamp   = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')
        footer      = @{ text = "$($script:E_GEAR) GWObserver Upload Manager" }
    }
}

function New-SuccessEmbed([hashtable]$Report) {
    $uploaded = [int]$Report['uploaded']
    $skipped = [int]$Report['skipped']
    $rejected = [int]$Report['rejected_corrupt']
    $fields = @(
        @{ name = "$($script:E_OUTBOX) Uploaded"; value = "$uploaded match$(if ($uploaded -ne 1) { 'es' })"; inline = $true }
        @{ name = "$($script:E_SKIP) Skipped"; value = "$skipped already synced"; inline = $true }
    )
    if ($rejected -gt 0) {
        $fields += @{ name = "$($script:E_PROHIBIT) Rejected"; value = "$rejected corrupt"; inline = $true }
    }
    if ($Report['warnings'] -and $Report['warnings'].Count -gt 0) {
        $warnText = ($Report['warnings'] | ForEach-Object { $_.warning }) -join "`n"
        if ($warnText.Length -gt 500) { $warnText = $warnText.Substring(0, 497) + '...' }
        $fields += @{ name = "$($script:E_WARN) Warnings"; value = $warnText; inline = $false }
    }
    if ($Report['bucket_stats']) {
        $sizeStr = Format-Bytes $Report['bucket_stats']['total_size_bytes']
        $bucketObjs = $Report['bucket_stats']['total_objects']
        $fields += @{ name = "$($script:E_CLOUD) R2 Storage"; value = "$bucketObjs matches | $sizeStr"; inline = $false }
    }
    return @{
        title       = "$($script:E_CHECK) Upload Complete"
        description = "Pipeline finished successfully."
        color       = 3066993  # green
        fields      = $fields
        timestamp   = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')
        footer      = @{ text = "$($script:E_GEAR) GWObserver Upload Manager" }
    }
}

# ── Mode: Run (default) ──────────────────────────────────────────────────────

function Invoke-Upload {
    Write-Log "========== Upload run starting =========="
    Invoke-LogRotation

    $pythonExe = $script:Config['PYTHON_EXE']
    $uploadScript = Join-Path $ScriptDir 'upload_to_r2.py'
    if (-not (Test-Path $uploadScript)) {
        Write-Log "upload_to_r2.py not found at $uploadScript" 'ERROR'
        return
    }

    # Temp file for JSON report
    $reportFile = Join-Path $env:TEMP "gwo_upload_report_$(Get-Date -Format 'yyyyMMdd_HHmmss').json"

    # Build command
    $pyArgs = @("`"$uploadScript`"", "--json-report", "`"$reportFile`"")
    Write-Log "Executing: $pythonExe $($pyArgs -join ' ')"

    # Run Python and capture output
    $env:PYTHONIOENCODING = 'utf-8'
    $process = Start-Process -FilePath $pythonExe -ArgumentList $pyArgs `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $env:TEMP 'gwo_stdout.tmp') `
        -RedirectStandardError (Join-Path $env:TEMP 'gwo_stderr.tmp')

    $exitCode = $process.ExitCode

    # Log stdout
    $stdoutFile = Join-Path $env:TEMP 'gwo_stdout.tmp'
    $stderrFile = Join-Path $env:TEMP 'gwo_stderr.tmp'
    if (Test-Path $stdoutFile) {
        foreach ($line in Get-Content $stdoutFile -Encoding UTF8) {
            if ($line.Trim()) { Write-Log "  $line" }
        }
        Remove-Item $stdoutFile -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $stderrFile) {
        foreach ($line in Get-Content $stderrFile -Encoding UTF8) {
            if ($line.Trim()) { Write-Log "  $line" 'ERROR' }
        }
        Remove-Item $stderrFile -Force -ErrorAction SilentlyContinue
    }

    Write-Log "Python exit code: $exitCode"

    # Parse JSON report
    $report = $null
    if (Test-Path $reportFile) {
        try {
            $raw = Get-Content $reportFile -Raw -Encoding UTF8 | ConvertFrom-Json
            $report = @{}
            foreach ($prop in $raw.PSObject.Properties) { $report[$prop.Name] = $prop.Value }
            # Convert nested objects to hashtables
            if ($report['bucket_stats'] -and $report['bucket_stats'] -isnot [hashtable]) {
                $bs = @{}
                foreach ($p in $report['bucket_stats'].PSObject.Properties) { $bs[$p.Name] = $p.Value }
                $report['bucket_stats'] = $bs
            }
            if ($report['errors'] -is [array]) {
                $report['errors'] = @($report['errors'] | ForEach-Object {
                    $h = @{}; foreach ($p in $_.PSObject.Properties) { $h[$p.Name] = $p.Value }; $h
                })
            }
        } catch {
            Write-Log "Failed to parse JSON report: $_" 'WARN'
        }
        Remove-Item $reportFile -Force -ErrorAction SilentlyContinue
    }

    # Build a fallback report if parsing failed
    if (-not $report) {
        $report = @{
            status         = $(if ($exitCode -eq 0) { 'success' } else { 'error' })
            uploaded       = 0
            skipped        = 0
            errors         = @(@{ match = ''; error = "Process exited with code $exitCode" })
            warnings       = @()
            bucket_stats   = $null
        }
    }

    # Log summary
    if ($report['bucket_stats']) {
        $sizeStr = Format-Bytes $report['bucket_stats']['total_size_bytes']
        Write-Log "Bucket: $($report['bucket_stats']['total_objects']) objects, $sizeStr"
    }
    Write-Log "Uploaded: $($report['uploaded']), Skipped: $($report['skipped']), Errors: $($report['errors'].Count)"

    # Update status file
    $status = Read-StatusFile
    $prevFailures = [int]$status['consecutive_failures']
    $now = Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ'

    $status['last_run'] = $now
    $status['last_run_status'] = $report['status']
    $status['total_runs'] = [int]$status['total_runs'] + 1

    if ($report['status'] -eq 'success') {
        $status['last_success'] = $now
        $status['consecutive_failures'] = 0
        $status['last_error'] = $null
    } else {
        $status['consecutive_failures'] = [int]$status['consecutive_failures'] + 1
        if ($report['errors'].Count -gt 0) {
            $status['last_error'] = $report['errors'][0]['error']
        }
    }

    $status['total_uploads'] = [int]$status['total_uploads'] + [int]$report['uploaded']

    if ($report['bucket_stats']) {
        $status['r2_storage_bytes'] = [long]$report['bucket_stats']['total_size_bytes']
        $status['r2_object_count'] = [int]$report['bucket_stats']['total_objects']
    }

    # Add to history
    $histEntry = @{ time = $now; status = $report['status']; uploaded = [int]$report['uploaded'] }
    if ($report['status'] -eq 'error' -and $report['errors'].Count -gt 0) {
        $histEntry['error'] = $report['errors'][0]['error']
    }
    $status['history'].Add($histEntry) | Out-Null

    Write-StatusFile $status
    Write-Log "Status file updated"

    # Discord notifications
    if ($report['status'] -eq 'error') {
        Write-Log "Sending failure notification" 'WARN'
        Send-DiscordWebhook (New-FailureEmbed $report $status)
    } elseif ($prevFailures -gt 0) {
        Write-Log "Sending recovery notification"
        Send-DiscordWebhook (New-RecoveryEmbed $report $prevFailures)
    } elseif ($script:Config['NOTIFY_ON_SUCCESS'] -eq 'true' -and [int]$report['uploaded'] -gt 0) {
        Write-Log "Sending success notification"
        Send-DiscordWebhook (New-SuccessEmbed $report)
    }

    Write-Log "========== Upload run complete =========="
}

# ── Mode: Install ─────────────────────────────────────────────────────────────

function Install-UploadTask {
    # Interactive setup if config file doesn't exist
    $envPath = Join-Path $ScriptDir 'upload_manager.env'
    if (-not (Test-Path $envPath)) {
        Write-Host "First-time setup: creating upload_manager.env" -ForegroundColor Cyan
        Write-Host ""

        # Discord webhook
        Write-Host "Discord webhook URL for failure/recovery notifications."
        Write-Host "  Create one in Discord: Server Settings > Integrations > Webhooks"
        Write-Host "  Leave blank to skip (you can add it later in upload_manager.env)."
        Write-Host ""
        $webhook = Read-Host "  Discord webhook URL"

        # Interval
        Write-Host ""
        $intervalInput = Read-Host "  Upload interval in minutes (default: 60)"
        if (-not $intervalInput) { $intervalInput = '60' }

        # Python path
        Write-Host ""
        $pythonInput = Read-Host "  Python executable (default: python)"
        if (-not $pythonInput) { $pythonInput = 'python' }

        # Write config
        $lines = @(
            "# Upload Manager configuration (auto-generated by -Install)"
            ""
            "DISCORD_WEBHOOK_URL=$webhook"
            "PYTHON_EXE=$pythonInput"
            "TASK_INTERVAL_MINUTES=$intervalInput"
            "LOG_RETENTION_DAYS=30"
            "NOTIFY_ON_SUCCESS=false"
        )
        $lines | Set-Content -Path $envPath -Encoding UTF8
        Write-Host ""
        Write-Host "  Config saved to: $envPath" -ForegroundColor Green
        Write-Host ""

        # Reload config with new values
        $script:Config = Read-ManagerConfig
    }

    # Check for elevation
    $isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host "Warning: not running as Administrator. Task creation may fail." -ForegroundColor Yellow
        Write-Host "Re-run this script as Administrator if it fails." -ForegroundColor Yellow
        Write-Host ""
    }

    $interval = [int]$script:Config['TASK_INTERVAL_MINUTES']
    $scriptPath = Join-Path $ScriptDir 'Upload-Manager.ps1'

    Write-Host "Creating scheduled task: $TaskName"
    Write-Host "  Script:   $scriptPath"
    Write-Host "  Interval: every $interval minutes"
    Write-Host ""

    $action = New-ScheduledTaskAction `
        -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" -Run" `
        -WorkingDirectory $ScriptDir

    $trigger = New-ScheduledTaskTrigger `
        -Once -At (Get-Date) `
        -RepetitionInterval (New-TimeSpan -Minutes $interval)

    $settings = New-ScheduledTaskSettingsSet `
        -StartWhenAvailable `
        -DontStopOnIdleEnd `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries

    try {
        Register-ScheduledTask `
            -TaskName $TaskName `
            -Action $action `
            -Trigger $trigger `
            -Settings $settings `
            -Force | Out-Null

        Write-Host "Task created successfully." -ForegroundColor Green
        Write-Host ""
        Write-Host "  Run now:    schtasks /run /tn `"$TaskName`""
        Write-Host "  View:       .\Upload-Manager.ps1 -Status"
        Write-Host "  Uninstall:  .\Upload-Manager.ps1 -Uninstall"
    } catch {
        Write-Host "Failed to create task: $_" -ForegroundColor Red
        Write-Host "Make sure you are running as Administrator." -ForegroundColor Yellow
    }
}

# ── Mode: Uninstall ───────────────────────────────────────────────────────────

function Uninstall-UploadTask {
    try {
        $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        if (-not $task) {
            Write-Host "Task '$TaskName' does not exist."
            return
        }
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Task '$TaskName' removed." -ForegroundColor Green
    } catch {
        Write-Host "Failed to remove task: $_" -ForegroundColor Red
    }
}

# ── Mode: Status ──────────────────────────────────────────────────────────────

function Show-UploadStatus {
    $status = Read-StatusFile

    Write-Host ""
    Write-Host "  GWObserver Upload Manager - Status" -ForegroundColor Cyan
    Write-Host "  ====================================" -ForegroundColor Cyan
    Write-Host ""

    # Last run
    if ($status['last_run']) {
        $color = $(if ($status['last_run_status'] -eq 'success') { 'Green' } else { 'Red' })
        Write-Host "  Last run:         $($status['last_run'])  [$($status['last_run_status'])]" -ForegroundColor $color
    } else {
        Write-Host "  Last run:         (never)" -ForegroundColor DarkGray
    }

    # Last success
    if ($status['last_success']) {
        Write-Host "  Last success:     $($status['last_success'])" -ForegroundColor Green
    } else {
        Write-Host "  Last success:     (never)" -ForegroundColor DarkGray
    }

    # Consecutive failures
    $failures = [int]$status['consecutive_failures']
    if ($failures -gt 0) {
        Write-Host "  Failures:         $failures consecutive" -ForegroundColor Red
        if ($status['last_error']) {
            Write-Host "  Last error:       $($status['last_error'])" -ForegroundColor Red
        }
    } else {
        Write-Host "  Failures:         0" -ForegroundColor Green
    }

    # Totals
    Write-Host ""
    Write-Host "  Total uploads:    $($status['total_uploads'])"
    Write-Host "  Total runs:       $($status['total_runs'])"

    # R2 storage
    if ([long]$status['r2_storage_bytes'] -gt 0) {
        $sizeStr = Format-Bytes ([long]$status['r2_storage_bytes'])
        $objCount = $status['r2_object_count']
        Write-Host ('  R2 storage:       ' + $sizeStr + ' (' + $objCount + ' objects)') -ForegroundColor Cyan
    }

    # Scheduled task
    Write-Host ""
    try {
        $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        if ($task) {
            $info = Get-ScheduledTaskInfo -TaskName $TaskName
            Write-Host "  Scheduled task:   $($task.State)" -ForegroundColor Green
            if ($info.LastRunTime) {
                Write-Host "  Task last ran:    $($info.LastRunTime)"
            }
            if ($info.NextRunTime) {
                Write-Host "  Task next run:    $($info.NextRunTime)"
            }
        } else {
            Write-Host "  Scheduled task:   Not installed" -ForegroundColor Yellow
        }
    } catch {
        Write-Host "  Scheduled task:   Could not query" -ForegroundColor DarkGray
    }

    # Recent history
    $hist = $status['history']
    if ($hist -and $hist.Count -gt 0) {
        Write-Host ""
        Write-Host "  Recent history (last 24 runs):" -ForegroundColor Cyan
        Write-Host "  -----------------------------------------------"
        $show = $(if ($hist.Count -gt 24) { $hist[($hist.Count - 24)..($hist.Count - 1)] } else { $hist })
        foreach ($entry in $show) {
            $time = $(if ($entry.time) { $entry.time } else { $entry['time'] })
            $st = $(if ($entry.status) { $entry.status } else { $entry['status'] })
            $up = $(if ($entry.uploaded -ne $null) { $entry.uploaded } else { $entry['uploaded'] })
            $icon = $(if ($st -eq 'success') { '+' } else { 'X' })
            $color = $(if ($st -eq 'success') { 'Green' } else { 'Red' })
            $line = "  [$icon] $time  uploaded: $up"
            if ($st -eq 'error') {
                $err = $(if ($entry.error) { $entry.error } else { $entry['error'] })
                if ($err) { $line += "  error: $err" }
            }
            Write-Host $line -ForegroundColor $color
        }
    }

    # Last log lines
    $logDir = $script:Config['LOG_DIR']
    if (Test-Path $logDir) {
        $latest = Get-ChildItem -Path $logDir -Filter 'upload_*.log' | Sort-Object Name -Descending | Select-Object -First 1
        if ($latest) {
            Write-Host ""
            Write-Host "  Latest log: $($latest.FullName)" -ForegroundColor Cyan
            Write-Host "  (last 10 lines):" -ForegroundColor Cyan
            Get-Content $latest.FullName -Tail 10 -Encoding UTF8 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
        }
    }

    Write-Host ""
}

# ── Mode: TestWebhook ─────────────────────────────────────────────────────────

function Test-UploadWebhook {
    if (-not $script:Config['DISCORD_WEBHOOK_URL']) {
        Write-Host "Error: DISCORD_WEBHOOK_URL not set in upload_manager.env" -ForegroundColor Red
        return
    }
    Write-Host "Sending test webhook..."
    $embed = @{
        title       = "$($script:E_TUBE) Webhook Test"
        description = "Connection successful. Notifications are working."
        color       = 3447003  # blue
        timestamp   = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')
        footer      = @{ text = "$($script:E_GEAR) GWObserver Upload Manager" }
    }
    Send-DiscordWebhook $embed
    Write-Host "Done." -ForegroundColor Green
}

# ── Main Dispatch ─────────────────────────────────────────────────────────────

$script:Config = Read-ManagerConfig

if ($Install) {
    Install-UploadTask
} elseif ($Uninstall) {
    Uninstall-UploadTask
} elseif ($Status) {
    Show-UploadStatus
} elseif ($TestWebhook) {
    Test-UploadWebhook
} else {
    # Default: Run
    Invoke-Upload
}
