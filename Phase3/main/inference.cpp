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

#include "CNN_RNN.h"
#include "MelSpectrogram.h"
#include "filter_module.h"
#include "filter_coeffs.h"
#include "Logger.h"

#define SAMPLE_RATE_HZ 16000
#define OFFSET_TO_CUT 0.5
#define FRAME_SIZE 2048
#define HOP_SIZE 512
#define N_MELS 128

#define MODEL_NAME "CNN1D_RNN"
#define USE_QUANTIZED_MODEL 1

#define DEBUG_MODE 1

#define LOG_TAG "INFERENCE"

// Parametri dello Spettrogramma calcolati sul subset di Training
const float SPEC_TRAIN_MEAN   = -60.698692321777344f;  
const float SPEC_TRAIN_STD    = 12.708149909973145f;   

// Parametri delle Labels (Portate) calcolate sul subset di Training
const float LABELS_TRAIN_MEAN = 0.38474632866537184f;  
const float LABELS_TRAIN_STD  = 0.16673187280278606f;

// Dimensione dell'arena per TFLite Quantizzato (i modelli int8 occupano meno RAM)
const int TFLITE_ARENA_SIZE = 200 * 1024;   //100 per CNN1D, 200 per CNN_RNN
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

void set_watchdog_timeout(uint32_t timeout_ms) {
    // 1. Salva il timeout attuale o inizializza con un valore alto
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdt_config);
}

