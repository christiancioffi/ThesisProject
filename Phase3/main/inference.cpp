#include "inference.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// TensorFlow Lite Micro (Necessari)
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "CNN.h"
#include "MelSpectrogram.h"
#include "filter_module.h"
#include "filter_coeffs.h"
#include "Logger.h"

#define SAMPLE_RATE_HZ 16000
#define OFFSET_TO_CUT 0.5
#define FRAME_SIZE 2048
#define HOP_SIZE 512
#define N_MELS 128

#define MODEL_NAME "CNN1D"

#define DEBUG_MODE 1

#define LOG_TAG "INFERENCE"

// Parametri dello Spettrogramma calcolati sul subset di Training
const float SPEC_TRAIN_MEAN   = -60.619049072265625f;  
const float SPEC_TRAIN_STD    = 12.7423095703125f;   

// Parametri delle Labels (Portate) calcolate sul subset di Training
const float LABELS_TRAIN_MEAN = 0.3859245630174793f;  
const float LABELS_TRAIN_STD  = 0.1672968350905522f;

// Dimensione dell'arena per TFLite Quantizzato (i modelli int8 occupano meno RAM)
const int TFLITE_ARENA_SIZE = 100 * 1024;   //100 per CNN1D, 200 per CNN_RNN
//uint8_t tensor_arena[TFLITE_ARENA_SIZE];


struct TFLiteContext {
    tflite::MicroInterpreter* interpreter = nullptr;
    uint8_t* arena = nullptr;

    TFLiteContext() = default;
    TFLiteContext(tflite::MicroInterpreter* i, uint8_t* a) : interpreter(i), arena(a) {}

    // Vieta la copia (eviterebbe il double-free)
    TFLiteContext(const TFLiteContext&) = delete;
    TFLiteContext& operator=(const TFLiteContext&) = delete;

    // Permetti solo lo spostamento
    TFLiteContext(TFLiteContext&& other) noexcept : interpreter(other.interpreter), arena(other.arena) {
        other.interpreter = nullptr;
        other.arena = nullptr;
    }
    TFLiteContext& operator=(TFLiteContext&& other) noexcept {
        if (this != &other) {
            if (interpreter) delete interpreter;
            if (arena) heap_caps_free(arena);
            interpreter = other.interpreter;
            arena = other.arena;
            other.interpreter = nullptr;
            other.arena = nullptr;
        }
        return *this;
    }

    ~TFLiteContext() {
        if (interpreter) delete interpreter;
        if (arena) heap_caps_free(arena);
    }
};

static void set_watchdog_timeout(uint32_t timeout_ms) {
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdt_config);
}

static TFLiteContext initialize_interpreter(bool use_quantized_model){

    const tflite::Model* model = nullptr;

    if(use_quantized_model) {
        Logger::instance().debug(LOG_TAG, "Initializing TFLite interpreter with quantized (int8) model...");
        model = tflite::GetModel(_model_int8);
    } else {
        Logger::instance().debug(LOG_TAG, "Initializing TFLite interpreter with float (float32) model...");
        model = tflite::GetModel(_model_f32);
    }

    if (model == nullptr) {
        Logger::instance().error(LOG_TAG, "Failed to get model!");
        return {nullptr, nullptr};
    }

    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Logger::instance().error(LOG_TAG, "Unsupported model schema version!");
        return {nullptr, nullptr};
    }

    uint8_t* tensor_arena = (uint8_t*)heap_caps_aligned_alloc(16, TFLITE_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    if (tensor_arena == nullptr) {
       Logger::instance().error(LOG_TAG, "Failed to allocate arena in PSRAM!");
       return {nullptr, nullptr};
    }

    // CNN1D
    static tflite::MicroMutableOpResolver<11> op_resolver;
    static bool resolvers_added = false;

    if (!resolvers_added) {
        op_resolver.AddConv2D();
        op_resolver.AddMaxPool2D();
        op_resolver.AddFullyConnected();
        op_resolver.AddReshape();
        op_resolver.AddMean();
        resolvers_added = true;
    }
    
    tflite::MicroInterpreter* interpreter = new(std::nothrow) tflite::MicroInterpreter(model, op_resolver, tensor_arena, TFLITE_ARENA_SIZE);

    if (interpreter == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of MicroInterpreter failed!");
        heap_caps_free(tensor_arena);
        return {nullptr, nullptr};
    }

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        size_t used = interpreter->arena_used_bytes();
        Logger::instance().error(LOG_TAG, "Allocation failed! Arena used: %d bytes, Arena available: %d", used, TFLITE_ARENA_SIZE);
        delete interpreter;
        heap_caps_free(tensor_arena);
        return {nullptr, nullptr};
    }

    return {interpreter, tensor_arena};
}

