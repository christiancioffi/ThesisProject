#ifndef CNN_RNN_H
#define CNN_RNN_H

#include <stdint.h>

// Allineamento a 16 byte richiesto dall'interprete di TensorFlow Lite Micro
// per un accesso efficiente ai pesi memorizzati nella memoria Flash dell'ESP32.
extern const unsigned char _model[];
extern const unsigned int _model_len;

#ifdef __cplusplus
extern "C" {
#endif


#ifdef __cplusplus
}
#endif

#endif // CNN_RNN_H