TFLiteContext initialize_interpreter(){
    const tflite::Model* model = tflite::GetModel(_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Logger::instance().error(LOG_TAG, "Unsupported model schema version!");
        return {nullptr, nullptr};
    }

    uint8_t* tensor_arena = (uint8_t*)heap_caps_aligned_alloc(16, TFLITE_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    if (tensor_arena == nullptr) {
       Logger::instance().info(LOG_TAG, "Error: Failed to allocate arena in PSRAM!\n");
       return {nullptr, nullptr};
    }

    // CNN1D_RNN
    static tflite::MicroMutableOpResolver<11> op_resolver;
    static bool resolvers_added = false;

    if (!resolvers_added) {
        op_resolver.AddTanh();
        op_resolver.AddUnpack();        //
        op_resolver.AddConv2D();
        op_resolver.AddSplit();         //
        op_resolver.AddQuantize();      //
        op_resolver.AddReshape();
        op_resolver.AddDequantize();    //
        op_resolver.AddFullyConnected(); //
        op_resolver.AddLogistic();      // (corrisponde alla funzione Sigmoid)
        op_resolver.AddAdd();           //
        op_resolver.AddMul();
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
        heap_caps_free(tensor_arena);
        return {nullptr, nullptr};
    }

    return {interpreter, tensor_arena};
}

float inference_from_audio(int32_t* pcm_buffer, size_t total_samples) {

    TFLiteContext tflite_context = initialize_interpreter();
    if (tflite_context.interpreter == nullptr) {
        Logger::instance().error(LOG_TAG, "Initialization of TFLite interpreter failed.");
        while(1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    tflite::MicroInterpreter* interpreter = tflite_context.interpreter;
    uint8_t* tensor_arena = tflite_context.arena;

    TfLiteTensor* input = interpreter->input(0);
    TfLiteTensor* output = interpreter->output(0);

    const float input_scale = input->params.scale;
    const int32_t input_zero_point = input->params.zero_point;
    const float output_scale = output->params.scale;
    const int32_t output_zero_point = output->params.zero_point;

    if (DEBUG_MODE) {
        float min_val_f = pcm_buffer[0]; 
        float max_val_f = pcm_buffer[0];
        
        for (size_t i = 0; i < total_samples; i++) {
            
            if (pcm_buffer[i] < min_val_f) min_val_f = pcm_buffer[i];
            if (pcm_buffer[i] > max_val_f) max_val_f = pcm_buffer[i];
        }

       Logger::instance().info(LOG_TAG, "PCM_BUFFER: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }
    
    // 2. Converti in float per il processamento Mel
    float* audio_buffer = (float*)heap_caps_calloc(total_samples, sizeof(float), MALLOC_CAP_SPIRAM);
    if (audio_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of audio_buffer failed");
        return -1.0f; // Indica un errore
    }

    for (size_t i = 0; i < total_samples; i++) {
        audio_buffer[i] = pcm_buffer[i] / 2147483648.0f; // Normalizzazione [-1, 1], il valore in questione è 2^31
        //printf("Sample %zu: %.8f->%.8f\n", i, pcm_buffer[i], audio_buffer[i]);
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

    //-----------------PRE-PROCESSING-------------------

    //-----------------FILTRO PASSA-ALTO-------------------


   Logger::instance().info(LOG_TAG, "Audio preprocessing: High-pass filter and trimming...\n");
    
    float * filtered_audio_buffer = (float*)heap_caps_calloc(total_samples, sizeof(float), MALLOC_CAP_SPIRAM);
    if (filtered_audio_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of filtered_audio_buffer failed");
        heap_caps_free(audio_buffer);
        audio_buffer = nullptr;
        return -1.0f; // Indica un errore
    }
    
    if(!apply_sosfiltfilt(audio_buffer, filtered_audio_buffer, total_samples, sos_coeffs, NUM_SECTIONS)) {
        Logger::instance().error(LOG_TAG, "Error during application of sosfiltfilt filter");
        heap_caps_free(audio_buffer);
        audio_buffer = nullptr;
        heap_caps_free(filtered_audio_buffer);
        filtered_audio_buffer = nullptr;
        return -1.0f; // Indica un errore
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
    
    int trim_samples = OFFSET_TO_CUT*SAMPLE_RATE_HZ;
    
    int total_samples_after_cut = total_samples - (2 * trim_samples);

    // Alloca il nuovo buffer ridotto
    float* preprocessed_audio_buffer = (float*)heap_caps_calloc(total_samples_after_cut, sizeof(float), MALLOC_CAP_SPIRAM);
    if (preprocessed_audio_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Allocation of preprocessed_audio_buffer failed");
        heap_caps_free(filtered_audio_buffer);
        filtered_audio_buffer = nullptr;
        return -1.0f; // Indica un errore
    }

    // Alloca il nuovo buffer ridotto
    // 2. Copia i dati dal grande al piccolo
    memcpy(preprocessed_audio_buffer, &filtered_audio_buffer[trim_samples], total_samples_after_cut * sizeof(float)); 

    heap_caps_free(filtered_audio_buffer);
    filtered_audio_buffer = nullptr;

    if(DEBUG_MODE){
        float min_val_f = preprocessed_audio_buffer[0];
        float max_val_f = preprocessed_audio_buffer[0];
        
        for (size_t i = 0; i < total_samples_after_cut; i++) {
            
            if (preprocessed_audio_buffer[i] < min_val_f) min_val_f = preprocessed_audio_buffer[i];
            if (preprocessed_audio_buffer[i] > max_val_f) max_val_f = preprocessed_audio_buffer[i];

        }

       Logger::instance().info(LOG_TAG, "AUDIO AFTER PREPROCESSING: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }
    
    /*
    for(int i = 0; i< 20; i++){
       Logger::instance().info(LOG_TAG, "Sample %d: %.8f ", i, preprocessed_audio_buffer[i]);
       Logger::instance().info(LOG_TAG, "\n");
    }
   Logger::instance().info(LOG_TAG, "\n");
    */
    
   Logger::instance().info(LOG_TAG, "Audio preprocessing completed. Total samples after preprocessing: %d\n", total_samples_after_cut);

    set_watchdog_timeout(15000);

    //-----------------SPETTROGRAMMA-------------------

   Logger::instance().info(LOG_TAG, "Calculating spectrogram...\n");

    //printf("Dimensione frame: %d, Hop size: %d, Numero di Mel bands: %d, total_samples_after_cut: %d\n", FRAME_SIZE, HOP_SIZE, N_MELS, total_samples_after_cut);
    
    // 3. Calcolo Spettrogramma
    MelSpectrogram mel_spectrogram = calculate_mel_spectrogram(preprocessed_audio_buffer, total_samples_after_cut, FRAME_SIZE, HOP_SIZE, N_MELS);

    float* spectrogram_buffer = mel_spectrogram.data;
    int n_mels = mel_spectrogram.n_mels;
    int num_frames = mel_spectrogram.n_frames;

    if (spectrogram_buffer == nullptr) {
        Logger::instance().error(LOG_TAG, "Error in Mel Spectrogram calculation");
        free_mel_spectrogram(mel_spectrogram);
        spectrogram_buffer = nullptr;
        heap_caps_free(preprocessed_audio_buffer);
        preprocessed_audio_buffer = nullptr;
        set_watchdog_timeout(5000);
        return -1.0f; // Indica un errore
    }

    //printf("Spettrogramma calcolato: %d frame, %d mel bands\n", num_frames, n_mels);

    set_watchdog_timeout(5000);
    

    heap_caps_free(preprocessed_audio_buffer);
    preprocessed_audio_buffer = nullptr;
    
    if(DEBUG_MODE){
        float min_val_f = spectrogram_buffer[0];
        float max_val_f = spectrogram_buffer[0];

        for (size_t i = 0; i < N_MELS * num_frames; i++) {
            
            if (spectrogram_buffer[i] < min_val_f) min_val_f = spectrogram_buffer[i];
            if (spectrogram_buffer[i] > max_val_f) max_val_f = spectrogram_buffer[i];
        }

       Logger::instance().info(LOG_TAG, "SPECTROGRAM: Min: %.8f, Max: %.8f\n", min_val_f, max_val_f);
    }
    
    /*
    for(int i = 0; i< num_frames; i++){
        for(int j = 0; j< 50; j++){
           Logger::instance().info(LOG_TAG, "Frame %d, Mel %d,: %.8f\n", i, j, spectrogram_buffer[j * num_frames + i]);
        }
       Logger::instance().info(LOG_TAG, "\n");
    }
   Logger::instance().info(LOG_TAG, "\n");
    */
    
    //-----------------INFERENZA-------------------
    
    
    // 4. Trasposizione e Quantizzazione
    for (int h = 0; h < N_MELS; h++) {
        for (int w = 0; w < num_frames; w++) {
            int target_idx = w * N_MELS + h; 
            float val = spectrogram_buffer[h * num_frames + w];

            float float_standardizzato = (val - SPEC_TRAIN_MEAN) / SPEC_TRAIN_STD;
            
            #if USE_QUANTIZED_MODEL
                int32_t quantizzato = std::round(float_standardizzato / input_scale) + input_zero_point;
                input->data.int8[target_idx] = (int8_t)std::max(-128, std::min(127, (int)quantizzato));
            #else
                input->data.f[target_idx] = float_standardizzato;
            #endif
        }
    }

    free_mel_spectrogram(mel_spectrogram);
    spectrogram_buffer = nullptr;

    if (interpreter->Invoke() != kTfLiteOk) return -1.0f; // Indica un errore

    // --- DEQUANTIZZAZIONE DELL'OUTPUT ---
    float raw_float_output = 0.0f;
    #if USE_QUANTIZED_MODEL
        raw_float_output = (output->data.int8[0] - output_zero_point) * output_scale;
    #else
        raw_float_output = output->data.f[0];
    #endif

    // --- DE-STANDARDIZZAZIONE: RIPRISTINO DELLA PORTATA FISICA REALE ---
    float predicted_value = (raw_float_output * LABELS_TRAIN_STD) + LABELS_TRAIN_MEAN;
    
    interpreter = nullptr;
    tensor_arena = nullptr;

    return predicted_value;
}
