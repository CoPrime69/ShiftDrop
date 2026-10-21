# Times two ffmpeg.exe builds side by side on the conversions people run most, with the app's own
# arguments (src/Media.cpp): a 1080p H.264 clip to MP4 (stream copy, and a full H.264 re-encode with
# the encoder the app would pick), to WebM (VP9 + Opus) and to MP3. Each run is done twice, the two
# builds taking turns, and the faster time is kept. Prints a table (and adds it to the GitHub job
# summary when there is one).
param(
    [Parameter(Mandatory = $true)][string]$New,
    [Parameter(Mandatory = $true)][string]$Old,
    [string]$NewName = 'new',
    [string]$OldName = 'old',
    [int]$Seconds = 10
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path ([System.IO.Path]::GetTempPath()) "sd-ffmpeg-timing-$PID"
New-Item -ItemType Directory -Force $dir | Out-Null

function Run([string]$exe, [string[]]$a) {
    $p = Start-Process -FilePath $exe -ArgumentList ($a | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) `
        -NoNewWindow -Wait -PassThru -RedirectStandardError (Join-Path $dir 'err.txt') -RedirectStandardOutput (Join-Path $dir 'out.txt')
    return $p.ExitCode
}

try {
    $clip = Join-Path $dir 'clip.mp4'
    $code = Run $New @('-hide_banner', '-nostdin', '-y', '-f', 'lavfi', '-i', "testsrc2=s=1920x1080:r=30:d=$Seconds,noise=alls=6:allf=t",
        '-f', 'lavfi', '-i', "sine=f=330:sample_rate=48000:d=$Seconds", '-c:v', 'libopenh264', '-b:v', '8M', '-g', '60',
        '-c:a', 'aac', '-b:a', '160k', '-shortest', '-f', 'mp4', $clip)
    if ($code) { throw "could not make the test clip: $(Get-Content (Join-Path $dir 'err.txt') -Raw)" }

    # The H.264 encoder the app would pick (Ffmpeg.cpp, FfmpegEngine::H264), per build.
    function H264([string]$exe) {
        foreach ($e in 'h264_nvenc', 'h264_qsv', 'h264_amf', 'h264_mf') {
            $fmt = if ($e -eq 'h264_nvenc') { 'yuv420p' } else { 'nv12' }
            if ((Run $exe @('-hide_banner', '-nostdin', '-f', 'lavfi', '-i', 'color=c=black:s=320x240:r=30:d=0.3', '-c:v', $e, '-pix_fmt', $fmt, '-f', 'null', '-')) -eq 0) { return $e }
        }
        return 'libopenh264'
    }
    function H264Args([string]$e) {
        switch ($e) {
            'h264_nvenc' { @('-c:v', $e, '-preset', 'p4', '-rc', 'vbr', '-cq', '23', '-b:v', '0', '-pix_fmt', 'yuv420p') }
            'h264_qsv' { @('-c:v', $e, '-preset', 'fast', '-global_quality', '23', '-pix_fmt', 'nv12') }
            'h264_amf' { @('-c:v', $e, '-quality', 'speed', '-rc', 'cqp', '-qp_i', '21', '-qp_p', '23', '-pix_fmt', 'nv12') }
            'h264_mf' { @('-c:v', $e, '-rate_control', 'u_vbr', '-b:v', '8000000', '-pix_fmt', 'nv12') }
            default { @('-c:v', 'libopenh264', '-rc_mode', 'bitrate', '-b:v', '8000000', '-pix_fmt', 'yuv420p') }
        }
    }
    $builds = @(
        @{ Name = $NewName; Exe = $New; H264 = (H264 $New) },
        @{ Name = $OldName; Exe = $Old; H264 = (H264 $Old) }
    )
    $jobs = [ordered]@{
        'MP4 (stream copy)' = { param($b) @('-map', '0:v:0?', '-map', '0:a:0?', '-c:v', 'copy', '-c:a', 'copy', '-movflags', '+faststart', '-f', 'mp4') }
        'MP4 (H.264 re-encode)' = { param($b) @('-map', '0:v:0?', '-map', '0:a:0?', '-vf', 'crop=trunc(iw/2)*2:trunc(ih/2)*2') + (H264Args $b.H264) + @('-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', '-f', 'mp4') }
        'WebM (VP9 + Opus)' = { param($b) @('-map', '0:v:0?', '-map', '0:a:0?', '-c:v', 'libvpx-vp9', '-crf', '32', '-b:v', '0', '-row-mt', '1', '-deadline', 'realtime', '-cpu-used', '6', '-c:a', 'libopus', '-b:a', '128k', '-f', 'webm') }
        'MP3' = { param($b) @('-map', '0:a:0', '-vn', '-c:a', 'libmp3lame', '-q:a', '2', '-f', 'mp3') }
        'Decode only' = { param($b) @('-map', '0', '-f', 'null') }
    }
    $rows = @()
    foreach ($job in $jobs.Keys) {
        $best = @{}
        foreach ($round in 1..2) {
            foreach ($b in $builds) {
                $out = if ($job -eq 'Decode only') { '-' } else { Join-Path $dir 'out.bin' }
                $a = @('-hide_banner', '-nostdin', '-protocol_whitelist', 'file', '-i', $clip) + (& $jobs[$job] $b) + @('-y', $out)
                $t = [System.Diagnostics.Stopwatch]::StartNew()
                $code = Run $b.Exe $a
                $ms = $t.Elapsed.TotalMilliseconds
                if ($code) { throw "$($b.Name) failed on '$job': $(Get-Content (Join-Path $dir 'err.txt') -Raw)" }
                if (-not $best.ContainsKey($b.Name) -or $ms -lt $best[$b.Name]) { $best[$b.Name] = $ms }
            }
        }
        $label = if ($job -eq 'MP4 (H.264 re-encode)') { "$job, $($builds[0].H264) / $($builds[1].H264)" } else { $job }
        $rows += [pscustomobject]@{ Job = $label; New = $best[$NewName]; Old = $best[$OldName] }
    }
    $table = @("| ${Seconds} s 1080p H.264 clip to | $NewName | $OldName | $NewName vs $OldName |", '|---|---:|---:|---:|')
    foreach ($r in $rows) { $table += ('| {0} | {1:N2} s | {2:N2} s | {3:+0;-0;0}% |' -f $r.Job, ($r.New / 1000), ($r.Old / 1000), (($r.New - $r.Old) / $r.Old * 100)) }
    $sizes = ('ffmpeg.exe size: {0} {1:N1} MB, {2} {3:N1} MB' -f $NewName, ((Get-Item $New).Length / 1MB), $OldName, ((Get-Item $Old).Length / 1MB))
    $table | ForEach-Object { Write-Host $_ }
    Write-Host $sizes
    if ($env:GITHUB_STEP_SUMMARY) { (@('### ffmpeg timing', '') + $table + @('', $sizes)) | Add-Content $env:GITHUB_STEP_SUMMARY }
}
finally {
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
}
