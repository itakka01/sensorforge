: <<'WINDOWS_BATCH'
@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Exapan Archive Converter
set "SF_SCRIPT=%~f0"
where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Windows PowerShell is unavailable or not on PATH.
  echo          Enable Windows PowerShell 5.1 and try again.
  pause
  exit /b 10
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $content=[IO.File]::ReadAllText($env:SF_SCRIPT); $part=[regex]::Match($content,'(?ms)^# POWERSHELL_BEGIN\r?\n(.*?)^# POWERSHELL_END\s*$'); if(-not $part.Success){throw 'Embedded PowerShell section is missing.'}; & ([scriptblock]::Create($part.Groups[1].Value)); if($null -eq $global:SFExitCode){exit 99}else{exit $global:SFExitCode} } catch { [Console]::Error.WriteLine('[ERROR] '+$_.Exception.Message); exit 99 }"
set "SF_EXIT=%errorlevel%"
echo.
if not "%SF_EXIT%"=="0" echo [NOTICE] Conversion did not finish successfully. Exit code: %SF_EXIT%
pause
exit /b %SF_EXIT%
WINDOWS_BATCH
#!/usr/bin/env bash
# Exapan archive converter | Ubuntu + Windows in ONE .cmd file
# Ubuntu: bash exapan_archive_convert.cmd
# Windows: double-click exapan_archive_convert.cmd
# Version marker read by the SensorForge firmware (day ZIP). Increase on every change,
# then run tools/update_archive_tools_manifest.sh before building the firmware.
# EXAPAN_CONVERTER_VERSION=1
# !!! NACH JEDER AENDERUNG DIESER DATEI: Versionsnummer oben erhoehen UND im Projekt
# !!!   tools/update_archive_tools_manifest.sh
# !!! ausfuehren (aktualisiert archive_tools_manifest.h), danach Firmware neu bauen.
# !!! Sonst legt die Firmware diese Datei NICHT ins Tagesarchiv (Pruefsumme unbekannt).
set -Eeuo pipefail
export LC_ALL=C
TZ_NAME="${TZ_NAME:-Europe/Vienna}"
VIEW_FPS="${VIEW_FPS:-4}"
MJPEG_QUALITY="${MJPEG_QUALITY:-3}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
DEST="$ROOT/converted"
OK=0; SKIP=0; FAIL=0
info() { printf '%s\n' "$*"; }
problem() { printf '[ERROR] %s\n' "$*" >&2; }
ready=1
info '============================================================'
info '  EXAPAN      |  MKV -> AVI  |  Linux'
info '============================================================'
info "Source directory: $ROOT"
info "Output directory: $DEST"
info '--- Dependency check ---'
for c in ffmpeg ffprobe awk date mktemp mv mkdir rm grep; do
  if command -v "$c" >/dev/null 2>&1; then
    info "[OK]     $c"
  else
    problem "Required tool '$c' was not found."
    ready=0
  fi
done
if (( ! ready )); then
  info 'Install on Ubuntu/Debian: sudo apt update && sudo apt install ffmpeg gawk coreutils tzdata'
  exit 10
fi
if ! ffmpeg -hide_banner -filters 2>/dev/null | grep -E '[[:space:]]subtitles[[:space:]]' >/dev/null; then
  problem 'FFmpeg does not support subtitles/libass. Install an FFmpeg build with libass.'
  exit 10
fi
if ! [[ "$VIEW_FPS" =~ ^[0-9]+([.][0-9]+)?$ ]] || [[ "$VIEW_FPS" == 0 ]]; then
  problem "Invalid VIEW_FPS: $VIEW_FPS"; exit 10
fi
if ! [[ "$MJPEG_QUALITY" =~ ^[2-9]$|^[12][0-9]$|^3[01]$ ]]; then
  problem "Invalid MJPEG_QUALITY (expected 2..31): $MJPEG_QUALITY"; exit 10
fi
if [[ ! -f "/usr/share/zoneinfo/$TZ_NAME" ]] || [[ "$TZ_NAME" == *'..'* ]]; then
  problem "Time zone '$TZ_NAME' is missing or invalid. Install tzdata: sudo apt install tzdata"; exit 10
