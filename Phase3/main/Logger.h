#pragma once

#include <cstdio>
#include <cstdint>
#include <cstdarg>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Livelli di log supportati, in ordine crescente di severità.
enum class LogLevel : uint8_t {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

// Logger thread-safe (singleton) per ESP-IDF.
// - Stampa a console con colori ANSI in base al livello.
// - Scrive su file di log (es. su SPIFFS / LittleFS / FATFS montato).
// - Esegue il rotate del file quando supera una dimensione massima.
// - Nessuna eccezione: tutte le funzioni restituiscono bool/codici d'errore.
class Logger {
public:
    static Logger& instance();

    // Deve essere chiamato una sola volta, dopo che il filesystem è montato.
    // maxFileSizeBytes: soglia oltre la quale scatta il rotate.
    // maxRotatedFiles: quanti file di backup mantenere (log.txt.1, log.txt.2, ...).
    bool init(const char* logFilePath,
              size_t maxFileSizeBytes = 64 * 1024,
              uint8_t maxRotatedFiles = 3);

    void deinit();

    void debug(const char* tag, const char* fmt, ...);
    void info(const char* tag, const char* fmt, ...);
    void warn(const char* tag, const char* fmt, ...);
    void error(const char* tag, const char* fmt, ...);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    void logInternal(LogLevel level, const char* tag, const char* fmt, va_list args);
    void rotateIfNeeded();
    void rotateNow();

    static const char* levelToString(LogLevel level);
    static const char* levelToColor(LogLevel level);

    static constexpr const char* kColorReset  = "\033[0m";
    static constexpr const char* kColorYellow = "\033[0;33m"; // DEBUG
    static constexpr const char* kColorGreen  = "\033[0;32m"; // INFO (old)
    static constexpr const char* kColorMagenta= "\033[0;35m"; // WARN
    static constexpr const char* kColorRed    = "\033[0;31m"; // ERROR
    static constexpr const char* kColorWhite  = "\033[0;37m"; // INFO

    SemaphoreHandle_t mMutex = nullptr;
    FILE* mFile = nullptr;

    char mLogFilePath[128] = {0};
    size_t mMaxFileSizeBytes = 0;
    uint8_t mMaxRotatedFiles = 0;
    bool mInitialized = false;
};
