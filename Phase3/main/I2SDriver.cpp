#include "I2SDriver.h"
#include "esp_log.h"
#include "Logger.h"

static const char* TAG = "I2SDriver";

I2SDriver::I2SDriver(int bclk, int ws, int din, uint32_t sample_rate) : sample_rate(sample_rate) {

    // 1. Configurazione Canale
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK) {
        Logger::instance().error(TAG, "Error creating I2S channel");
        return;
    }

    // 2. Configurazione Bus
    i2s_std_config_t std_cfg = {
    .clk_cfg = {
        .sample_rate_hz = sample_rate,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    },
    // Il microfono richiede OSR fisso = 64 (BCLK = 64 x fs). Con lo slot a 24 bit
    // in modalità mono lo std driver genera solo 48 BCLK/WS, rompendo la sincronia
    // interna del microfono. A 32 bit si ottengono 2 slot x 32 bit = 64 BCLK/WS.
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
        .bclk = (gpio_num_t)bclk,
        .ws   = (gpio_num_t)ws,
        .dout = GPIO_NUM_NC,
        .din  = (gpio_num_t)din,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
    };
    
    // ATTENZIONE: verifica come è cablato il pin SEL sul microfono.
    // Il datasheet dice: se SEL=HIGH, il DATA pin viene guidato quando WS=HIGH.
    // Con la convenzione I2S standard, WS=HIGH corrisponde tipicamente al canale
    // destro. Se SEL è collegato a Vdd (come nello schema a singolo mic del
    // datasheet), il canale corretto da campionare è quasi certamente
    // I2S_STD_SLOT_RIGHT, non LEFT. Se SEL è a GND, usa LEFT.
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    // 3. Inizializzazione e avvio
    if (i2s_channel_init_std_mode(rx_handle, &std_cfg) == ESP_OK && 
        i2s_channel_enable(rx_handle) == ESP_OK) {
        initialized = true;
        Logger::instance().info(TAG, "I2S initialized correctly");
    } else {
        Logger::instance().error(TAG, "Error initializing std mode");
    }
}

I2SDriver::~I2SDriver() {
    if (rx_handle != NULL) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        Logger::instance().info(TAG, "I2S channel deleted");
    }
}

size_t I2SDriver::record(int32_t* buffer, float seconds) {
    if (!initialized) return 0;

    size_t total_samples_needed = (size_t)(sample_rate * seconds);
    size_t samples_read_so_far = 0;

    while (samples_read_so_far < total_samples_needed) {
        // Calcola quanti ne mancano
        size_t remaining = total_samples_needed - samples_read_so_far;
        // Leggi il minimo tra 512 e ciò che resta
        size_t chunk_to_read = (remaining < 512) ? remaining : 512;
        
        size_t bytes_read = 0;
        i2s_channel_read(rx_handle, &buffer[samples_read_so_far], 
                        chunk_to_read * sizeof(int32_t), &bytes_read, 100);
        
        samples_read_so_far += (bytes_read / sizeof(int32_t));
        
        // Protezione: se l'hardware non risponde, usciamo per evitare loop infiniti
        if (bytes_read == 0) break; 
    } 

    return samples_read_so_far;
}