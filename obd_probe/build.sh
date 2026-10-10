#!/bin/bash
# Builds the firmware for one board. Usage: ./build.sh s3 | esp32 | c3
# Output: build-<board>/ (merged image for first USB flash, .bin for OTA) and elf/<version>-<board>.elf
# (keep the .elf of anything you flash: crash backtraces from /api/crash decode against it).
set -e
cd "$(dirname "$0")"
case "$1" in
  s3)    FQBN=esp32:esp32:esp32s3:PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB ;;  # Waveshare ESP32-S3-Touch-LCD-4.3
  esp32) FQBN=esp32:esp32:esp32:PartitionScheme=min_spiffs ;;                           # original ESP32 (Classic SPP + BLE)
  c3)    FQBN=esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs ;;            # ESP32-C3 (BLE only)
  *) echo "usage: $0 s3|esp32|c3"; exit 1 ;;
esac
if [ ! -f secrets.h ]; then
  cp secrets.example.h secrets.h
  echo "Created secrets.h from secrets.example.h - put your WiFi details in it, then build again."
  exit 1
fi
FLAGS="-DLV_CONF_INCLUDE_SIMPLE -I$PWD"
arduino-cli compile -b "$FQBN" --output-dir "build-$1" \
  --build-property "compiler.c.extra_flags=$FLAGS" --build-property "compiler.cpp.extra_flags=$FLAGS" .
VER=$(grep -m1 'define FW_VERSION' obd_probe.ino | cut -d'"' -f2)
mkdir -p elf && cp "build-$1/obd_probe.ino.elf" "elf/obd_probe-$VER-$1.elf" && cp "build-$1/obd_probe.ino.bin" "elf/obd_probe-$VER-$1.bin"
