SensorForge - WiFi Firmware Update
==================================

Baseline used
-------------
- itakka_sensorcam(20260910-160532).ino
- webconfig(20260910-160531).cpp

Only these two source files are changed in this release.
No existing source line was removed; the changes are additive plus one extra
System-menu entry and one extra reboot-status branch.

User flow
---------
1. Open WebConfig -> System -> Firmware Update.
2. Select a compiled .bin file.
3. Press "UPLOAD & PRUEFEN".
4. The image is written to /firmware/.wifi_upload.part on SD.
5. The existing SD-updater validator checks:
   - inactive OTA partition exists
   - image size fits
   - ESP32 application magic byte
   - SensorForge board compatibility marker
6. On success it is renamed to /firmware/.wifi_upload.ready.
   This filename does NOT end in .bin, so an accidental reboot does not install it.
7. Press "JETZT INSTALLIEREN".
8. The ready file is renamed to /firmware/wifi_update.bin.
9. New recording starts are blocked and a controlled reboot is scheduled.
10. On boot, the existing SD auto-updater validates the image again and performs
    the already established OTA write to the inactive partition.

Safety behavior
---------------
- Upload is rejected while a recording is active.
- Upload is rejected while another SD/storage operation owns the storage lock.
- Upload is rejected if another .bin candidate already exists in /firmware.
- An interrupted upload never becomes a .bin candidate.
- A validated upload is not installed until explicit confirmation.
- Install/discard actions use a per-boot action token in addition to the existing
  WebConfig authentication middleware.
- Multipart upload performs an explicit authentication check before any SD write.
- The existing SD auto-update flash code is unchanged.

Files
-----
- itakka_sensorcam.ino             complete updated main source
- webconfig.cpp                    complete updated WebConfig source
- itakka_sensorcam_wifi_update.diff exact diff against uploaded main baseline
- webconfig_wifi_update.diff        exact diff against uploaded WebConfig baseline
- BASELINE_SHA256.txt               SHA256 hashes of uploaded baseline files

Important
---------
The final installed-source metadata continues to be written by the existing SD
updater. It will therefore show something like:
  SD auto-update: wifi_update.bin
This accurately indicates that WiFi staged the file and the established SD
updater performed the actual OTA installation.

Remote-update caveat
--------------------
The updater can reject corrupt/wrong-board images, but it cannot know whether a
technically valid new firmware contains an application bug that prevents WiFi
from starting. For a device without physical access, install only builds that
were first tested on a second XIAO ESP32S3 Sense.
