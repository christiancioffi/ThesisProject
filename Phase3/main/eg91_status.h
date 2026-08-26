#pragma once

#include <cstdint>

// Sostituisce le eccezioni: ogni operazione fallibile ritorna un Eg91Status
// (o un semplice bool per i casi binari successo/fallimento con log gia'
// emesso internamente). [[nodiscard]] sulle funzioni che lo restituiscono
// forza il chiamante a controllarlo, dato che non c'e' piu' un'eccezione
// a impedire di ignorare un errore.
enum class Eg91Status : uint8_t {
    Ok = 0,
    Timeout,        // Nessuna risposta entro il tempo massimo
    AtError,        // Il modulo ha risposto con ERROR / +CME ERROR
    InvalidConfig,  // Configurazione applicativa non valida
    ParseError,     // Risposta AT ricevuta ma non nel formato atteso
    HttpError,      // Status HTTP fuori range 2xx
    NotEnabled,     // Operazione richiesta a modulo non abilitato
    HardwareError,  // Fallimento driver UART/GPIO
    Busy,           // Impossibile ottenere il lock di trasmissione in tempo
};

constexpr const char* to_string(Eg91Status s) noexcept {
    switch (s) {
        case Eg91Status::Ok: return "Ok";
        case Eg91Status::Timeout: return "Timeout";
        case Eg91Status::AtError: return "AtError";
        case Eg91Status::InvalidConfig: return "InvalidConfig";
        case Eg91Status::ParseError: return "ParseError";
        case Eg91Status::HttpError: return "HttpError";
        case Eg91Status::NotEnabled: return "NotEnabled";
        case Eg91Status::HardwareError: return "HardwareError";
        case Eg91Status::Busy: return "Busy";
    }
    return "Unknown";
}
