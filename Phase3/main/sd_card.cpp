#include "sd_card.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char* LOG_TAG = "SDCard";

esp_err_t inizializza_sd(int pin_mosi, int pin_miso, int pin_clk, int pin_cs,
                          const char* mount_point) {
    ESP_LOGI(LOG_TAG, "Initializing SD...");

    // 1. Configurazione del File System
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card;

    // 2. Host tramite macro SDSPI (anziché quelle SDMMC generiche)
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_host_device_t spi_host = SPI2_HOST;
    host.slot = spi_host;

    // 3. Configurazione del Bus SPI
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = pin_mosi,
        .miso_io_num = pin_miso,
        .sclk_io_num = pin_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG, "Initialization of SPI bus failed (0x%x)", ret);
        return ret;
    }

    // 4. Configurazione dello slot con macro specifica SDSPI
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)pin_cs;
    slot_config.host_id = spi_host;

    // 5. Mount
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG, "SD mounting failed (0x%x).", ret);
        // Cleanup the bus if it fails to avoid the 0x103 error on the next quick start
        spi_bus_free(spi_host);
        return ret;
    }

    ESP_LOGI(LOG_TAG, "SD card mounted successfully!");
    return ESP_OK;
}