fi
if ! TZ="$TZ_NAME" date -d '@0' '+%Y' >/dev/null 2>&1; then
  problem 'GNU date is required for timestamp calculation (Ubuntu package: coreutils).'; exit 10
fi
info '[OK]     FFmpeg subtitles/libass'
if ffmpeg -hide_banner -h full 2>/dev/null | grep -- '-fps_mode' >/dev/null; then
  SYNC_ARGS=(-fps_mode cfr)
else
  SYNC_ARGS=(-vsync cfr)
  info '[INFO]   Older FFmpeg detected: using -vsync cfr'
fi
info "[OK]     Time zone $TZ_NAME"
info "[OK]     AVI: $VIEW_FPS FPS | MJPEG Q=$MJPEG_QUALITY"
if ! mkdir -p -- "$DEST"; then problem "Cannot create output directory: $DEST"; exit 11; fi
# Previously converted AVI files in the delivery folder are consolidated.
shopt -s nullglob nocaseglob
for oldavi in "$ROOT"/*_timestamp.avi; do
  moved="$DEST/${oldavi##*/}"
  if [[ -e "$moved" ]]; then
    problem "Existing AVI not moved; destination file already exists: ${oldavi##*/}"
    ((FAIL+=1))
  elif ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 "$oldavi" 2>/dev/null | grep -q video; then
    if mv -n -- "$oldavi" "$moved"; then info "[ARCHIVE] Existing AVI moved: ${oldavi##*/}"; else problem "Could not move existing AVI: ${oldavi##*/}"; ((FAIL+=1)); fi
  else
    problem "Existing AVI is not readable: ${oldavi##*/}"
    ((FAIL+=1))
  fi
done
shopt -u nocaseglob
shopt -s nullglob nocaseglob
files=("$ROOT"/*.mkv)
shopt -u nocaseglob
if ((${#files[@]} == 0)); then info '[INFO] No MKV files found beside the script.'; exit 0; fi
for source in "${files[@]}"; do
  name="${source##*/}"; stem="${name%.*}"
  target="$DEST/${stem}_timestamp.avi"
  archived="$DEST/$name"
  info '------------------------------------------------------------'
  info "[FILE] $name"
  if [[ -e "$archived" ]]; then problem "Archive filename already exists: $archived. Source file remains unchanged."; ((FAIL+=1)); continue; fi
  if [[ -e "$target" ]]; then
    if ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 "$target" 2>/dev/null | grep -q video; then
      if mv -n -- "$source" "$archived"; then info "[SKIP] AVI already exists; source MKV archived."; ((SKIP+=1)); else problem 'Could not archive the source MKV.'; ((FAIL+=1)); fi
    else problem 'Existing AVI is not readable. Manual inspection required.'; ((FAIL+=1)); fi
    continue
  fi
  if ! tmpdir="$(mktemp -d)"; then problem 'Could not create temporary directory.'; ((FAIL+=1)); continue; fi
  ptsfile="$tmpdir/pts.txt"; srt="$tmpdir/time.srt"; outpart="$DEST/.${stem}_timestamp.partial.avi"
  rm -f -- "$outpart"
  creation="$(ffprobe -v error -show_entries format_tags=creation_time -of default=noprint_wrappers=1:nokey=1 "$source" 2>/dev/null | head -n 1 || true)"
  if [[ -z "$creation" ]]; then problem 'MKV does not contain Matroska DateUTC/creation_time metadata.'; rm -rf -- "$tmpdir"; ((FAIL+=1)); continue; fi
  if ! start="$(date -u -d "$creation" '+%s.%N' 2>/dev/null)"; then problem "Invalid capture start time: $creation"; rm -rf -- "$tmpdir"; ((FAIL+=1)); continue; fi
  if ! ffprobe -v error -select_streams v:0 -show_frames -show_entries frame=best_effort_timestamp_time -of csv=p=0 "$source" > "$ptsfile"; then
    problem 'ffprobe could not read frames (file may be encrypted or damaged).'; rm -rf -- "$tmpdir"; ((FAIL+=1)); continue
  fi
  if ! TZ="$TZ_NAME" awk -v start="$start" '
    function stamp(t,    ms,h,m,s,rem){ms=int(t*1000+0.5); h=int(ms/3600000); rem=ms%3600000; m=int(rem/60000); rem%=60000; s=int(rem/1000); return sprintf("%02d:%02d:%02d,%03d",h,m,s,rem%1000)}
    function emit(i,p,e,    absolute,sec,tenth,txt){absolute=start+p; sec=int(absolute); tenth=int((absolute-sec)*10+0.000001); if(tenth>9)tenth=9; txt=strftime("%Y-%m-%d %H:%M:%S",sec) "." tenth; print i; print stamp(p) " --> " stamp(e); print txt "\n"}
    /^[0-9]+([.][0-9]+)?$/ {p=$1+0; if(n>0){if(p<prior){bad=1; exit}; e=p-0.001; if(e<=prior)e=prior+0.100; emit(n,prior,e)}; n++; prior=p}
    END {if(bad)exit 7; if(n==0)exit 6; emit(n,prior,prior+1.0)}
  ' "$ptsfile" > "$srt"; then
    problem 'Missing or non-monotonic frame timestamps.'; rm -rf -- "$tmpdir"; ((FAIL+=1)); continue
  fi
  # FFmpeg filename escaping for the subtitles filter (colons, slashes, quotes)
  escaped="${srt//\\/\\\\}"; escaped="${escaped//:/\\:}"; escaped="${escaped//\'/\\\'}"; escaped="${escaped//,/\\,}"; escaped="${escaped//\[/\\[}"; escaped="${escaped//\]/\\]}"
  vf="subtitles=filename='${escaped}':force_style='Alignment=3,MarginR=18,MarginV=18,FontSize=22,Outline=2,Shadow=0',setpts=N/(${VIEW_FPS}*TB)"
  if ffmpeg -hide_banner -loglevel warning -stats -nostdin -y -i "$source" -an -vf "$vf" -r "$VIEW_FPS" "${SYNC_ARGS[@]}" -c:v mjpeg -q:v "$MJPEG_QUALITY" -pix_fmt yuvj420p -metadata comment='Exapan; real capture timestamps' "$outpart"; then
    if ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 "$outpart" 2>/dev/null | grep -q video && mv -n -- "$outpart" "$target"; then
      if mv -n -- "$source" "$archived"; then info "[OK]    AVI created; original MKV archived."; ((OK+=1)); else problem "AVI created, but could not archive source MKV: $name"; ((FAIL+=1)); fi
    else problem 'Output AVI is unreadable or the destination already exists.'; rm -f -- "$outpart"; ((FAIL+=1)); fi
  else problem 'FFmpeg conversion failed. Source MKV is unchanged.'; rm -f -- "$outpart"; ((FAIL+=1)); fi
  rm -rf -- "$tmpdir"
