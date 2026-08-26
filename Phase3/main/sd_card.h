#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"

// Inizializza il bus SPI e monta la SD card in FAT su mount_point (es. "/sdcard").
esp_err_t inizializza_sd(int pin_mosi, int pin_miso, int pin_clk, int pin_cs,
                          const char* mount_point);

#endif
