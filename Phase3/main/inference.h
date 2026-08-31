#ifndef INFERENCE_H
#define INFERENCE_H

#include <stddef.h>
#include <cstdint>
#include "MelSpectrogram.h"

// FASE 1 - Preprocessing audio completo:
// normalizzazione -> filtro passa-alto -> trimming -> Mel-spectrogram.
// Va chiamata UNA SOLA VOLTA per ciclo di misura, indipendentemente da
// quanti modelli/varianti vuoi poi testare sullo stesso audio.
// In caso di errore ritorna un MelSpectrogram con data == nullptr (stessa
// convenzione già usata da calculate_mel_spectrogram in MelSpectrogram.cpp).
MelSpectrogram compute_spectrogram_from_audio(int32_t* pcm_buffer, size_t total_samples);

// FASE 2 - Quantizzazione (se richiesta) + inferenza + dequantizzazione,
// a partire da uno spettrogramma già calcolato in FASE 1.
// Può essere chiamata più volte sullo stesso MelSpectrogram, ad es. una
// volta con use_quantized_model=true e una con false, per confrontare le
// due varianti senza rifare il preprocessing.
// Ritorna -1.0f in caso di errore (inizializzazione interprete o inferenza).
float inference_from_spectrogram(const MelSpectrogram& spectrogram, bool use_quantized_model);

// Wrapper di comodo che esegue FASE 1 + FASE 2 in sequenza: comportamento
// equivalente alla vecchia inference_from_audio a singola variante.
//
// IMPORTANTE: se devi fare doppia inferenza (int8 + float32) sullo STESSO
// audio, NON chiamare questa funzione due volte -- raddoppieresti inutilmente
// filtro, trimming e calcolo dello spettrogramma. Usa invece:
//   MelSpectrogram spec = compute_spectrogram_from_audio(pcm_buffer, total_samples);
//   if (spec.data != nullptr) {
//       float pred_int8 = inference_from_spectrogram(spec, true);
//       float pred_f32  = inference_from_spectrogram(spec, false);
//       free_mel_spectrogram(spec);
//   }
float inference_from_audio(int32_t* pcm_buffer, size_t total_samples, bool use_quantized_model);

#endif