done
info '============================================================'
info "SUMMARY: $OK converted | $SKIP skipped | $FAIL errors"
info "Output and MKV archive: $DEST"
((FAIL==0)) || exit 2
exit 0

: <<'POWERSHELL_BLOCK'
# POWERSHELL_BEGIN
$global:SFExitCode=0
$ErrorActionPreference = 'Stop'
function Say([string]$Text) { Write-Host $Text }
function Fail([string]$Text) { Write-Host "[ERROR] $Text" -ForegroundColor Red }
function CmdExists([string]$Name) { return [bool](Get-Command $Name -ErrorAction SilentlyContinue) }
function SrtTime([double]$Seconds) {
    $ms=[long][Math]::Floor($Seconds*1000+0.5)
    if($ms -lt 0){$ms=0}
    $h=[long][Math]::Floor($ms/3600000); $r=$ms%3600000
    return ('{0:00}:{1:00}:{2:00},{3:000}' -f $h,[long][Math]::Floor($r/60000),[long][Math]::Floor(($r%60000)/1000),($r%1000))
}
try {
    $root=Split-Path -Parent $env:SF_SCRIPT
    $dest=Join-Path $root 'converted'
    $fps=4.0; $quality=3
    if($env:VIEW_FPS){$fps=[double]::Parse($env:VIEW_FPS,[Globalization.CultureInfo]::InvariantCulture)}
    if($env:MJPEG_QUALITY){$quality=[int]$env:MJPEG_QUALITY}
    Say '============================================================'
    Say '  EXAPAN      |  MKV -> AVI  |  Windows'
    Say '============================================================'
    Say "Source directory: $root"
    Say "Output directory: $dest"
    Say '--- Dependency check ---'
    $ready=$true
    foreach($cmd in @('ffmpeg.exe','ffprobe.exe')) {
        if(CmdExists $cmd){Say "[OK]     $cmd"}else{Fail "'$cmd' was not found on PATH."; $ready=$false}
    }
    if(-not $ready){ Say 'Installation: winget install --id Gyan.FFmpeg -e'; Say 'Restart the terminal or sign out and back in after installation.'; $global:SFExitCode=10; return }
    $filters=(& ffmpeg -hide_banner -filters 2>$null | Out-String)
    if($filters -notmatch '\s+subtitles\s+') {Fail 'FFmpeg lacks the subtitles/libass filter. Install a full FFmpeg build.';$global:SFExitCode=10;return}
    Say '[OK]     FFmpeg subtitles/libass'
    $ffhelp=(& ffmpeg -hide_banner -h full 2>$null | Out-String)
    $syncArgs=if($ffhelp -match '\-fps_mode'){ @('-fps_mode','cfr') }else{ @('-vsync','cfr') }
    if($fps -le 0 -or $quality -lt 2 -or $quality -gt 31){Fail 'Invalid options: VIEW_FPS must be positive and MJPEG_QUALITY must be 2..31.';$global:SFExitCode=10;return}
    $tzName = if($env:TZ_NAME){$env:TZ_NAME}else{'Europe/Vienna'}
    try {$tz=[TimeZoneInfo]::FindSystemTimeZoneById($tzName)} catch {try {$tz=[TimeZoneInfo]::FindSystemTimeZoneById('W. Europe Standard Time')}catch{Fail 'Time zone Europe/Vienna/W. Europe Standard Time is unavailable.';$global:SFExitCode=10;return}}
    Say "[OK]     Time zone $($tz.Id)"
    Say "[OK]     AVI: $fps FPS | MJPEG Q=$quality"
    [IO.Directory]::CreateDirectory($dest) | Out-Null
    foreach($old in @(Get-ChildItem -LiteralPath $root -File | Where-Object {$_.Name -ilike '*_timestamp.avi'})) {
        $moved=Join-Path $dest $old.Name
        if([IO.File]::Exists($moved)){Fail "Existing AVI not moved; destination file already exists: $($old.Name)"; $global:SFExitCode=2;continue}
        $v=(& ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 $old.FullName 2>$null | Out-String)
        if($v -notmatch 'video'){Fail "Existing AVI is not readable: $($old.Name)";$global:SFExitCode=2;continue}
        try{[IO.File]::Move($old.FullName,$moved);Say "[ARCHIVE] Existing AVI moved: $($old.Name)"}catch{Fail "Could not move existing AVI: $($old.Name)";$global:SFExitCode=2}
    }
    $files=@(Get-ChildItem -LiteralPath $root -File | Where-Object {$_.Extension -ieq '.mkv'})
    if($files.Count -eq 0){Say '[INFO] No MKV files found beside the script.';return}
    $ok=0;$skipped=0;$failed=0
    foreach($file in $files){
        $src=$file.FullName; $stem=[IO.Path]::GetFileNameWithoutExtension($file.Name)
        $target=Join-Path $dest ($stem+'_timestamp.avi')
        $archive=Join-Path $dest $file.Name
        Say '------------------------------------------------------------'; Say "[FILE] $($file.Name)"
        if([IO.File]::Exists($archive)){Fail "Archive filename already exists: $archive. Source file remains unchanged.";$failed++;continue}
        if([IO.File]::Exists($target)){
            $verified=(& ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 $target 2>$null | Out-String)
            if($verified -match 'video'){
                try{[IO.File]::Move($src,$archive);Say '[SKIP] AVI already exists; source MKV archived.';$skipped++}catch{Fail "Could not archive source MKV: $_";$failed++}
            }else{Fail 'Existing AVI is not readable. Manual inspection required.';$failed++}
            continue
        }
        $temp=Join-Path ([IO.Path]::GetTempPath()) ('exapan_'+[guid]::NewGuid().ToString('N'))
        $partial=Join-Path $dest ('.'+$stem+'_timestamp.partial.avi')
        try {
            [IO.Directory]::CreateDirectory($temp)|Out-Null
            $srt=Join-Path $temp 'timestamp.srt'
            $jsonText=(& ffprobe -v error -select_streams v:0 -show_frames -show_entries 'format_tags=creation_time:frame=best_effort_timestamp_time,pts_time,pkt_dts_time' -of json $src 2>$null | Out-String)
            if($LASTEXITCODE -ne 0){throw 'ffprobe could not read the MKV (it may be encrypted or damaged).'}
            $probe=$jsonText | ConvertFrom-Json
            $creation=$probe.format.tags.creation_time
            if(-not $creation){throw 'MKV does not contain Matroska DateUTC/creation_time metadata.'}
            $base=[DateTimeOffset]::Parse($creation,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::AssumeUniversal)
            $frames=@($probe.frames)
            $pts=New-Object 'System.Collections.Generic.List[double]'
            foreach($f in $frames){
                foreach($key in @('best_effort_timestamp_time','pts_time','pkt_dts_time')){
                    $raw=$f.$key
                    if($null -ne $raw -and $raw -ne 'N/A'){
                        $num=0.0
                        if([double]::TryParse([string]$raw,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$num)){
                            if($num -ge 0){$pts.Add($num)};break
                        }
                    }
                }
            }
            if($pts.Count -eq 0){throw 'No video frame timestamps found.'}
            $enc=New-Object Text.UTF8Encoding($false)
            $writer=New-Object IO.StreamWriter($srt,$false,$enc)
            try {
                for($i=0;$i -lt $pts.Count;$i++){
                    $p=$pts[$i]
                    if($i -gt 0 -and $p -lt $pts[$i-1]){throw 'Frame timestamps are not monotonic.'}
                    $end=if($i+1 -lt $pts.Count){$pts[$i+1]-0.001}else{$p+1.0}
                    if($end -le $p){$end=$p+0.100}
                    $utc=$base.AddSeconds($p)
                    $wall=[TimeZoneInfo]::ConvertTime($utc,$tz)
                    $label=$wall.ToString('yyyy-MM-dd HH:mm:ss.f',[Globalization.CultureInfo]::InvariantCulture)
                    $writer.WriteLine($i+1); $writer.WriteLine((SrtTime $p)+' --> '+(SrtTime $end));$writer.WriteLine($label);$writer.WriteLine()
                }
            }finally{$writer.Dispose()}
            # FFmpeg filtergraph path: forward slash, escaped drive colon and single quote.
            $escaped=$srt.Replace('\','/').Replace(':','\:').Replace("'","\'").Replace('[','\[').Replace(']','\]').Replace(',','\,')
            $fpsText=$fps.ToString([Globalization.CultureInfo]::InvariantCulture)
            $vf="subtitles=filename='$escaped':force_style='Alignment=3,MarginR=18,MarginV=18,FontSize=22,Outline=2,Shadow=0',setpts=N/($fpsText*TB)"
            if([IO.File]::Exists($partial)){throw "Temporary output file already exists: $partial"}
            & ffmpeg -hide_banner -loglevel warning -stats -nostdin -n -i $src -an -vf $vf -r $fpsText @syncArgs -c:v mjpeg -q:v ([string]$quality) -pix_fmt yuvj420p -metadata 'comment=Exapan; real capture timestamps' $partial
            if($LASTEXITCODE -ne 0){throw 'FFmpeg conversion failed.'}
            $verified=(& ffprobe -v error -select_streams v:0 -show_entries stream=codec_type -of csv=p=0 $partial 2>$null | Out-String)
            if($verified -notmatch 'video'){throw 'Output AVI is unreadable.'}
            [IO.File]::Move($partial,$target)
            [IO.File]::Move($src,$archive)
            Say '[OK]    AVI created; original MKV archived.';$ok++
        }catch{Fail "$($_.Exception.Message) | Original remains in source directory unless already archived.";$failed++}
        finally{if([IO.Directory]::Exists($temp)){[IO.Directory]::Delete($temp,$true)}; if([IO.File]::Exists($partial)){[IO.File]::Delete($partial)}}
    }
    Say '============================================================';Say "SUMMARY: $ok converted | $skipped skipped | $failed errors";Say "Output and MKV archive: $dest"
    if($failed -gt 0){$global:SFExitCode=2}
} catch {Fail $_.Exception.Message;$global:SFExitCode=11}
# POWERSHELL_END
POWERSHELL_BLOCK
