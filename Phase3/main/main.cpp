// Librerie Standard C (Molto leggere)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>     
#include <dirent.h> 
#include <unistd.h> 
#include <inttypes.h>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <sys/time.h>
#include <time.h>
#include <regex>
#include <string>
#include <stdint.h>
#include <stdexcept>  // std::exception, catturato attorno alle funzioni LTE

// Driver ESP32 e FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h" 
#include "esp_heap_caps.h"
#include "I2SDriver.h"
#include "sd_card.h"
#include "inference.h"
#include "MelSpectrogram.h"
#include "ConfigLoader.h"
#include "eg91_sender.h"
#include "Logger.h"

// --- CONFIGURAZIONE PIN ---
#define SD_PIN_MISO   GPIO_NUM_13
#define SD_PIN_MOSI   GPIO_NUM_11
#define SD_PIN_CLK    GPIO_NUM_12
#define SD_PIN_CS     GPIO_NUM_1
#define I2S_BCLK_PIN GPIO_NUM_46
#define I2S_WS_PIN GPIO_NUM_47
#define I2S_DIN_PIN GPIO_NUM_48

#define LTE_UART_NUM UART_NUM_1
#define LTE_PIN_TX   GPIO_NUM_41
#define LTE_PIN_RX   GPIO_NUM_42
#define LTE_PIN_PWR  GPIO_NUM_4
#define LTE_PIN_RST  GPIO_NUM_3

#define SD_MOUNT_POINT    "/sd"
#define CSV_PATH SD_MOUNT_POINT "/predictions.csv"
#define SLEEP_BETWEEN_CYCLES_MS   (5 * 60 *1000)      // 5 minuti (5 * 60 *1000)
#define SYNC_INTERVAL_SEC (12 * 3600) // 12 ore (in secondi)
#define UPLOAD_INTERVAL_SEC (6 * 3600) // 6 ore (in secondi)
#define MAX_SYNC_RETRIES 10
#define UPLOAD_URL "https://tesi.aliagrid.com/predictions"    // ec2-3-122-216-71.eu-central-1.compute.amazonaws.com:8443

#define AUDIO_DURATION_SEC 2
#define SAMPLE_RATE_HZ 16000
#define TOTAL_SAMPLES (AUDIO_DURATION_SEC * SAMPLE_RATE_HZ)
#define OFFSET_TO_CUT 0.5
#define FRAME_SIZE 2048
#define HOP_SIZE 512
#define N_MELS 128
#define NUM_CHANNELS 1

#define LOG_TAG "MAIN"
static const char* LTE_TAG = "LTE";

// Definizione della struttura per l'header WAV (32-bit PCM)
typedef struct __attribute__((packed)) {
    char riff_id[4];            // "RIFF"
    uint32_t file_size;         // Dimensione file totale - 8
    char wave_id[4];            // "WAVE"
    char fmt_id[4];             // "fmt "
    uint32_t fmt_size;          // 16 per PCM
    uint16_t audio_format;      // 1 per PCM
    uint16_t num_channels;      // 1 per Mono, 2 per Stereo
    uint32_t sample_rate;       // es: 16000, 44100
    uint32_t byte_rate;         // sample_rate * num_channels * (bits/8)
    uint16_t block_align;       // num_channels * (bits/8)
    uint16_t bits_per_sample;   // 32
    char data_id[4];            // "data"
    uint32_t data_size;         // Dimensione dei dati audio in byte
} wav_header_t;

// Variabili in memoria RTC per tracciare sincronizzazione e upload
static RTC_DATA_ATTR time_t last_sync_time = 0;
static RTC_DATA_ATTR time_t last_upload_time = 0;

// Variabile globale z_offset
static std::string z_offset = "+0000";

