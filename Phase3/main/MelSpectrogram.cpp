#include "MelSpectrogram.h"
#include "esp_dsp.h"
#include "hann_window.h"
#include "mel_matrix.h"
#include <math.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "Logger.h"
#include <algorithm>


void window_hann(const float* frame_in, float* frame_out, int frame_size) {
    for (int i = 0; i < frame_size; i++) {
        frame_out[i] = frame_in[i] * hann_window[i];
    }
}

int calculate_stft_frame(float* fft_buffer, const float* frame_in, float* magnitude_out, int frame_size) {
    
    // 1. Preparazione del buffer complesso (Reale, Immaginario = 0)
    for (int i = 0; i < frame_size; i++) {
        fft_buffer[2 * i] = frame_in[i];
        fft_buffer[2 * i + 1] = 0.0f;
    }

    // 2. Esecuzione FFT Radix-2
    dsps_fft2r_fc32(fft_buffer, frame_size);

    // 3. Bit-reversal per ordinare i bin di frequenza
    dsps_bit_rev_fc32(fft_buffer, frame_size);

    //dsps_cplx2reC_fc32(fft_buffer, frame_size);


    // A. Bin 0 (DC) -> Immaginaria è idealmente 0
    magnitude_out[0] = fabsf(fft_buffer[0]); // Se vuoi la Magnitudine

    // B. Bin centrali (da 1 a N/2 - 1)
    for (int i = 1; i < frame_size / 2; i++) {
        float re = fft_buffer[2 * i];
        float im = fft_buffer[2 * i + 1];
        
        // Opzione Magnitudine lineare
        magnitude_out[i] = sqrtf(re * re + im * im);
    }

    // C. Bin Nyquist (frame_size / 2) -> Si trova all'indice 2 * (N/2) = frame_size
    // Anche qui l'immaginaria è idealmente 0
    magnitude_out[frame_size / 2] = fabsf(fft_buffer[frame_size]);
    
    return 0;
}

void apply_mel_filter_banks(const float* magnitude_in, const float *matrix, float* mel_out, int frame_size, int n_mels) {
    for (int i = 0; i < n_mels; i++) {
        float sum = 0.0f;
        for (int j = 0; j <= frame_size / 2; j++) {
            sum += magnitude_in[j] * matrix[i * (frame_size / 2 + 1) + j];
        }
        mel_out[i] = sum;
    }
}

void to_db(float* data, int size) {
    const float amin = 1e-10f; 
    const float top_db = 80.0f;
    
    // 1. Trova il massimo valore nel buffer per usarlo come riferimento (eventualmente fallo dopo)
    float ref = 1.0f;
    
    // Assicuriamoci che il riferimento non sia inferiore ad amin
    ref = std::max(ref, amin);
    float log_ref = 10.0f * log10f(ref);

    // 2. Converte in dB
    for (int i = 0; i < size; i++) {
        float val = std::max(data[i], amin);
        float db_value = 10.0f * log10f(val) - log_ref;
        
        // 3. Applica il limite (clipping)
        //data[i] = std::max(db_value, -top_db);
        data[i] = db_value;
    }

    float max = *std::max_element(data, data + size);
    for(int i = 0; i < size; i++) {
        data[i] = std::max(data[i], max - top_db);
    }

}