// ---------------------------------------------------------------------------
// FASE 1: preprocessing audio -> spettrogramma Mel.
// Non dipende dal modello: va eseguita una sola volta per ciclo di misura.
// ---------------------------------------------------------------------------
MelSpectrogram compute_spectrogram_from_audio(int32_t* pcm_buffer, size_t total_samples) {

    if (DEBUG_MODE) {
        float min_val_f = pcm_buffer[0];
        float max_val_f = pcm_buffer[0];
        for (size_t i = 0; i < total_samples; i++) {
            if (pcm_buffer[i] < min_val_f) min_val_f = pcm_buffer[i];
            if (pcm_buffer[i] > max_val_f) max_val_f = pcm_buffer[i];
        }
        Logger::instance().info(LOG_TAG, "PCM_BUFFER: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }

    // 1. Converti in float per il processamento Mel
    float* audio_buffer = (float*)heap_caps_calloc(total_samples, sizeof(float), MALLOC_CAP_SPIRAM);
    if (audio_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of audio_buffer failed");
        return MelSpectrogram();
    }

    for (size_t i = 0; i < total_samples; i++) {
        audio_buffer[i] = pcm_buffer[i] / 2147483648.0f; // Normalizzazione [-1, 1], il valore in questione è 2^31
    }

    if (DEBUG_MODE) {
        float min_val_f = audio_buffer[0];
        float max_val_f = audio_buffer[0];
        for (size_t i = 0; i < total_samples; i++) {
            if (audio_buffer[i] < min_val_f) min_val_f = audio_buffer[i];
            if (audio_buffer[i] > max_val_f) max_val_f = audio_buffer[i];
        }
        Logger::instance().info(LOG_TAG, "AUDIO_BUFFER: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }

    //-----------------FILTRO PASSA-ALTO-------------------

    Logger::instance().info(LOG_TAG, "Audio preprocessing: High-pass filter and trimming...\n");

    float* filtered_audio_buffer = (float*)heap_caps_calloc(total_samples, sizeof(float), MALLOC_CAP_SPIRAM);
    if (filtered_audio_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of filtered_audio_buffer failed");
        heap_caps_free(audio_buffer);
        return MelSpectrogram();
    }

    if (!apply_sosfiltfilt(audio_buffer, filtered_audio_buffer, total_samples, sos_coeffs, NUM_SECTIONS)) {
        Logger::instance().error(LOG_TAG, "Error during application of sosfiltfilt filter");
        heap_caps_free(audio_buffer);
        heap_caps_free(filtered_audio_buffer);
        return MelSpectrogram();
    }

    heap_caps_free(audio_buffer);
    audio_buffer = nullptr;

    if (DEBUG_MODE) {
        float min_val_f = filtered_audio_buffer[0];
        float max_val_f = filtered_audio_buffer[0];
        for (size_t i = 0; i < total_samples; i++) {
            if (filtered_audio_buffer[i] < min_val_f) min_val_f = filtered_audio_buffer[i];
            if (filtered_audio_buffer[i] > max_val_f) max_val_f = filtered_audio_buffer[i];
        }
        Logger::instance().info(LOG_TAG, "AUDIO AFTER HIGH-PASS FILTER: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }

    //-----------------TAGLIO-------------------

    int trim_samples = OFFSET_TO_CUT * SAMPLE_RATE_HZ;
    int total_samples_after_cut = total_samples - (2 * trim_samples);

    if (total_samples_after_cut <= 0) {
        Logger::instance().error(LOG_TAG, "total_samples_after_cut <= 0, audio troppo corto per il trimming configurato");
        heap_caps_free(filtered_audio_buffer);
        return MelSpectrogram();
    }

    float* preprocessed_audio_buffer = (float*)heap_caps_calloc(total_samples_after_cut, sizeof(float), MALLOC_CAP_SPIRAM);
    if (preprocessed_audio_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of preprocessed_audio_buffer failed");
        heap_caps_free(filtered_audio_buffer);
        return MelSpectrogram();
    }

    memcpy(preprocessed_audio_buffer, &filtered_audio_buffer[trim_samples], total_samples_after_cut * sizeof(float));

    heap_caps_free(filtered_audio_buffer);
    filtered_audio_buffer = nullptr;

    if (DEBUG_MODE) {
        float min_val_f = preprocessed_audio_buffer[0];
        float max_val_f = preprocessed_audio_buffer[0];
        for (size_t i = 0; i < total_samples_after_cut; i++) {
            if (preprocessed_audio_buffer[i] < min_val_f) min_val_f = preprocessed_audio_buffer[i];
            if (preprocessed_audio_buffer[i] > max_val_f) max_val_f = preprocessed_audio_buffer[i];
        }
        Logger::instance().info(LOG_TAG, "AUDIO AFTER PREPROCESSING: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }

    Logger::instance().info(LOG_TAG, "Audio preprocessing completed. Total samples after preprocessing: %d\n", total_samples_after_cut);

    set_watchdog_timeout(15000);

    //-----------------SPETTROGRAMMA-------------------

    Logger::instance().info(LOG_TAG, "Calculating spectrogram...\n");

    MelSpectrogram mel_spectrogram = calculate_mel_spectrogram(preprocessed_audio_buffer, total_samples_after_cut, FRAME_SIZE, HOP_SIZE, N_MELS);

    set_watchdog_timeout(5000);

    heap_caps_free(preprocessed_audio_buffer);
    preprocessed_audio_buffer = nullptr;

    if (mel_spectrogram.data == nullptr) {
        Logger::instance().error(LOG_TAG, "Error in Mel Spectrogram calculation");
        return mel_spectrogram; // data == nullptr, nulla da liberare
    }

    if (DEBUG_MODE) {
        float min_val_f = mel_spectrogram.data[0];
        float max_val_f = mel_spectrogram.data[0];
        for (size_t i = 0; i < (size_t)N_MELS * mel_spectrogram.n_frames; i++) {
            if (mel_spectrogram.data[i] < min_val_f) min_val_f = mel_spectrogram.data[i];
            if (mel_spectrogram.data[i] > max_val_f) max_val_f = mel_spectrogram.data[i];
        }
        Logger::instance().info(LOG_TAG, "SPECTROGRAM: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }

    return mel_spectrogram;
}

// ---------------------------------------------------------------------------
// FASE 2: quantizzazione (se richiesta) + inferenza + dequantizzazione,
// a partire da uno spettrogramma già calcolato.
// ---------------------------------------------------------------------------
float inference_from_spectrogram(const MelSpectrogram& spectrogram, bool use_quantized_model) {

    if (spectrogram.data == nullptr) {
        Logger::instance().error(LOG_TAG, "inference_from_spectrogram: spectrogram.data == nullptr");
        return -1.0f;
    }

    TFLiteContext tflite_context = initialize_interpreter(use_quantized_model);
    if (tflite_context.interpreter == nullptr) {
        // Prima: while(1) vTaskDelay(...) bloccava il device per sempre se
        // l'init falliva (es. arena insufficiente per una delle due varianti).
        // Ora si torna al chiamante con l'errore, che decide se skippare il
        // ciclo, così il device resta vivo e riprova al ciclo successivo.
        Logger::instance().error(LOG_TAG, "Initialization of TFLite interpreter failed.");
        return -1.0f;
    }
    tflite::MicroInterpreter* interpreter = tflite_context.interpreter;

    TfLiteTensor* input = interpreter->input(0);
    TfLiteTensor* output = interpreter->output(0);

    const float input_scale = input->params.scale;
    const int32_t input_zero_point = input->params.zero_point;
    const float output_scale = output->params.scale;
    const int32_t output_zero_point = output->params.zero_point;

    const int n_mels = spectrogram.n_mels;
    const int num_frames = spectrogram.n_frames;
    const float* spectrogram_buffer = spectrogram.data;

    // Trasposizione e Quantizzazione
    for (int h = 0; h < n_mels; h++) {
        for (int w = 0; w < num_frames; w++) {
            int target_idx = w * n_mels + h;
            float val = spectrogram_buffer[h * num_frames + w];

            float float_standardizzato = (val - SPEC_TRAIN_MEAN) / SPEC_TRAIN_STD;

            if (use_quantized_model) {
                int32_t quantizzato = std::round(float_standardizzato / input_scale) + input_zero_point;
                input->data.int8[target_idx] = (int8_t)std::max(-128, std::min(127, (int)quantizzato));
            } else {
                input->data.f[target_idx] = float_standardizzato;
            }
        }
    }

    if (interpreter->Invoke() != kTfLiteOk) {
        Logger::instance().error(LOG_TAG, "Invoke failed (use_quantized_model=%d)", (int)use_quantized_model);
        return -1.0f;
    }

    // --- DEQUANTIZZAZIONE DELL'OUTPUT ---
    float raw_float_output = 0.0f;
    if (use_quantized_model) {
        raw_float_output = (output->data.int8[0] - output_zero_point) * output_scale;
    } else {
        raw_float_output = output->data.f[0];
    }

    // --- DE-STANDARDIZZAZIONE: RIPRISTINO DELLA PORTATA FISICA REALE ---
    float predicted_value = (raw_float_output * LABELS_TRAIN_STD) + LABELS_TRAIN_MEAN;

    return predicted_value;
    // tflite_context esce da scope qui: il suo distruttore libera interprete e arena.
}

// ---------------------------------------------------------------------------
// Wrapper di comodo: FASE 1 + FASE 2 in sequenza per una singola variante.
// Per la doppia inferenza (int8 + float32) sullo stesso audio, preferire
// compute_spectrogram_from_audio() + due chiamate a inference_from_spectrogram().
// ---------------------------------------------------------------------------
float inference_from_audio(int32_t* pcm_buffer, size_t total_samples, bool use_quantized_model) {

    MelSpectrogram spectrogram = compute_spectrogram_from_audio(pcm_buffer, total_samples);
    if (spectrogram.data == nullptr) {
        return -1.0f;
    }

    float predicted_value = inference_from_spectrogram(spectrogram, use_quantized_model);

    free_mel_spectrogram(spectrogram);

    return predicted_value;
}
