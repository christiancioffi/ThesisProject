#include "eg91_sender.h"

#include <regex>
#include <charconv>
#include <algorithm>
#include <new>       // std::bad_alloc, catturato in create()
#include <stdexcept> // std::exception, catturato in create()
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Logger.h"

static const char* TAG = "Eg91Sender";

namespace {

//const std::regex kUrlRegex(R"(https?://([a-zA-Z0-9-\.]+)(:[0-9]+)?(/[a-zA-Z0-9\-._~!$&'()*+,;=:@/?#%]*)?)");
const std::regex kUrlRegex(R"(https?://([^/:]+)(?::(\d+))?(/.*)?)");

std::string_view extract_cmd_key(std::string_view command) noexcept {
    size_t end = command.find_first_of("=?");
    return (end == std::string_view::npos) ? command : command.substr(0, end);
}

// Sostituisce std::stoi: con le eccezioni disattivate, stoi su input malformato
// chiamerebbe abort(). std::from_chars non lancia mai, ritorna un error_code.
bool parse_int(std::string_view sv, int& out) noexcept {
    auto res = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return res.ec == std::errc();
}

// Estrae host/path da un URL gia' validato con kUrlRegex, senza eccezioni.
struct UrlParts {
    bool valid = false;
    std::string host;
    std::string path;
};

UrlParts split_url(std::string_view url) {
    UrlParts parts;
    try {
        std::cmatch m;
        if (!std::regex_search(url.data(), url.data() + url.size(), m, kUrlRegex)) {
            return parts;
        }
        parts.valid = true;
        parts.host.assign(m[1].first, m[1].second);
        if (m[3].matched && m[3].length() > 0) {
            parts.path.assign(m[3].first, m[3].second);
        } else {
            parts.path = "/";
        }
        return parts;
    } catch (const std::regex_error& e) {
        Logger::instance().error("Eg91Sender", "regex_error nel parsing dell'URL: %s", e.what());
        return UrlParts{}; // valid=false: stesso esito di un URL malformato, gestito dai chiamanti esistenti
    }
}

} // namespace