MelSpectrogram calculate_mel_spectrogram(const float* audio_input, int total_samples, int frame_size, int hop_size, int n_mels) {

    if (audio_input == nullptr) {
        Logger::instance().error("DSP", "Error: audio buffer pointer is null!\n");
        return MelSpectrogram();
    }

    // NOTA IMPORTANTE SULL'ALLINEAMENTO:
    // heap_caps_calloc NON garantisce 16-byte alignment. Le implementazioni
    // assembly-ottimizzate delle funzioni FFT di esp-dsp (usate automaticamente
    // su ESP32-S3, es. dsps_fft2r_fc32_ae32) richiedono che i buffer passati
    // siano allineati a 16 byte: se non lo sono, possono leggere/scrivere
    // memoria leggermente fuori dai limiti del buffer, in modo dipendente da
    // dove l'allocatore ha piazzato il blocco in quel momento (quindi il bug
    // "appare o sparisce" a seconda di cos'altro è stato allocato prima).
    // Usiamo quindi heap_caps_aligned_alloc(16, ...) + azzeramento manuale al
    // posto di heap_caps_calloc per tutti i buffer coinvolti nella pipeline FFT.
    float* mel_frame  = (float*)heap_caps_aligned_alloc(16, n_mels * sizeof(float), MALLOC_CAP_SPIRAM);
    float* magnitude  = (float*)heap_caps_aligned_alloc(16, (frame_size / 2 + 1) * sizeof(float), MALLOC_CAP_SPIRAM);
    float* fft_buffer = (float*)heap_caps_aligned_alloc(16, (frame_size * 2) * sizeof(float), MALLOC_CAP_SPIRAM);
    //float* mel_filters = (float*)heap_caps_aligned_alloc(16, (frame_size / 2 + 1) * n_mels * sizeof(float), MALLOC_CAP_SPIRAM);
    float* windowed   = (float*)heap_caps_aligned_alloc(16, frame_size * sizeof(float), MALLOC_CAP_SPIRAM);

    if (mel_frame)   memset(mel_frame, 0, n_mels * sizeof(float));
    if (magnitude)   memset(magnitude, 0, (frame_size / 2 + 1) * sizeof(float));
    if (fft_buffer)  memset(fft_buffer, 0, (frame_size * 2) * sizeof(float));
    //if (mel_filters) memset(mel_filters, 0, (frame_size / 2 + 1) * n_mels * sizeof(float));
    if (windowed)    memset(windowed, 0, frame_size * sizeof(float));

    if (!mel_frame || !magnitude || !fft_buffer || !windowed) {
        Logger::instance().error("DSP", "Error: allocation of buffer for Mel Spectrogram failed!");
        // Gestisci l'errore: libera quelli allocati e torna
        if (mel_frame)  heap_caps_free(mel_frame);
        if (magnitude)  heap_caps_free(magnitude);
        if (fft_buffer) heap_caps_free(fft_buffer);
        //if (mel_filters) heap_caps_free(mel_filters);
        if (windowed) heap_caps_free(windowed);
        return MelSpectrogram();
    }

    //memcpy(mel_filters, mel_matrix, (frame_size / 2 + 1) * n_mels * sizeof(float));

    int padding_amount = frame_size / 2; // Questo è il padding tipico di librosa
    int new_total_samples = total_samples + (2 * padding_amount);

    //printf("Padding audio: Aggiunta di %d campioni all'inizio e alla fine.\n", padding_amount);

    // 1. Alloca il nuovo buffer più grande (allineato: viene copiato in windowed
    //    e poi passato indirettamente alla FFT tramite window_hann -> windowed)
    float* padded_audio = (float*)heap_caps_aligned_alloc(16, new_total_samples * sizeof(float), MALLOC_CAP_SPIRAM);

    if (padded_audio == nullptr) {
        Logger::instance().error("MEM", "Error: Insufficient memory for padding");
        heap_caps_free(mel_frame);
        heap_caps_free(magnitude);
        heap_caps_free(fft_buffer);
        //heap_caps_free(mel_filters);
        heap_caps_free(windowed);

        return MelSpectrogram();
    }

    // heap_caps_aligned_alloc non azzera la memoria: lo facciamo esplicitamente,
    // dato che ci affidiamo agli zeri di padding lasciati ai due estremi del buffer.
    memset(padded_audio, 0, new_total_samples * sizeof(float));

    // 2. Copia i dati originali nel centro del nuovo buffer
    // La posizione di partenza sarà 'padding_amount'
    memcpy(&padded_audio[padding_amount], audio_input, total_samples * sizeof(float));


    int num_frames = 1 + (new_total_samples - frame_size) / hop_size;

    if (dsps_fft2r_init_fc32(NULL, frame_size) != ESP_OK) {
        Logger::instance().error("DSP", "Initialization of FFT failed");
        heap_caps_free(padded_audio);
        heap_caps_free(mel_frame);
        heap_caps_free(magnitude);
        heap_caps_free(fft_buffer);
        //heap_caps_free(mel_filters);
        heap_caps_free(windowed);
        return MelSpectrogram();
    }

    float* spectrogram_buffer = (float*)heap_caps_calloc(n_mels * num_frames, sizeof(float), MALLOC_CAP_SPIRAM);

    if (spectrogram_buffer == nullptr) {
        Logger::instance().error("MEM", "Error: Insufficient memory for spectrogram buffer");
        heap_caps_free(padded_audio);
        heap_caps_free(mel_frame);
        heap_caps_free(magnitude);
        heap_caps_free(fft_buffer);
        //heap_caps_free(mel_filters);
        heap_caps_free(windowed);
        return MelSpectrogram();
    }

    //printf("Numero totale di frame da processare: %d\n", num_frames);
    
    for (int f = 0; f < num_frames; f++) {

        window_hann(&padded_audio[f * hop_size], windowed, frame_size);

        // 1. STFT
        if (calculate_stft_frame(fft_buffer, windowed, magnitude, frame_size) != 0) {
            Logger::instance().error("DSP", "Error in STFT calculation for frame %d", f);
            heap_caps_free(padded_audio);
            heap_caps_free(mel_frame);
            heap_caps_free(magnitude);
            heap_caps_free(fft_buffer);
            //heap_caps_free(mel_filters);
            heap_caps_free(windowed);
            heap_caps_free(spectrogram_buffer);
            dsps_fft2r_deinit_fc32();
            return MelSpectrogram(); // Salta questo frame e continua con il successivo
        }

        for (int i = 0; i <= frame_size / 2; i++) {
            magnitude[i] = magnitude[i] * magnitude[i];
        }
        
        // 2. Filtri Mel
        apply_mel_filter_banks(magnitude, mel_matrix, mel_frame, frame_size, n_mels);   //mel_filters
        
        
        for (int i = 0; i < n_mels; i++) {
            spectrogram_buffer[i * num_frames + f] = mel_frame[i];
        }
        

        //printf("Frame %d/%d processato.\n", f + 1, num_frames);
    }

    // 3. Conversione in dB
    to_db(spectrogram_buffer, n_mels * num_frames);

    heap_caps_free(padded_audio);
    heap_caps_free(mel_frame);
    heap_caps_free(magnitude);
    heap_caps_free(fft_buffer);
    //heap_caps_free(mel_filters);
    heap_caps_free(windowed);
    dsps_fft2r_deinit_fc32(); 

    padded_audio = nullptr;
    mel_frame = nullptr;
    magnitude = nullptr;

    MelSpectrogram mel_spectrogram;
    mel_spectrogram.data = spectrogram_buffer;
    mel_spectrogram.n_mels = n_mels;
    mel_spectrogram.n_frames = num_frames;

    return mel_spectrogram;
}

void free_mel_spectrogram(MelSpectrogram& spec) {
    if (spec.data) {
        heap_caps_free(spec.data);
        spec.data = nullptr;
    }
}