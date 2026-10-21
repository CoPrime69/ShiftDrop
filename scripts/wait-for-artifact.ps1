# CI only: waits until another job of this workflow run has uploaded the artifact $Name, so a job can
# start (check out, set up) while the job it depends on is still running, instead of after it.
# Fails as soon as that job ends without the artifact, or after $Minutes. Needs GH_TOKEN with
# actions: read.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Job,
    [int]$Minutes = 45
)
$ErrorActionPreference = 'Stop'
$run = "repos/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"

function Uploaded {
    $n = gh api "$run/artifacts?name=$Name" --jq '.total_count'
    return ($LASTEXITCODE -eq 0 -and [int]$n -gt 0)
}

$start = Get-Date
# Every 4 s, and the job's state every 30 s or so: a cold build can take ten minutes, and the
# workflow token's API calls are rationed per hour.
for ($i = 0; ((Get-Date) - $start).TotalMinutes -lt $Minutes; $i++) {
    if (Uploaded) {
        Write-Host "$Name is there after $([int]((Get-Date) - $start).TotalSeconds) s"
        exit 0
    }
    if ($i % 8 -eq 7) {
        $state = gh api "$run/attempts/$env:GITHUB_RUN_ATTEMPT/jobs?per_page=100" --jq ".jobs[] | select(.name == `"$Job`") | .status"
        if ("$state" -eq 'completed' -and -not (Uploaded)) {
            throw "The $Job job ended without uploading $Name"
        }
    }
    Start-Sleep -Seconds 4
}
throw "No $Name after $Minutes minutes"
