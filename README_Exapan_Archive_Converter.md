# EXAPAN Archive Converter

**User Guide | Windows and Ubuntu/Linux**  
**Converter file:** `exapan_archive_convert.cmd`

## Overview

EXAPAN Archive Converter converts compatible EXAPAN/SensorForge sparse-frame Matroska video archives (`.mkv`) into compact Motion JPEG AVI files (`.avi`). Each stored frame is shown once, with its **original capture date and time burned into the picture**.

The converter works from one shared `.cmd` file on both Windows and Ubuntu. It does **not require Python** or a separate application installer for the converter itself.

> **Important:** FFmpeg and FFprobe must be installed and accessible. The converter verifies its dependencies before processing and provides installation guidance when components are missing. Software is **not installed automatically**.

## Quick Start

1. Place `exapan_archive_convert.cmd` in the same folder as your `.mkv` archives.
2. Install FFmpeg/FFprobe if the dependency check reports that they are missing (see below).
3. Start the converter:
   - **Windows:** Double-click `exapan_archive_convert.cmd`.
   - **Ubuntu/Linux:** Open a terminal in the archive folder and run:
     ```bash
     bash exapan_archive_convert.cmd
     ```
4. Follow the status messages in the console. The converter processes all `.mkv` files located **directly in its own folder** (not in subfolders).
5. Find successfully converted video files and archived original MKVs in the automatically created `converted` folder.

You can run the converter again after adding more MKV files.

## Prerequisites and Readiness Check

| Component | Windows | Ubuntu / Debian |
|---|---|---|
| Script environment | Windows Command Prompt + Windows PowerShell 5.1 | Bash |
| FFmpeg and FFprobe | Required; available through `PATH` | Required; available through `PATH` |
| FFmpeg `subtitles` filter (libass) | Required | Required |
| Time zone support | Windows time-zone database | `tzdata` with `Europe/Vienna` |
| Standard system tools | Built into Windows | GNU `date`, `awk`, `mktemp`, `mv`, `mkdir`, `rm`, `grep` |
| Python | **Not required** | **Not required** |

**Windows installation guidance:** Open PowerShell or Terminal and run:

```powershell
winget install --id Gyan.FFmpeg -e
```

If `winget` is unavailable, install a full FFmpeg build that includes FFprobe and libass, and add its `bin` folder to your Windows `PATH`. Restart the terminal afterward. The converter also requires Windows PowerShell 5.1, normally available on supported Windows desktop installations.

**Ubuntu / Debian installation guidance:**

```bash
sudo apt update
sudo apt install ffmpeg gawk coreutils tzdata
```

The converter checks the tools and the FFmpeg subtitle filter at start-up. If a required component is missing, it prints an **[ERROR]** message and stops without processing files.

## Output and Archive Layout

**Before:**

```text
EXAPAN_Archive/
├── exapan_archive_convert.cmd
├── capture_001.mkv
└── capture_002.mkv
```

**After a successful run:**

```text
EXAPAN_Archive/
├── exapan_archive_convert.cmd
└── converted/
    ├── capture_001.mkv
    ├── capture_001_timestamp.avi
    ├── capture_002.mkv
    └── capture_002_timestamp.avi
```

The `converted` folder is also used to collect readable, previously generated `*_timestamp.avi` files left beside the script.

**Original file handling:** A source MKV is moved to `converted` only after a valid AVI has been generated, or when the corresponding existing AVI passes the converter's basic video-stream check. On a conversion failure the source MKV is normally left in its original folder.

**Files are not intentionally overwritten.** Name collisions, unreadable output files and archive conflicts are reported as errors and require inspection. Do not delete original recordings until you have verified the result independently.

## Conversion Behavior