// ---------------------------------------------------------------------------
// Inizializzazione SD + creazione header CSV se il file non esiste ancora
// ---------------------------------------------------------------------------
static bool init_storage() {
    esp_err_t ret = inizializza_sd(SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CLK, SD_PIN_CS,
                                    SD_MOUNT_POINT);
    if (ret != ESP_OK) {
        return false;
    }

    FILE* f = fopen(CSV_PATH, "r");
    if (f == NULL) {
        f = fopen(CSV_PATH, "w");
        if (f) {
            fprintf(f, "timestamp,prediction_int8,prediction_f32\n");
            fflush(f);
            int fd = fileno(f);
            if (fd >= 0) {
                fsync(fd);
            }
            fclose(f);
        }
    } else {
        fclose(f);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Funzione per leggere il file CSV in una stringa (Implementazione mancante aggiunta)
// ---------------------------------------------------------------------------
static std::string read_csv_to_string(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        return "";
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string buffer;
    if (length > 0) {
        buffer.resize(length);
        size_t read_bytes = fread(&buffer[0], 1, length, f);
        buffer.resize(read_bytes);
    }
    fclose(f);
    return buffer;
}

// ---------------------------------------------------------------------------
// Timestamp: ora reale (epoch in millisecondi), letta dal clock di sistema.
// ---------------------------------------------------------------------------
static int64_t get_timestamp_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ---------------------------------------------------------------------------
// Logica di controllo per le 12 ore basata su variabile
// ---------------------------------------------------------------------------
static bool should_sync_time() {
    time_t now;
    time(&now);
    
    if (now < 1767225600 || last_sync_time == 0) { 
        Logger::instance().info(LTE_TAG, "Clock not initialized. Synchronization required.");
        return true;
    }

    double elapsed_seconds = difftime(now, last_sync_time);
    if (elapsed_seconds >= SYNC_INTERVAL_SEC) {
        Logger::instance().info(LTE_TAG, "12 hours have passed (%.0f seconds). Synchronization required.", elapsed_seconds);
        return true;
    }

    Logger::instance().info(LTE_TAG, "Synchronization skipped. Still %.0f seconds until the next 12-hour period.", SYNC_INTERVAL_SEC - elapsed_seconds);
    return false;
}

static void save_last_sync_time() {
    time(&last_sync_time);
    Logger::instance().info(LTE_TAG, "Timestamp of the last synchronization updated in memory.");
}

// Mappatura dei simboli generati dal sistema di build per il file .api-key
extern const uint8_t api_key_start[] asm("_binary__api_key_start");
extern const uint8_t api_key_end[]   asm("_binary__api_key_end");

// Funzione helper per leggere il file dalla memoria Flash
static std::string get_api_key() {
    size_t length = api_key_end - api_key_start;
    std::string key((const char*)api_key_start, length);
    
    // Rimuovi eventuali \n o \r finali
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r')) {
        key.pop_back();
    }
    return key;
}

// ---------------------------------------------------------------------------
// Funzioni LTE
// ---------------------------------------------------------------------------

bool synchronize_system_time(Eg91Sender& eg91) {
    std::string time_str, dst_str;
    
    if (!eg91.get_time(time_str, dst_str)) {
        Logger::instance().warn(LTE_TAG, "Unable to get time from cellular network.");
        return false;
    }

    int year, month, day, hour, minute, second, zz;
    try {
        std::regex time_regex(R"((\d{4})/(\d{2})/(\d{2}),(\d{2}):(\d{2}):(\d{2})([+-]\d+))");
        std::smatch m;

        if (!std::regex_search(time_str, m, time_regex)) {
            Logger::instance().error(LTE_TAG, "Invalid time format: %s", time_str.c_str());
            return false;
        }

        // std::stoi lancia std::invalid_argument/std::out_of_range su input
        // malformato o fuori range: la regex sopra vincola gia' i gruppi a
        // sole cifre, quindi in condizioni normali non dovrebbe mai
        // succedere, ma time_str arriva dal modem (dato "esterno") e non
        // vale la pena scommetterci la stabilita' del ciclo principale.
        year   = std::stoi(m[1].str());
        month  = std::stoi(m[2].str());
        day    = std::stoi(m[3].str());
        hour   = std::stoi(m[4].str());
        minute = std::stoi(m[5].str());
        second = std::stoi(m[6].str());
        zz     = std::stoi(m[7].str());
    } catch (const std::regex_error& e) {
        Logger::instance().error(LTE_TAG, "regex_error in parsing the network time: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        Logger::instance().error(LTE_TAG, "Invalid network time format (%s): %s", e.what(), time_str.c_str());
        return false;
    }

    char sign = (zz >= 0) ? '+' : '-';
    int abs_zz = std::abs(zz);
    int total_minutes = abs_zz * 15;
    int hours = total_minutes / 60;
    int minutes = total_minutes % 60;
    
    char z_offset_buf[16];
    snprintf(z_offset_buf, sizeof(z_offset_buf), "%c%02d%02d", sign, hours, minutes);
    
    z_offset = z_offset_buf;

    struct tm tm_info = {0};
    tm_info.tm_year = year - 1900;
    tm_info.tm_mon  = month - 1;
    tm_info.tm_mday = day;
    tm_info.tm_hour = hour;
    tm_info.tm_min  = minute;
    tm_info.tm_sec  = second;

    time_t t = mktime(&tm_info);
    if (t == -1) {
        Logger::instance().error(LTE_TAG, "Temporal conversion failed with mktime");
        return false;
    }

    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    Logger::instance().info(LTE_TAG, "Clock synchronized successfully! Date/Time: %04d/%02d/%02d %02d:%02d:%02d (Z offset: %s)",
             year, month, day, hour, minute, second, z_offset.c_str());
    return true;
}

bool synchronize_clock() {
    // Eg91Sender::create() e synchronize_system_time() gestiscono gia' al
    // loro interno le eccezioni che possono nascere (regex, stoi, ecc.) e
    // non dovrebbero propagarne. Questo try/catch resta comunque come
    // ultima rete di sicurezza per il ciclo LTE nel main loop: se qualcosa
    // di imprevisto sfuggisse comunque (es. std::bad_alloc costruendo
    // Eg91Config/lte_config), preferiamo loggare e passare al prossimo
    // ciclo piuttosto che lasciare un'eccezione non gestita far ripartire
    // il dispositivo in modo incontrollato a meta' di un ciclo di misura.
    try {
        if (should_sync_time()) {
            Eg91Config lte_config;
            if (load_eg91_config(lte_config)) {
                auto eg91 = Eg91Sender::create(LTE_UART_NUM, LTE_PIN_TX, LTE_PIN_RX, LTE_PIN_PWR, LTE_PIN_RST, lte_config);
                if (eg91) {
                    Logger::instance().info(LTE_TAG, "EG91 module initialized successfully.");
                    if (synchronize_system_time(*eg91)) {
                        save_last_sync_time(); 
                        return true; // Sincronizzazione andata a buon fine
                    } else {
                        return false; // Errore durante la sincronizzazione dell'ora
                    }
                } else {
                    Logger::instance().error(LTE_TAG, "Failed to initialize EG91 module.");
                    return false; // Errore di inizializzazione modem
                }
            } else {
                Logger::instance().warn(LOG_TAG, "Unable to load the JSON configuration file. Continuing without synchronization.");
                return false; // Errore di caricamento config
            }
        }

        // Se non era necessario sincronizzare, l'operazione è comunque priva di errori
        return true;
    } catch (const std::exception& e) {
        Logger::instance().error(LTE_TAG, "Unexpected exception during clock synchronization: %s", e.what());
        return false;
    } catch (...) {
        Logger::instance().error(LTE_TAG, "Unknown exception during clock synchronization");
        return false;
    }
}

static bool should_upload_time() {
    time_t now;
    time(&now);
    
    if (now < 1767225600) { 
        Logger::instance().info(LTE_TAG, "Clock not yet synchronized. Upload skipped.");
        return false;
    }

    if (last_upload_time == 0) {
        Logger::instance().info(LTE_TAG, "First CSV upload requested.");
        return true;
    }

    double elapsed_seconds = difftime(now, last_upload_time);
    if (elapsed_seconds >= UPLOAD_INTERVAL_SEC) {
        Logger::instance().info(LTE_TAG, "%.0f seconds have passed since last CSV upload. Procedure requested.", elapsed_seconds);
        return true;
    }

    Logger::instance().info(LTE_TAG, "CSV upload skipped. Still need to wait %.0f seconds.", UPLOAD_INTERVAL_SEC - elapsed_seconds);
    return false;
}

static void send_predictions_to_server() {
    // Stessa logica di sicurezza di synchronize_clock(): https_post() e
    // create() sono gia' protetti internamente (contratto "mai un throw"
    // di Eg91Sender), ma questa funzione orchestra l'intero ciclo di
    // upload nel main loop, quindi resta comunque l'ultima rete di
    // sicurezza per qualunque eccezione sfuggisse (es. da std::string/
    // std::vector su allocazioni relative a csv_data, headers, ecc.).
    try {
        std::string csv_data = read_csv_to_string(CSV_PATH);
        if (csv_data.empty()) {
            Logger::instance().warn(LOG_TAG, "Empty or illegible CSV file, skipping upload.");
            return;
        }

        // Lettura della chiave direttamente dalla Flash
        std::string api_key = get_api_key();
        if (api_key.empty()) {
            Logger::instance().warn(LOG_TAG, "API Key not found in firmware.");
        }

        Eg91Config lte_config;
        if (!load_eg91_config(lte_config)) {
            Logger::instance().warn(LOG_TAG, "Unable to load LTE configuration for upload.");
            return;
        }

        auto eg91 = Eg91Sender::create(LTE_UART_NUM, LTE_PIN_TX, LTE_PIN_RX, 
                                       LTE_PIN_PWR, LTE_PIN_RST, lte_config);
        if (!eg91) {
            Logger::instance().error(LTE_TAG, "Failed to initialize EG91 module during upload.");
            return;
        }

        // Aggiunta dell'header
        HeaderList headers;
        if (!api_key.empty()) {
            headers.push_back({"X-API-KEY", api_key});
        }

        HttpsResult result;
        Eg91Status status = eg91->https_post(UPLOAD_URL, csv_data, result, 0, "text/plain", &headers);
        
        if (status == Eg91Status::Ok && result.http_status >= 200 && result.http_status < 300) {
            Logger::instance().info(LOG_TAG, "Upload of CSV completed successfully!");
            time(&last_upload_time); // Aggiorna il timestamp dell'ultimo upload
            
            // Svuota/azzera il file CSV dopo l'upload riuscito
            FILE* f = fopen(CSV_PATH, "w");
            if (f) {
                fprintf(f, "timestamp,prediction_int8,prediction_f32\n");
                fflush(f);              // svuota il buffer di stdio
                int fd = fileno(f);     // ottieni il file descriptor
                if (fd >= 0) {
                    fsync(fd);           // forza la scrittura sul filesystem/flash
                }
                fclose(f);
                Logger::instance().info(LOG_TAG, "CSV file cleaned and reset with the header.");
            }
        } else {
            Logger::instance().error(LOG_TAG, "Upload CSV failed. HTTP Status: %d", result.http_status);
        }
    } catch (const std::exception& e) {
        Logger::instance().error(LOG_TAG, "Unexpected exception during CSV upload: %s", e.what());
    } catch (...) {
        Logger::instance().error(LOG_TAG, "Unknown exception during CSV upload");
    }
}

// ---------------------------------------------------------------------------
// Scrittura su CSV
// ---------------------------------------------------------------------------
static void append_prediction_to_csv(const char* timestamp_str, float prediction_int8, float prediction_f32) {
    FILE* f = fopen(CSV_PATH, "a");
    if (f == NULL) {
        Logger::instance().error(LOG_TAG, "Unable to open %s in append mode", CSV_PATH);
        return;
    }
    fprintf(f, "%s,%.8f,%.8f\n", timestamp_str, prediction_int8, prediction_f32);
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);
}

void save_audio(const int32_t* buffer, size_t dimensione, const char* percorsoRelativo) {
    char percorsoCompleto[128];
    snprintf(percorsoCompleto, sizeof(percorsoCompleto), "%s/%s", SD_MOUNT_POINT, percorsoRelativo);

    uint32_t data_size = dimensione * sizeof(int32_t);

    wav_header_t header;
    memcpy(header.riff_id, "RIFF", 4);
    header.file_size = data_size + sizeof(wav_header_t) - 8;
    memcpy(header.wave_id, "WAVE", 4);
    memcpy(header.fmt_id, "fmt ", 4);
    header.fmt_size = 16;
    header.audio_format = 1; 
    header.num_channels = NUM_CHANNELS;
    header.sample_rate = SAMPLE_RATE_HZ;
    header.bits_per_sample = 32;
    header.byte_rate = SAMPLE_RATE_HZ * NUM_CHANNELS * (header.bits_per_sample / 8);
    header.block_align = NUM_CHANNELS * (header.bits_per_sample / 8);
    memcpy(header.data_id, "data", 4);
    header.data_size = data_size;

    FILE* f = fopen(percorsoCompleto, "wb");
    if (f == NULL) {
        Logger::instance().error(LOG_TAG, "Error opening audio file");
        return;
    }

    fwrite(&header, sizeof(wav_header_t), 1, f);
    size_t elementiScritti = fwrite(buffer, sizeof(int32_t), dimensione, f);
    fclose(f);

    if (elementiScritti == dimensione) {
        Logger::instance().info(LOG_TAG, "WAV file saved successfully: %d audio bytes", (int)data_size);
    } else {
        Logger::instance().error(LOG_TAG, "Error writing to audio file!");
    }
}


extern "C" void app_main(void){

    if (!init_storage()) {
        ESP_LOGE(LOG_TAG, "Init SD card failed, stopping.");
        return;
    }

    Logger::instance().init("/sd/Log/log.txt", 2 * 1024 * 1024, 20);


    I2SDriver mic(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DIN_PIN, SAMPLE_RATE_HZ);
    if (!mic.is_valid()) {
        Logger::instance().error(LOG_TAG, "Init I2S failed, stopping.");
        return;
    }

    int32_t* pcm_buffer = (int32_t*)heap_caps_malloc(TOTAL_SAMPLES * sizeof(int32_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buffer) {
        pcm_buffer = (int32_t*)malloc(TOTAL_SAMPLES * sizeof(int32_t));
    }

    if (!pcm_buffer) {
        Logger::instance().error(LOG_TAG, "Buffer allocation failed, stopping.");
        return;
    }

    Logger::instance().info(LOG_TAG, "Starting cycle: %.1fs of audio at %d Hz (%zu samples)",
             (float)AUDIO_DURATION_SEC, SAMPLE_RATE_HZ, TOTAL_SAMPLES);

    while (true) {

        // 1. Sincronizzazione orologio (massimo MAX_SYNC_RETRIES tentativi)
        bool synced = false;

        for (int i = 0; i < MAX_SYNC_RETRIES; ++i) {
            if (synchronize_clock()) {
                synced = true;
                break; // Usciamo dal ciclo se la sincronizzazione ha successo
            }
            Logger::instance().warn(LOG_TAG, "Clock synchronization failed (attempt %d/%d). Retrying...", i + 1, MAX_SYNC_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        if (!synced) {
            Logger::instance().error(LOG_TAG, "Synchronization failed after %d attempts. The device will operate without updated network timestamp.", MAX_SYNC_RETRIES);
        }

        Logger::instance().info(LOG_TAG, "========== Audio recording of %d seconds ==========", AUDIO_DURATION_SEC);

        size_t samples_read = mic.record(pcm_buffer, AUDIO_DURATION_SEC);

        Logger::instance().info(LOG_TAG, "========== End audio recording ==========");

        int64_t ts = get_timestamp_ms();
        ts -= 500;                              // Per il preprocessing

        time_t sec = ts / 1000;
        int ms = ts % 1000;
        struct tm timeinfo;
        localtime_r(&sec, &timeinfo);

        char time_buf[32];
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);

        char iso_timestamp[64];
        snprintf(iso_timestamp, sizeof(iso_timestamp), "%s.%03d%s", time_buf, ms, z_offset.c_str());

        if (samples_read < TOTAL_SAMPLES) {
            Logger::instance().warn(LOG_TAG, "Only %zu/%zu samples recorded, skipping cycle",
                     samples_read, TOTAL_SAMPLES);
            vTaskDelay(pdMS_TO_TICKS(SLEEP_BETWEEN_CYCLES_MS));
            continue;
        }

        // Spettrogramma calcolato UNA VOLTA SOLA (filtro/trimming/Mel-spectrogram
        // sono identici per le due varianti: dipendono solo dall'audio, non dal modello).
        MelSpectrogram spectrogram = compute_spectrogram_from_audio(pcm_buffer, TOTAL_SAMPLES);
        if (spectrogram.data == nullptr) {
            Logger::instance().error(LOG_TAG, "Error during spectrogram computation, skipping cycle.");
            Logger::instance().info(LOG_TAG, "Sleeping for %d ms before next cycle...", SLEEP_BETWEEN_CYCLES_MS);
            vTaskDelay(pdMS_TO_TICKS(SLEEP_BETWEEN_CYCLES_MS));
            Logger::instance().info(LOG_TAG, "Woke up, starting next cycle...");
            continue;
        }

        Logger::instance().info(LOG_TAG, "Computing predictions for timestamp: %s", iso_timestamp);

        int64_t start_int8 = esp_timer_get_time();
        float prediction_int8 = inference_from_spectrogram(spectrogram, true);
        int64_t duration_int8 = esp_timer_get_time() - start_int8;
        Logger::instance().info(LOG_TAG, "Prediction (int8): %.8f (Time: %.3f ms)", prediction_int8, duration_int8 / 1000.0f);

        int64_t start_f32 = esp_timer_get_time();
        float prediction_f32 = inference_from_spectrogram(spectrogram, false);
        int64_t duration_f32 = esp_timer_get_time() - start_f32;
        Logger::instance().info(LOG_TAG, "Prediction (float32): %.8f (Time: %.3f ms)", prediction_f32, duration_f32 / 1000.0f);

        free_mel_spectrogram(spectrogram);

        if(prediction_int8 < 0.0f) {
            Logger::instance().error(LOG_TAG, "Error during inference (int8), skipping cycle.");
            Logger::instance().info(LOG_TAG, "Sleeping for %d ms before next cycle...", SLEEP_BETWEEN_CYCLES_MS);
            vTaskDelay(pdMS_TO_TICKS(SLEEP_BETWEEN_CYCLES_MS));
            Logger::instance().info(LOG_TAG, "Woke up, starting next cycle...");
            continue;
        }

        if(prediction_f32 < 0.0f) {
            Logger::instance().error(LOG_TAG, "Error during inference (float32), skipping cycle.");
            Logger::instance().info(LOG_TAG, "Sleeping for %d ms before next cycle...", SLEEP_BETWEEN_CYCLES_MS);
            vTaskDelay(pdMS_TO_TICKS(SLEEP_BETWEEN_CYCLES_MS));
            Logger::instance().info(LOG_TAG, "Woke up, starting next cycle...");
            continue;
        }

        Logger::instance().info(LOG_TAG, "t=%s  prediction_int8=%.8f  prediction_f32=%.8f", iso_timestamp, prediction_int8, prediction_f32);

        // Aggiunge la predizione al CSV
        append_prediction_to_csv(iso_timestamp, prediction_int8, prediction_f32);
        
        // 2. Controllo e invio periodico del CSV al server (ogni 6 ore) - CORRETTO
        if (should_upload_time()) {
            send_predictions_to_server();
        }
        
        /*

        // 3. Salvataggio del file audio locale
        std::string safe_timestamp = time_buf;
        std::replace(safe_timestamp.begin(), safe_timestamp.end(), ':', '-');

        char percorso_audio[128];
        snprintf(percorso_audio, sizeof(percorso_audio), "Audio/audio_%s.wav", safe_timestamp.c_str());

        save_audio(pcm_buffer, TOTAL_SAMPLES, percorso_audio);
        
        */

        
        Logger::instance().info(LOG_TAG, "Sleeping for %d ms before next cycle...", SLEEP_BETWEEN_CYCLES_MS);
        vTaskDelay(pdMS_TO_TICKS(SLEEP_BETWEEN_CYCLES_MS));
        Logger::instance().info(LOG_TAG, "Woke up, starting next cycle...");
    }

    free(pcm_buffer);
}