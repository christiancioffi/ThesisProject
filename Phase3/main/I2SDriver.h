#ifndef I2S_DRIVER_H
#define I2S_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include "driver/i2s_std.h"

class I2SDriver {
public:
    // Costruttore che inizializza tutto
    I2SDriver(int bclk_pin, int ws_pin, int din_pin, uint32_t sample_rate);

    // Distruttore per pulire le risorse
    ~I2SDriver();

    // Controlla se l'hardware è pronto
    bool is_valid() const { return initialized; }

    // Registra 'seconds' secondi di audio in 'buffer' (deve essere già allocato,
    // grande almeno sample_rate * seconds elementi int32_t).
    // Ritorna il numero di campioni effettivamente letti.
    size_t record(int32_t* buffer, float seconds);

private:
    i2s_chan_handle_t rx_handle = NULL;
    bool initialized = false;
    uint32_t sample_rate;
};

#endif