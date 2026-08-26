#pragma once

#include <regex>
#include <string_view>
#include <vector>

// Risultato del parsing di una risposta AT.
// text punta ALL'INTERNO del buffer "data" passato a parse_response:
// e' valido solo finche' quel buffer rimane valido (nessuna allocazione,
// nessuna copia — a differenza della versione precedente basata su std::string).
struct ParseResult {
    bool matched = false;
    bool is_error = false;
    std::string_view text;
};

// Equivalente di ATCommandsParser, ma con comando+risposta accorpati in una
// singola "entry" (invece di due contenitori separati come in Python: una
// lista ordinata di pattern comando + un dizionario risposte tenuto in sync
// per nome). Elimina l'hashing di stringhe e il doppio lookup ad ogni parse.
//
// Immutabile dopo la costruzione -> thread-safe per letture concorrenti
// (std::regex_search su un oggetto regex costante e' rientrante).
class ATCommandsParser {
public:
    ATCommandsParser();

    ParseResult parse_response(std::string_view command, std::string_view data) const noexcept;

private:
    struct Entry {
        const char* name;           // solo per log/debug, string literal statica
        std::regex command_pattern; // ancorato ^...$, come nella versione Python
        std::regex response_pattern;
    };

    std::vector<Entry> entries_;
    std::regex error_pattern_;
};
