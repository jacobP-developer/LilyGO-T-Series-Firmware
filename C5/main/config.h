// main/config.h
#pragma once

// --- Input ---
#define PIN_NUM_BOOT_BUTTON 28

// --- SD Card (SPI mode on shared LCD bus, T-Dongle-C5) ---
// The C5 in ESP-IDF 5.5.5 does not expose SDMMC_HOST_DEFAULT; SDSPI works
// on the same pins via the already-initialised SPI2 bus (MISO=7, MOSI=2, SCK=6).
#define PIN_SD_CLK   6
#define PIN_SD_CMD   2
#define PIN_SD_D0    7
#define PIN_SD_CS    23
#define SD_MOUNT_POINT "/sdcard"

// --- USB Protocol ---
#define MAX_PACKET_LEN 1024