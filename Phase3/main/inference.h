#ifndef INFERENCE_H
#define INFERENCE_H

#include <stddef.h>
#include <cstdint>

// Implementazione già esistente (es. modello TFLite Micro / altro).
// pcm_buffer: campioni audio int32_t (24-bit) registrati dal microfono
// total_samples: numero di campioni nel buffer
float inference_from_audio(int32_t* pcm_buffer, size_t total_samples);

#endif