std::unique_ptr<Eg91Sender> Eg91Sender::create(
    uart_port_t uart_num, gpio_num_t tx, gpio_num_t rx,
    gpio_num_t power_pin, gpio_num_t reset_pin, const Eg91Config& config) {

    // Con le eccezioni ora abilitate, "new Eg91Sender(...)" non e' piu'
    // garantito privo di operazioni fallibili come diceva il commento
    // originale: il costruttore inizializza rx_manager_, il cui membro
    // ATCommandsParser compila una dozzina di std::regex nel SUO
    // costruttore, e puo' lanciare std::regex_error (pattern malformato:
    // sarebbe un bug, ma va comunque intercettato qui invece di far
    // crashare l'intero firmware in modo non controllato). init() a sua
    // volta concatena diverse std::string per costruire i comandi AT, che
    // in teoria potrebbero sollevare std::bad_alloc su un target con RAM
    // limitata come l'ESP32. create() e' l'unico punto ragionevole per
    // contenere entrambi i casi, perche' e' l'unico chiamante che puo'
    // ancora tornare nullptr in modo pulito (il contratto pubblico di
    // Eg91Sender resta "mai un throw", vedi commenti nell'header).
    try {
        // new + costruttore privato "leggero": nessuna operazione fallibile qui dentro.
        auto sender = std::unique_ptr<Eg91Sender>(new Eg91Sender(uart_num, tx, rx, power_pin, reset_pin, config));

        if (!sender->init()) {
            Logger::instance().error(TAG, "EG91 initialization failed");
            return nullptr; // sender viene distrutto qui: il distruttore chiama deinit()
        }
        return sender;
    } catch (const std::regex_error& e) {
        Logger::instance().error(TAG, "Inizializzazione fallita: pattern regex non valido (%s)", e.what());
        return nullptr;
    } catch (const std::bad_alloc& e) {
        Logger::instance().error(TAG, "Inizializzazione fallita: memoria esaurita (%s)", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        Logger::instance().error(TAG, "Inizializzazione fallita: eccezione imprevista (%s)", e.what());
        return nullptr;
    } catch (...) {
        Logger::instance().error(TAG, "Inizializzazione fallita: eccezione sconosciuta");
        return nullptr;
    }
}

Eg91Sender::Eg91Sender(uart_port_t uart_num, gpio_num_t tx, gpio_num_t rx,
                        gpio_num_t power_pin, gpio_num_t reset_pin, const Eg91Config& config)
    : uart_num_(uart_num), uart_tx_pin_(tx), uart_rx_pin_(rx),
      lte_power_pin_(power_pin), lte_reset_pin_(reset_pin), config_(config) {}

Eg91Sender::~Eg91Sender() {
    deinit();
}

bool Eg91Sender::init() {
    Logger::instance().info(TAG, "Initializing EG91 sender module...");

    if (config_.auth_type != "passwd" && config_.auth_type != "certs") {
        Logger::instance().error(TAG, "Invalid config value: auth_type");
        return false;
    }

    gpio_set_direction(lte_power_pin_, GPIO_MODE_OUTPUT);
    gpio_set_direction(lte_reset_pin_, GPIO_MODE_OUTPUT);

    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate = 115200;
    uart_cfg.data_bits = UART_DATA_8_BITS;
    uart_cfg.parity = UART_PARITY_DISABLE;
    uart_cfg.stop_bits = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    if (uart_param_config(uart_num_, &uart_cfg) != ESP_OK ||
        uart_set_pin(uart_num_, uart_tx_pin_, uart_rx_pin_,
                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        Logger::instance().error(TAG, "UART pin/param configuration failed");
        return false;
    }

    QueueHandle_t uart_queue;
    if (uart_driver_install(uart_num_, kMaxBufferSize * 2, kMaxBufferSize * 2, 20, &uart_queue, 0) != ESP_OK) {
        Logger::instance().error(TAG, "UART driver install failed");
        return false;
    }
    uart_installed_ = true;

    if (!rx_manager_.init(uart_num_, uart_queue)) {
        Logger::instance().error(TAG, "UART RX manager initialization failed");
        return false;
    }

    if (!power_on()) {
        Logger::instance().error(TAG, "Failed to power on EG91 module");
        return false;
    }
    powered_on_ = true;

    if (!enable()) {
        Logger::instance().error(TAG, "Failed to enable EG91 sender");
        return false;
    }

    return true;
}

void Eg91Sender::deinit() {
    disable();
    // shut_down() tenta AT+QPOWD, quindi va chiamata MENTRE l'UART e' ancora
    // installata (e prima di rx_manager_.deinit()): l'ordine originale la
    // chiamava dopo uart_driver_delete(), rendendo AT+QPOWD impossibile.
    if (powered_on_) {
        if (!shut_down()) {
            Logger::instance().warn(TAG, "EG91 shutdown did not complete as expected; continuing cleanup anyway");
        }
        powered_on_ = false;
    }
    rx_manager_.deinit();
    if (uart_installed_) {
        uart_driver_delete(uart_num_);
        uart_installed_ = false;
    }
}

bool Eg91Sender::is_module_on() {
    // Probe non distruttivo: un modulo davvero acceso risponde sempre a un
    // "AT" (indipendentemente da ATE0/registrazione rete/PDP, che vengono
    // dopo in enable()). quiet_on_timeout=true perche' un mancato responso
    // qui e' un esito atteso quando il modulo e' spento, non un errore.
    std::string response;
    return send_command("AT", response, -1, /*quiet_on_timeout=*/true) == Eg91Status::Ok;
}

bool Eg91Sender::power_on() {
    Logger::instance().info(TAG, "Powering on EG91 module...");

    if (!uart_installed_) {
        Logger::instance().error(TAG, "Cannot power on: UART not installed");
        return false;
    }

    // PWRKEY COMMUTA lo stato del modulo (spento->acceso o acceso->spento
    // a seconda della durata dell'impulso), non lo forza in uno stato
    // preciso. Se il modulo era gia' acceso (sessione precedente non
    // spenta correttamente, VBAT mai rimossa, ecc.) un impulso pensato per
    // "accenderlo" lo spegnerebbe invece: e' esattamente quanto si vede in
    // log ("POWERED DOWN" subito dopo il presunto power-on). Verifichiamo
    // sempre lo stato reale prima di decidere se pulsare.
    if (is_module_on()) {
        Logger::instance().info(TAG, "EG91 module was already powered on, skipping PWRKEY pulse");
        return true;
    }

    gpio_set_level(lte_power_pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(kPowerOnPulseMs));
    gpio_set_level(lte_power_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(kPowerOnSettleMs));

    if (!is_module_on()) {
        Logger::instance().error(TAG, "EG91 module did not respond after power-on pulse");
        return false;
    }

    Logger::instance().info(TAG, "EG91 module powered on successfully");
    return true;
}

bool Eg91Sender::shut_down() {
    Logger::instance().info(TAG, "Shutting down EG91 module...");

    if (!uart_installed_) {
        // Senza UART non possiamo ne' verificare lo stato ne' tentare
        // AT+QPOWD: ricorriamo direttamente all'impulso hardware,
        // assumendo lo scenario peggiore (modulo acceso).
        gpio_set_level(lte_power_pin_, 1);
        vTaskDelay(pdMS_TO_TICKS(kShutdownPulseMs));
        gpio_set_level(lte_power_pin_, 0);
        vTaskDelay(pdMS_TO_TICKS(kShutdownSettleMs));
        Logger::instance().info(TAG, "EG91 module shut down successfully");
        return true;
    }

    // Stessa logica di power_on(): PWRKEY commuta, non forza. Se il modulo
    // e' gia' spento, non tocchiamo PWRKEY (altrimenti lo riaccenderemmo).
    if (!is_module_on()) {
        Logger::instance().info(TAG, "EG91 module was already powered off");
        return true;
    }

    // Via "sicura" raccomandata da Quectel: prova prima AT+QPOWD. Se il
    // modulo non risponde, ricadi sull'impulso hardware su PWRKEY — che a
    // questo punto sappiamo per certo essere applicato a un modulo acceso
    // (verificato sopra), quindi lo spegnera' correttamente invece di
    // rischiare di riaccenderlo.
    std::string response;
    bool graceful = (send_command("AT+QPOWD", response) == Eg91Status::Ok);
    if (!graceful) {
        Logger::instance().warn(TAG, "AT+QPOWD failed or timed out, falling back to PWRKEY pulse");
        gpio_set_level(lte_power_pin_, 1);
        vTaskDelay(pdMS_TO_TICKS(kShutdownPulseMs));
        gpio_set_level(lte_power_pin_, 0);
    }

    // In entrambi i casi il modulo esegue una procedura di power-down che
    // puo' richiedere fino a ~30s: attendere meno rischia di interromperla
    // a meta' (rischio di danneggiare la flash interna, per nota esplicita
    // del datasheet).
    vTaskDelay(pdMS_TO_TICKS(kShutdownSettleMs));

    if (is_module_on()) {
        Logger::instance().error(TAG, "EG91 module did not power off as expected (still responsive)");
        return false;
    }

    Logger::instance().info(TAG, "EG91 module shut down successfully");
    return true;
}

bool Eg91Sender::hard_reset() {
    Logger::instance().info(TAG, "Resetting EG91 module...");

    // RESET_N e' un reset "warm" di un modulo GIA' acceso (vedi nota
    // Quectel nell'header): non serve/non ha senso su un modulo spento, e
    // chiamarlo per "recuperare" un modulo che in realta' e' stato spento
    // per errore (vedi power_on()) non lo riaccende affatto — e' proprio
    // quanto successo in log (due timeout su "AT" dopo un reset che non
    // poteva funzionare, perche' il modulo era gia' spento).
    if (uart_installed_ && !is_module_on()) {
        Logger::instance().warn(TAG, "EG91 module is powered off, RESET_N has no effect; use power_on() instead");
        return false;
    }

    gpio_set_level(lte_reset_pin_, 1);
    vTaskDelay(pdMS_TO_TICKS(kResetPulseMs));
    gpio_set_level(lte_reset_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(kResetSettleMs));

    if (!is_module_on()) {
        Logger::instance().error(TAG, "EG91 module did not respond after hardware reset");
        return false;
    }

    Logger::instance().info(TAG, "EG91 module reset successfully");
    return true;
}

uint32_t Eg91Sender::resp_timeout_ms(std::string_view cmd_key, uint32_t fallback_ms) const {
    auto it = max_resp_time_.find(cmd_key);
    return (it != max_resp_time_.end()) ? it->second : fallback_ms;
}

Eg91Status Eg91Sender::send_command(std::string_view command, std::string& out_response,
                                     int wait_time_ms, bool quiet_on_timeout) {
    Logger::instance().info(TAG, "Sending AT command: %.*s", (int)command.size(), command.data());

    uint32_t wait_time = (wait_time_ms > 0)
        ? static_cast<uint32_t>(wait_time_ms)
        : resp_timeout_ms(extract_cmd_key(command), kMinWaitTimeMs);
    wait_time = std::max(wait_time, kMinWaitTimeMs);

    // RAII: apre la transazione per l'INTERA durata del comando e la chiude
    // automaticamente all'uscita dallo scope, in ogni percorso (successo,
    // errore di parsing, timeout) — sostituisce il try/finally che si
    // userebbe con le eccezioni. Il main task e' l'unico chiamante di
    // send_command(), quindi qui non c'e' mai contesa da attendere: se
    // acquired() e' false e' solo perche' il chiamante ha (erroneamente)
    // annidato due transazioni senza il release() intermedio.
    Eg91AtTransaction txn(rx_manager_, command);
    if (!txn.acquired()) {
        Logger::instance().error(TAG, "Could not open UART transaction (nested acquire without release)");
        return Eg91Status::Busy;
    }

    std::string to_send;
    to_send.reserve(command.size() + 1);
    to_send.append(command).push_back('\r');

    uart_write_bytes(uart_num_, to_send.data(), to_send.size());
    uart_wait_tx_done(uart_num_, pdMS_TO_TICKS(1000));

    AtResponse resp;
    if (!rx_manager_.wait_response(wait_time, resp)) {
        if (quiet_on_timeout) {
            Logger::instance().debug(TAG, "No response to '%.*s' (timeout, expected for a state probe)",
                     (int)command.size(), command.data());
        } else {
            Logger::instance().error(TAG, "Timeout occurred while sending command '%.*s'", (int)command.size(), command.data());
        }
        return Eg91Status::Timeout;
    }

    out_response = std::move(resp.text);
    if (resp.is_error) {
        Logger::instance().error(TAG, "Received an ERROR response: %s", out_response.c_str());
        return Eg91Status::AtError;
    }

    Logger::instance().info(TAG, "Received response: %s", out_response.c_str());
    return Eg91Status::Ok;
}

bool Eg91Sender::check_network_registration() {

    std::string response;

    if (send_command("AT+CREG?", response) != Eg91Status::Ok) {
        network_registered_ = false;
        return false;
    }

    try {
        std::cmatch m;
        std::regex creg_re(R"(\+CREG:\s*(\d+),(\d+))");
        if (std::regex_search(response.c_str(), response.c_str() + response.size(), m, creg_re)) {
            int status = 0;
            if (parse_int(std::string_view(m[2].first, m[2].length()), status)) {
                network_registered_ = (status == 1 || status == 5); // home o roaming
                return network_registered_;
            }
        }
    } catch (const std::regex_error& e) {
        Logger::instance().error(TAG, "regex_error nel parsing di AT+CREG?: %s", e.what());
    }

    network_registered_ = false;
    return false;
}

bool Eg91Sender::enable() {
    if (enabled_) return true;
    Logger::instance().info(TAG, "Enabling EG91 sender module...");

    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        bool attempt_ok = true;
        // Distingue "il modulo non risponde nemmeno ad AT" (giustifica un
        // reset hardware) da "il modulo risponde ma un passo successivo e'
        // fallito" (SIM/rete/PDP: un power-cycle regolare basta e disturba
        // meno, vedi il ramo di retry sotto).
        bool module_unresponsive = false;
        std::string response;

        rx_manager_.clear_state();

        if (send_command("AT", response) != Eg91Status::Ok) {
            attempt_ok = false;
            module_unresponsive = true;
        } else if (send_command("ATE0", response) != Eg91Status::Ok) {
            attempt_ok = false;
        }

        if (attempt_ok) {
            if (send_command("AT+CPIN?", response) != Eg91Status::Ok ||
                response.find("READY") == std::string::npos) {
                Logger::instance().error(TAG, "SIM not ready");
                attempt_ok = false;
            }
        }

        // AGGIUNTA: Configura APN e seleziona la rete automatica PRIMA della registrazione
        if (attempt_ok) {
            std::string apn_cmd = "AT+QICSGP=" + std::to_string(kPdpCtxId) + ",1,\"" + config_.apn + "\",\"\",\"\",1";
            if (send_command(apn_cmd, response) != Eg91Status::Ok ||
                send_command("AT+COPS=0", response) != Eg91Status::Ok) {
                Logger::instance().error(TAG, "Failed to set APN or auto operator selection");
                attempt_ok = false;
            }
        }

        // CICLO DI ATTESA ROBUSTO (fino a ~30 secondi di tentativi per il roaming)
        if (attempt_ok) {
            Logger::instance().info(TAG, "Waiting for network registration...");
            bool registered = false;
            for (int retry = 0; retry < 10; ++retry) {
                if (check_network_registration()) {
                    registered = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(3000)); // Controlla ogni 3 secondi
            }

            if (!registered) {
                Logger::instance().error(TAG, "Network registration failed (timeout reached)");
                attempt_ok = false;
            }
        }

        if (attempt_ok) {
            Logger::instance().info(TAG, "Successfully connected to the cellular network");
            if (activate_pdp_context() != Eg91Status::Ok) {
                attempt_ok = false;
            }
        }

        if (attempt_ok) {
            Logger::instance().info(TAG, "EG91 sender module enabled successfully");
            enabled_ = true;
            return true;
        }

        if (attempt < kMaxRetries - 1) {
            if (module_unresponsive) {
                // Estrema ratio, come da datasheet Quectel: il modulo non
                // risponde nemmeno al comando "AT" piu' semplice, quindi
                // neppure AT+QPOWD (tentato dentro shut_down()) puo'
                // funzionare in queste condizioni. RESET_N e' l'unica via
                // rimasta per un modulo genuinamente bloccato.
                Logger::instance().error(TAG, "Enable attempt %d failed (module unresponsive), retrying after hardware reset...", attempt + 1);
                if (!hard_reset()) {
                    Logger::instance().error(TAG, "Hardware reset did not bring the module back up");
                }
            } else {
                // Il modulo risponde ai comandi AT: un fallimento qui
                // (SIM, registrazione di rete, PDP) non e' un modulo
                // "bloccato". Un power-cycle regolare (spegnimento anche
                // per via AT+QPOWD + riaccensione) e' sufficiente e meno
                // invasivo di un reset hardware, che Quectel riserva
                // esplicitamente al caso di modulo non rispondente.
                Logger::instance().error(TAG, "Enable attempt %d failed, retrying after power-cycle...", attempt + 1);
                if (!shut_down() || !power_on()) {
                    Logger::instance().error(TAG, "Power-cycle did not complete as expected");
                }
            }
        } else {
            Logger::instance().error(TAG, "EG91 enabling failed after %d attempts", kMaxRetries);
        }
    }
    return false;
}

Eg91Status Eg91Sender::activate_pdp_context() {
    std::string response;

    std::string apn_cmd = "AT+QICSGP=" + std::to_string(kPdpCtxId) + ",1,\"" + config_.apn + "\",\"\",\"\",1";
    if (send_command(apn_cmd, response) != Eg91Status::Ok) return Eg91Status::AtError;

    if (send_command("AT+QIACT?", response) != Eg91Status::Ok) return Eg91Status::AtError;

    if (send_command("AT+QIACT=" + std::to_string(kPdpCtxId), response) != Eg91Status::Ok) {
        return Eg91Status::AtError;
    }

    if (send_command("AT+QIACT?", response) != Eg91Status::Ok) return Eg91Status::AtError;

    return Eg91Status::Ok;
}

Eg91Status Eg91Sender::deactivate_pdp_context() {
    std::string response;
    if (send_command("AT+QIDEACT=" + std::to_string(kPdpCtxId), response) != Eg91Status::Ok) {
        return Eg91Status::AtError;
    }
    return Eg91Status::Ok;
}

void Eg91Sender::disable() {
    if (!enabled_) {
        Logger::instance().info(TAG, "EG91 sender module already disabled");
        return;
    }
    Logger::instance().info(TAG, "Disabling EG91 sender module");
    if (deactivate_pdp_context() != Eg91Status::Ok) {
        Logger::instance().error(TAG, "Error disabling EG91 sender module (deactivation failed)");
    }
    enabled_ = false;
    Logger::instance().info(TAG, "EG91 sender module disabled");
}

bool Eg91Sender::get_time(std::string& out_time, std::string& out_dst) {
    std::string response;
    if (send_command("AT+QLTS=2", response) != Eg91Status::Ok) return false;

    try {
        std::cmatch m;
        std::regex qlts_re(R"qlts(\+QLTS: "([^"]+)")qlts");
        if (!std::regex_search(response.c_str(), response.c_str() + response.size(), m, qlts_re)) {
            return false;
        }

        std::string_view time_str(m[1].first, m[1].length()); // "2026/01/22,12:35:25+04,1"
        size_t last_comma = time_str.rfind(',');
        if (last_comma == std::string_view::npos) return false;

        out_time.assign(time_str.substr(0, last_comma));
        out_dst.assign(time_str.substr(last_comma + 1));
        return true;
    } catch (const std::regex_error& e) {
        Logger::instance().error(TAG, "regex_error nel parsing di AT+QLTS: %s", e.what());
        return false;
    }
}

void Eg91Sender::close_https_connection() {
    Logger::instance().info(TAG, "Closing HTTPS connection");
    std::string response;
    if (send_command("AT+QHTTPSTOP", response) != Eg91Status::Ok) {
        Logger::instance().error(TAG, "Error occurred while closing HTTPS connection");
    }
}

std::string Eg91Sender::clean_http_payload(std::string_view raw) {
    std::string cleaned(raw);
    for (std::string_view tok : {"CONNECT", "OK", "+QHTTPREAD: 0"}) {
        size_t p;
        while ((p = cleaned.find(tok)) != std::string::npos) cleaned.erase(p, tok.size());
    }
    size_t start = cleaned.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = cleaned.find_last_not_of(" \t\r\n");
    return cleaned.substr(start, end - start + 1);
}

Eg91Status Eg91Sender::https_get(std::string_view url, HttpsResult& out, int timeout_s,
                                  const HeaderList* headers) {
    if (!enabled_) return Eg91Status::NotEnabled;
    Logger::instance().info(TAG, "Starting HTTPS GET request to: %.*s", (int)url.size(), url.data());

    // timeout_s=0 e' simbolico: se il chiamante non ha fissato un valore
    // esplicito, ogni comando risolve il proprio timeout dal timeout massimo
    // di risposta noto per QUEL comando specifico (max_resp_time_), invece
    // di condividere un unico numero indovinato per l'intera richiesta. Se
    // invece il chiamante ha passato un valore > 0, quello vale per tutti i
    // comandi, come nel comportamento precedente.
    auto resolve_timeout_s = [this, timeout_s](std::string_view cmd_key) -> int {
        return (timeout_s != 0) ? timeout_s
                                 : static_cast<int>(resp_timeout_ms(cmd_key, kMinWaitTimeMs) / 1000);
    };

    std::string response;
    auto fail = [this](Eg91Status s) { close_https_connection(); return s; };

    if (send_command("AT+QHTTPCFG=\"contextid\"," + std::to_string(kPdpCtxId), response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);

    if (headers) {
        if (send_command("AT+QHTTPCFG=\"requestheader\",1", response) != Eg91Status::Ok)
            return fail(Eg91Status::AtError);
    }

    if (send_command("AT+QHTTPCFG=\"sslctxid\"," + std::to_string(kHttpsSslCtxId), response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);
    if (send_command("AT+QSSLCFG=\"sni\"," + std::to_string(kHttpsSslCtxId) + ",1", response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);

    const int url_timeout_s = resolve_timeout_s("AT+QHTTPURL");
    if (send_command("AT+QHTTPURL=" + std::to_string(url.size()) + "," + std::to_string(url_timeout_s), response)
        != Eg91Status::Ok)
        return fail(Eg91Status::AtError);
    if (send_command(url, response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);

    std::string header_str;
    if (headers) {
        UrlParts parts = split_url(url);
        if (!parts.valid) return fail(Eg91Status::InvalidConfig);

        header_str = "GET " + parts.path + " HTTP/1.1\r\n";
        header_str += "Host: " + parts.host + "\r\n";
        header_str += "User-Agent: QUECTEL_MODULE\r\n";
        header_str += "Accept: */*\r\n";
        header_str += "Content-Length: 0\r\n";
        for (const auto& h : *headers) {
            header_str.append(h.name).append(": ").append(h.value).append("\r\n");
        }
        header_str += "\r\n";
    }

    const int get_timeout_s = resolve_timeout_s("AT+QHTTPGET");
    std::string qhttpget_cmd = headers
        ? "AT+QHTTPGET=" + std::to_string(get_timeout_s) + "," + std::to_string(header_str.size())
        : "AT+QHTTPGET=" + std::to_string(get_timeout_s);

    if (send_command(qhttpget_cmd, response) != Eg91Status::Ok) return fail(Eg91Status::AtError);

    if (headers) {
        // Timeout coerente con la reale durata della richiesta HTTP, non il
        // default minimo di send_command.
        if (send_command(header_str, response, get_timeout_s * 1000) != Eg91Status::Ok) {
            return fail(Eg91Status::AtError);
        }
    }

    int http_status = 0;
    try {
        std::cmatch m;
        std::regex status_re(R"(\+QHTTPGET: \d+,(\d+))");
        if (!std::regex_search(response.c_str(), response.c_str() + response.size(), m, status_re)) {
            return fail(Eg91Status::ParseError);
        }
        if (!parse_int(std::string_view(m[1].first, m[1].length()), http_status)) {
            return fail(Eg91Status::ParseError);
        }
    } catch (const std::regex_error& e) {
        Logger::instance().error(TAG, "regex_error nel parsing dello status di AT+QHTTPGET: %s", e.what());
        return fail(Eg91Status::ParseError);
    }
    if (http_status < 200 || http_status >= 300) {
        Logger::instance().error(TAG, "HTTPS GET request failed (STATUS CODE: %d)", http_status);
        out.http_status = http_status;
        return fail(Eg91Status::HttpError);
    }
    Logger::instance().info(TAG, "HTTPS GET request succeeded with status code: %d", http_status);

    const int read_timeout_s = resolve_timeout_s("AT+QHTTPREAD");
    if (send_command("AT+QHTTPREAD=" + std::to_string(read_timeout_s), response) != Eg91Status::Ok) {
        return fail(Eg91Status::AtError);
    }
    close_https_connection();

    out.http_status = http_status;
    out.body = clean_http_payload(response);
    Logger::instance().info(TAG, "HTTPS GET request completed successfully");
    return Eg91Status::Ok;
}

Eg91Status Eg91Sender::send_https_post_body(std::string_view body, std::string& out_response, int timeout_s) {
    // timeout_s=0 e' simbolico: risolto qui su "AT+QHTTPPOST", come nelle
    // altre funzioni che accettano timeout_s.
    if (timeout_s == 0) {
        timeout_s = static_cast<int>(resp_timeout_ms("AT+QHTTPPOST", kMinWaitTimeMs) / 1000);
    }

    uint32_t wait_time = std::max<uint32_t>(
        resp_timeout_ms("AT+QHTTPPOST", static_cast<uint32_t>(timeout_s) * 1000), kMinWaitTimeMs);

    Eg91AtTransaction txn(rx_manager_, "POST_BODY");
    if (!txn.acquired()) return Eg91Status::Busy;

    Logger::instance().debug(TAG, "Sending POST body data...");
    constexpr size_t kChunkSize = 128;
    for (size_t i = 0; i < body.size(); i += kChunkSize) {
        size_t n = std::min(kChunkSize, body.size() - i);
        uart_write_bytes(uart_num_, body.data() + i, n);
        uart_wait_tx_done(uart_num_, pdMS_TO_TICKS(1000));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    Logger::instance().info(TAG, "Body sent!");

    AtResponse resp;
    if (!rx_manager_.wait_response(wait_time, resp)) {
        Logger::instance().error(TAG, "Timeout occurred while sending HTTPS POST body");
        return Eg91Status::Timeout;
    }

    out_response = std::move(resp.text);
    if (resp.is_error) {
        Logger::instance().error(TAG, "Received an ERROR response: %s", out_response.c_str());
        return Eg91Status::AtError;
    }
    return Eg91Status::Ok;
}

Eg91Status Eg91Sender::https_post(std::string_view url, std::string_view body, HttpsResult& out,
                                   int timeout_s, std::string_view content_type,
                                   const HeaderList* headers) {
    if (!enabled_) return Eg91Status::NotEnabled;
    Logger::instance().info(TAG, "Starting HTTPS POST request to: %.*s", (int)url.size(), url.data());

    // timeout_s=0 e' simbolico: vedi commento analogo in https_get().
    auto resolve_timeout_s = [this, timeout_s](std::string_view cmd_key) -> int {
        return (timeout_s != 0) ? timeout_s
                                 : static_cast<int>(resp_timeout_ms(cmd_key, kMinWaitTimeMs) / 1000);
    };

    auto fail = [this](Eg91Status s) { close_https_connection(); return s; };

    if (http_content_types_.find(content_type) == http_content_types_.end()) {
        Logger::instance().error(TAG, "Unsupported content type: %.*s", (int)content_type.size(), content_type.data());
        return Eg91Status::InvalidConfig;
    }

    std::string response;
    if (send_command("AT+QHTTPCFG=\"contextid\"," + std::to_string(kPdpCtxId), response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);

    if (headers) {
        if (send_command("AT+QHTTPCFG=\"requestheader\",1", response) != Eg91Status::Ok)
            return fail(Eg91Status::AtError);
    } else {
        int ct = http_content_types_.at(content_type);
        if (send_command("AT+QHTTPCFG=\"contenttype\"," + std::to_string(ct), response) != Eg91Status::Ok)
            return fail(Eg91Status::AtError);
    }

    if (send_command("AT+QHTTPCFG=\"sslctxid\"," + std::to_string(kHttpsSslCtxId), response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);
    if (send_command("AT+QSSLCFG=\"sni\"," + std::to_string(kHttpsSslCtxId) + ",1", response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);
    
    const int url_timeout_s = resolve_timeout_s("AT+QHTTPURL");
    if (send_command("AT+QHTTPURL=" + std::to_string(url.size()) + "," + std::to_string(url_timeout_s), response)
        != Eg91Status::Ok)
        return fail(Eg91Status::AtError);
    if (send_command(url, response) != Eg91Status::Ok)
        return fail(Eg91Status::AtError);

    std::string full_body(body);
    if (headers) {
        UrlParts parts = split_url(url);
        if (!parts.valid) return fail(Eg91Status::InvalidConfig);

        std::string header_str = "POST " + parts.path + " HTTP/1.1\r\n";
        header_str += "Host: " + parts.host + "\r\n";
        header_str += "User-Agent: QUECTEL_MODULE\r\n";
        header_str += "Accept: */*\r\n";
        header_str += "Content-Type: " + std::string(content_type) + "\r\n";
        header_str += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        for (const auto& h : *headers) {
            if (h.name == "Content-Type") continue; // evita duplicati
            header_str.append(h.name).append(": ").append(h.value).append("\r\n");
        }
        header_str += "\r\n";
        full_body = header_str + full_body;
    }

    const int post_timeout_s = resolve_timeout_s("AT+QHTTPPOST");
    if (send_command("AT+QHTTPPOST=" + std::to_string(full_body.size()) + "," +
                      std::to_string(post_timeout_s) + "," + std::to_string(post_timeout_s), response, 10000)
        != Eg91Status::Ok)
        return fail(Eg91Status::AtError);

    Eg91Status post_status = send_https_post_body(full_body, response, post_timeout_s);
    if (post_status != Eg91Status::Ok) return fail(post_status);

    int http_status = 0;
    try {
        std::cmatch m;
        std::regex status_re(R"(\+QHTTPPOST: \d+,(\d+))");
        if (!std::regex_search(response.c_str(), response.c_str() + response.size(), m, status_re)) {
            return fail(Eg91Status::ParseError);
        }
        if (!parse_int(std::string_view(m[1].first, m[1].length()), http_status)) {
            return fail(Eg91Status::ParseError);
        }
    } catch (const std::regex_error& e) {
        Logger::instance().error(TAG, "regex_error nel parsing dello status di AT+QHTTPPOST: %s", e.what());
        return fail(Eg91Status::ParseError);
    }
    if (http_status < 200 || http_status >= 300) {
        Logger::instance().error(TAG, "HTTPS POST request failed (STATUS CODE: %d)", http_status);
        out.http_status = http_status;
        return fail(Eg91Status::HttpError);
    }
    Logger::instance().info(TAG, "HTTPS POST request succeeded with status code: %d", http_status);

    const int read_timeout_s = resolve_timeout_s("AT+QHTTPREAD");
    if (send_command("AT+QHTTPREAD=" + std::to_string(read_timeout_s), response) != Eg91Status::Ok) {
        return fail(Eg91Status::AtError);
    }
    close_https_connection();

    out.http_status = http_status;
    out.body = clean_http_payload(response);
    Logger::instance().info(TAG, "HTTPS POST request completed successfully");
    return Eg91Status::Ok;
}