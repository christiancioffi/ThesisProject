#include "Logger.h"

#include <cstring>
#include <sys/stat.h>

#include "esp_timer.h"
#include <time.h>
#include <unistd.h>

namespace {
constexpr size_t kMaxLogLineLen = 256;

// Crea la cartella che contiene logFilePath, se non esiste già.
// Necessario su filesystem con vere directory (LittleFS, FATFS);
// su SPIFFS (che è "flat") mkdir non serve ma non dà comunque fastidio:
// fallisce con ENOENT/EEXIST a seconda dei casi e viene ignorato.
void ensureParentDirExists(const char* logFilePath) {
    const char* lastSlash = strrchr(logFilePath, '/');
    if (lastSlash == nullptr || lastSlash == logFilePath) {
        return; // nessuna sottocartella nel path
    }

    char dirPath[128];
    size_t dirLen = static_cast<size_t>(lastSlash - logFilePath);
    if (dirLen >= sizeof(dirPath)) {
        dirLen = sizeof(dirPath) - 1;
    }
    memcpy(dirPath, logFilePath, dirLen);
    dirPath[dirLen] = '\0';

    struct stat st;
    if (stat(dirPath, &st) == 0) {
        return; // esiste già
    }

    mkdir(dirPath, 0775); // ignora l'errore: se fallisce, fopen() più avanti lo segnalerà comunque
}
}

Logger& Logger::instance() {
    static Logger sInstance;
    return sInstance;
}

Logger::~Logger() {
    deinit();
}

bool Logger::init(const char* logFilePath, size_t maxFileSizeBytes, uint8_t maxRotatedFiles) {
    if (mInitialized || logFilePath == nullptr) {
        return false;
    }

    mMutex = xSemaphoreCreateMutex();
    if (mMutex == nullptr) {
        return false;
    }

    strncpy(mLogFilePath, logFilePath, sizeof(mLogFilePath) - 1);
    mLogFilePath[sizeof(mLogFilePath) - 1] = '\0';

    mMaxFileSizeBytes = maxFileSizeBytes;
    mMaxRotatedFiles = maxRotatedFiles;

    ensureParentDirExists(mLogFilePath);

    // Apre in append: se il file esiste continua a scrivere in coda,
    // altrimenti lo crea.
    mFile = fopen(mLogFilePath, "a");
    if (mFile == nullptr) {
        vSemaphoreDelete(mMutex);
        mMutex = nullptr;
        return false;
    }

    mInitialized = true;
    return true;
}

void Logger::deinit() {
    if (!mInitialized) {
        return;
    }
    if (mMutex != nullptr) {
        xSemaphoreTake(mMutex, portMAX_DELAY);
    }
    if (mFile != nullptr) {
        fclose(mFile);
        mFile = nullptr;
    }
    if (mMutex != nullptr) {
        xSemaphoreGive(mMutex);
        vSemaphoreDelete(mMutex);
        mMutex = nullptr;
    }
    mInitialized = false;
}

const char* Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::LOG_DEBUG: return "DEBUG";
        case LogLevel::LOG_INFO:  return "INFO";
        case LogLevel::LOG_WARN:  return "WARN";
        case LogLevel::LOG_ERROR: return "ERROR";
        default:                  return "?????";
    }
}

const char* Logger::levelToColor(LogLevel level) {
    switch (level) {
        case LogLevel::LOG_DEBUG: return kColorYellow;
        case LogLevel::LOG_INFO:  return kColorWhite;
        case LogLevel::LOG_WARN:  return kColorMagenta;
        case LogLevel::LOG_ERROR: return kColorRed;
        default:                  return kColorReset;
    }
}

void Logger::debug(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logInternal(LogLevel::LOG_DEBUG, tag, fmt, args);
    va_end(args);
}

void Logger::info(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logInternal(LogLevel::LOG_INFO, tag, fmt, args);
    va_end(args);
}

void Logger::warn(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logInternal(LogLevel::LOG_WARN, tag, fmt, args);
    va_end(args);
}

void Logger::error(const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logInternal(LogLevel::LOG_ERROR, tag, fmt, args);
    va_end(args);
}

void Logger::logInternal(LogLevel level, const char* tag, const char* fmt, va_list args) {
    if (!mInitialized) {
        return;
    }

    char message[kMaxLogLineLen];
    vsnprintf(message, sizeof(message), fmt, args);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    char timeStr[20]; // Formato: YYYY-MM-DD HH:MM:SS
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

    if (xSemaphoreTake(mMutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    // --- stampa a console, colorata in base al livello ---
    printf("%s[%s][%-5s][%s] %s%s\n",
           levelToColor(level),
           timeStr,
           levelToString(level),
           tag != nullptr ? tag : "-",
           message,
           kColorReset);

    // --- scrittura su file, senza codici colore ANSI ---
    if (mFile != nullptr) {
        fprintf(mFile, "[%s][%-5s][%s] %s\n",
                timeStr,
                levelToString(level),
                tag != nullptr ? tag : "-",
                message);
        fflush(mFile);
        fsync(fileno(mFile));
    }

    rotateIfNeeded();

    xSemaphoreGive(mMutex);
}

// NOTA: va chiamata SEMPRE con il mutex già acquisito dal chiamante
// (vedi logInternal), non prende il lock da sola.
void Logger::rotateIfNeeded() {
    if (mFile == nullptr || mMaxFileSizeBytes == 0) {
        return;
    }

    long currentSize = ftell(mFile);
    if (currentSize < 0 || static_cast<size_t>(currentSize) < mMaxFileSizeBytes) {
        return;
    }

    rotateNow();
}

void Logger::rotateNow() {
    // Chiude il file corrente prima di rinominare qualsiasi cosa.
    if (mFile != nullptr) {
        fclose(mFile);
        mFile = nullptr;
    }

    // Fa scorrere i backup più vecchi: log.txt.2 -> log.txt.3, log.txt.1 -> log.txt.2, ecc.
    // Il più vecchio (log.txt.<maxRotatedFiles>) viene sovrascritto/perso.
    char oldPath[160];
    char newPath[160];

    for (int i = static_cast<int>(mMaxRotatedFiles) - 1; i >= 1; --i) {
        snprintf(oldPath, sizeof(oldPath), "%s.%d", mLogFilePath, i);
        snprintf(newPath, sizeof(newPath), "%s.%d", mLogFilePath, i + 1);
        // rename() su un file inesistente fallisce silenziosamente: va bene così,
        // non tutti gli indici esistono finché la rotazione non è "a regime".
        remove(newPath);
        rename(oldPath, newPath);
    }

    // log.txt -> log.txt.1
    snprintf(newPath, sizeof(newPath), "%s.1", mLogFilePath);
    remove(newPath);
    rename(mLogFilePath, newPath);

    // Riapre un log.txt vuoto e pulito.
    mFile = fopen(mLogFilePath, "w");
}
