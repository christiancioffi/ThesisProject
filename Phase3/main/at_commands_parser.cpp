#include "at_commands_parser.h"
#include "Logger.h"

namespace {

constexpr const char* kSimplestIpPattern = R"([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)";
constexpr const char* kTimePattern =
    R"(\d\d\d\d/(0[1-9]|1[0-2])/(0[1-9]|[12]\d|3[01]),([01]\d|2[0-3]):([0-5]\d):([0-5]\d)[+-]\d\d)";

struct EntryDef {
    const char* name;
    const char* command_pattern;
    std::string response_pattern; // std::string perche' alcune sono composte a runtime (concat)
};

} // namespace

ATCommandsParser::ATCommandsParser() {
    const std::vector<EntryDef> defs = {
        {"AT", R"(AT)", R"((AT\r)?\r\nOK\r\n)"},
        {"ATE0", R"(ATE0)", R"((ATE0\r)?\r\nOK\r\n)"},
        {"AT+QPOWD", R"(AT\+QPOWD(=\d)?)", R"(\r\nOK\r\n\r\nPOWERED DOWN\r\n)"},
        {"AT+CSQ", R"(AT\+CSQ)", R"(\r\n\+CSQ: \d+,\d+\r\n\r\nOK\r\n)"},
        {"AT+COPS?", R"(AT\+COPS\?)", R"(\r\n\+COPS: \d,\d,"[^"]+",\d+\r\n\r\nOK\r\n)"},
        {"AT+COPS=?", R"(AT\+COPS=\d)", R"(\r\nOK\r\n)"},
        {"AT+CGDCONT", R"(AT\+CGDCONT=\d,"IP","[^"]*")", R"(\r\nOK\r\n)"},
        {"AT+CREG?", R"(AT\+CREG\?)",
         R"(\r\n\+CREG: \d+,\d+(,"[0-9a-f]{4}","[0-9a-f]{4,7}"(,\d+)?)?\r\n\r\nOK\r\n)"},
        {"AT+CPIN?", R"(AT\+CPIN\?)", R"(\r\n\+CPIN: (.+)\r\n\r\nOK\r\n)"},
        {"AT+CFUN", R"(AT\+CFUN=[0-5](,[0-1])?)", R"(\r\nOK\r\n)"},
        {"AT+QICSGP",
         R"(AT\+QICSGP=\d(\d)?(,[1-3],".*"(,".*",".*"(,[0-3])?)?)?)", R"(\r\nOK\r\n)"},
        {"AT+QIACT?", R"(AT\+QIACT\?)",
         std::string(R"((\r\n\+QIACT: \d(\d)?,\d,\d(,")") + kSimplestIpPattern + R"(")?\r\n)?\r\nOK\r\n)"},
        {"AT+QIACT=", R"(AT\+QIACT=\d(\d)?)", R"(\r\nOK\r\n)"},
        {"AT+QIDEACT=", R"(AT\+QIDEACT=\d(\d)?)", R"(\r\nOK\r\n)"},
        {"AT+QLTS", R"(AT\+QLTS=[0-2])",
         std::string(R"(\r\n\+QLTS: ")") + kTimePattern + R"(,\d"\r\n\r\nOK\r\n)"},
        {"AT+QHTTPCFG", R"(AT\+QHTTPCFG=".+"(,\d(\d)?))", R"(\r\nOK\r\n)"},
        {"AT+QSSLCFG", R"(AT\+QSSLCFG="sni",\d(\d)?(,[0-1])?)", R"(\r\nOK\r\n)"},
        {"AT+QHTTPURL", R"(AT\+QHTTPURL=\d+(,\d+)?)", R"(\r\nCONNECT\r\n)"},
        {"AT+QHTTPGET_NOHDR", R"(AT\+QHTTPGET(=\d+)?)",
         R"(\r\nOK\r\n\r\n\+QHTTPGET: \d+(,\d+(,\d+)?)?\r\n)"},
        {"AT+QHTTPGET_HDR", R"(AT\+QHTTPGET=\d+,\d+(,\d+)?)", R"(\r\nCONNECT\r\n)"},
        {"GET_HEADERS",
         R"(GET /[a-zA-Z0-9\-._~!$&'()*+,;=:@/?#%]* HTTP/1\.1\r\n([A-Za-z0-9!#$%&'*+.^_`|~-]+:[ \t]*[^\r\n]+?\r\n)+?\r\n)",
         R"(\r\nOK\r\n\r\n\+QHTTPGET: \d+(,\d+(,\d+)?)?\r\n)"},
        {"AT+QHTTPREAD", R"(AT\+QHTTPREAD(=\d+)?)",R"(\r\nCONNECT\r\n([\s\S]*?)\r\nOK\r\n\r\n\+QHTTPREAD: \d+\r\n)"},
        {"AT+QHTTPPOST", R"(AT\+QHTTPPOST=\d+(,\d+,\d+)?)", R"(\r\nCONNECT\r\n)"},
        {"AT+QHTTPSTOP", R"(AT\+QHTTPSTOP)", R"(\r\nOK\r\n)"},
        {"URL", R"(https?://([a-zA-Z0-9-\.]+)(:[0-9]+)?(/[a-zA-Z0-9\-._~!$&'()*+,;=:@/?#%]*)?)",
         R"(\r\nOK\r\n)"},
        {"POST_BODY", R"(.*)", R"(\r\nOK\r\n\r\n\+QHTTPPOST: \d+(,\d+(,\d+)?)?\r\n)"},
    };

    entries_.reserve(defs.size());
    for (const auto& d : defs) {
        // std::regex(pattern) puo' lanciare std::regex_error se il pattern e'
        // malformato. Qui i pattern sono tutti letterali statici noti a
        // compile-time: un errore qui e' SEMPRE un bug di programmazione
        // (typo in una regex), non una condizione runtime recuperabile.
        // Logghiamo comunque quale entry ha fallito (il messaggio di
        // regex_error da solo non lo dice) prima di rilanciare: il
        // chiamante (Eg91Sender::create) decide se e come fallire in modo
        // pulito, invece di un abort() cieco qui dentro.
        try {
            entries_.push_back(Entry{
                d.name,
                std::regex(std::string("^") + d.command_pattern + "$"),
                std::regex(d.response_pattern),
            });
        } catch (const std::regex_error& e) {
            Logger::instance().error("ATCommandsParser",
                "Pattern regex non valido per l'entry '%s': %s", d.name, e.what());
            throw;
        }
    }

    try {
        error_pattern_ = std::regex(R"(\r\n(\+CME ERROR: \d+)|(ERROR)\r\n)");
    } catch (const std::regex_error& e) {
        Logger::instance().error("ATCommandsParser", "Pattern regex non valido per error_pattern_: %s", e.what());
        throw;
    }
}

ParseResult ATCommandsParser::parse_response(std::string_view command, std::string_view data) const noexcept {
    ParseResult result;
    /*
    if (command.rfind("AT+QHTTPREAD", 0) == 0) {
        // La risposta corretta di QHTTPREAD termina tipicamente con \r\nOK\r\n\r\n+QHTTPREAD: 0\r\n
        // oppure contiene la sequenza di completamento.
        // Verifichiamo se i dati contengono la chiusura dell'operazione.
        size_t ok_pos = data.find("\r\nOK\r\n");
        size_t qhttpread_end = data.find("+QHTTPREAD:");
        
        if (ok_pos != std::string_view::npos && qhttpread_end != std::string_view::npos) {
            result.matched = true;
            result.is_error = false;
            result.text = data; // Restituisce l'intero blocco letto (incluso il corpo HTML)
            return result;
        }
    }*/

    // NOTA su noexcept: questa funzione gira nel task UART dedicato
    // (listener_task), un contesto RTOS in tempo reale dove lasciar
    // scappare un'eccezione sarebbe pericoloso a prescindere da quanti
    // try/catch esistano piu' in alto nella call chain (il task non ha un
    // frame "main" con un catch ad attenderla, e comunque parse_response
    // e' dichiarata noexcept: un'eccezione che la attraversasse
    // chiamerebbe std::terminate() immediatamente, bypassando qualunque
    // catch esterno). Con le eccezioni ora abilitate, std::regex_match e
    // std::regex_search possono in teoria lanciare std::regex_error (es.
    // se il motore incontra condizioni di complessita' anomale sui dati
    // ricevuti dal modem, che non controlliamo). Le contatta qui dentro e
    // le traduce in "nessun match", lo stesso esito gia' previsto e gestito
    // dal chiamante (UartRxManager::listener_task continua ad accumulare
    // dati in attesa del prossimo evento).
    try {
        const Entry* matched_entry = nullptr;
        for (const auto& entry : entries_) {
            if (std::regex_match(command.begin(), command.end(), entry.command_pattern)) {
                matched_entry = &entry;
                break;
            }
        }
        if (matched_entry == nullptr) {
            return result; // comando non riconosciuto
        }

        std::cmatch match;
        if (std::regex_search(data.data(), data.data() + data.size(), match, matched_entry->response_pattern)) {
            result.matched = true;
            result.is_error = false;
            result.text = std::string_view(match[0].first, static_cast<size_t>(match[0].length()));
            return result;
        }

        std::cmatch error_match;
        if (std::regex_search(data.data(), data.data() + data.size(), error_match, error_pattern_)) {
            result.matched = true;
            result.is_error = true;
            result.text = std::string_view(error_match[0].first, static_cast<size_t>(error_match[0].length()));
        }

        return result;
    } catch (const std::regex_error& e) {
        Logger::instance().error("UartRxManager", "regex_error durante il parsing della risposta per '%.*s': %s",
                                  (int)command.size(), command.data(), e.what());
        return ParseResult{}; // matched=false: equivalente a "nessun match ancora", nessuna eccezione esce da qui
    } catch (...) {
        // Ultima rete di sicurezza: qualunque altra eccezione (es. std::bad_alloc
        // se il motore regex alloca internamente) non deve MAI attraversare
        // il confine di una funzione noexcept che gira nel task RTOS.
        Logger::instance().error("UartRxManager", "Eccezione sconosciuta durante il parsing della risposta per '%.*s'",
                                  (int)command.size(), command.data());
        return ParseResult{};
    }
}