- **Video:** Motion JPEG encoded in an AVI container.
- **Playback:** Compact, constant-frame-rate output; default **4 frames per second**.
- **Timestamps:** Burned into each frame using the MKV creation time and the original frame presentation timestamps.
- **Time zone:** `Europe/Vienna` by default, including applicable daylight-saving time transitions.
- **Frame gaps:** Rejected or missing capture frames are not replaced with long still-image segments.
- **Audio:** Not included in the output AVI.
- **Processing scope:** Only `.mkv` files in the converter's folder; no recursive scanning.

**Compatibility note:** The source must be a readable MKV with Matroska `DateUTC` / `creation_time` metadata and usable, monotonic video frame timestamps. This converter is intended for EXAPAN/SensorForge sparse-frame recordings; other MKV files may not work correctly. Encrypted recordings must first be exported or decrypted using the appropriate EXAPAN/SensorForge workflow.

## Optional Advanced Settings

For technically experienced users, the converter supports these environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `TZ_NAME` | `Europe/Vienna` | Time zone for the displayed timestamp |
| `VIEW_FPS` | `4` | Output playback frame rate; must be positive |
| `MJPEG_QUALITY` | `3` | FFmpeg MJPEG quality setting; accepted range: 2–31 (lower is generally higher quality) |

**Ubuntu/Linux example:**

```bash
TZ_NAME=Europe/Vienna VIEW_FPS=6 MJPEG_QUALITY=3 bash exapan_archive_convert.cmd
```

**Windows Command Prompt example:**

```bat
set "VIEW_FPS=6"
exapan_archive_convert.cmd
```

These settings are optional. Default settings are recommended for normal use. The time-zone choice on Windows depends on the installed Windows time-zone database; a Windows Central European time-zone fallback is used if `Europe/Vienna` cannot be resolved.

## Status Messages and Error Handling

| Message | Meaning |
|---|---|
| `[OK]` | Required component verified or conversion completed successfully |
| `[INFO]` | Informational status message |
| `[FILE]` | Source file currently being processed |
| `[ARCHIVE]` | Existing AVI moved into the archive folder |
| `[SKIP]` | Valid output AVI already exists; source MKV archived without reconversion |
| `[ERROR]` | A problem prevented an operation; review the message |
| `SUMMARY` | End-of-run totals: converted, skipped and errors |

**Typical problems:**

- **FFmpeg or FFprobe not found:** Install FFmpeg, ensure both tools are on `PATH`, then restart the terminal.
- **Subtitles/libass filter missing:** Use a full FFmpeg distribution that supports the `subtitles` filter.
- **Missing capture time or frame timestamps:** The MKV is incompatible, damaged, encrypted or missing required metadata.
- **Target file already exists:** Inspect the files in `converted`; the converter does not overwrite them.
- **Permission denied / unable to move file:** Check folder permissions, free disk space and whether another program is using the files.
- **No MKV files found:** Place recordings in the same directory as the converter and run it again.

### Exit Codes

| Code | Meaning |
|---|---|
| `0` | Completed without reported conversion errors (also possible if no MKVs were found) |
| `2` | One or more files failed |
| `10` | Missing or invalid prerequisite/configuration |
| `11` | Output folder or unexpected system-level problem |
| `99` | Windows launcher / embedded PowerShell failure |

## Operational Notes

- Run the converter on **local, writable storage** with sufficient free space for AVI output and temporary files.
- Avoid running two instances against the same archive folder at the same time.
- The converter does not validate output visually. Open a sample AVI to confirm quality and timestamp accuracy before deleting or distributing any original material.
- Windows displays a console window and pauses at the end so messages remain visible. Ubuntu runs in the terminal.
- The same `.cmd` file is deliberately used on both platforms, but the **Ubuntu start command must explicitly invoke Bash**. Double-clicking the `.cmd` file is intended for Windows only.

## Support Checklist

If conversion fails, provide your support contact with the operating-system version, the displayed `[ERROR]` message, the installed FFmpeg version (`ffmpeg -version`), and whether the input MKV was exported from an encrypted archive.

---

**EXAPAN Archive Converter — User Guide**  
Keep this README together with `exapan_archive_convert.cmd` when distributing archives to end users.
