#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>

#include "driver/uart.h"
#include "driver/gpio.h"

#include "uart_rx_manager.h"
#include "at_commands_parser.h"
#include "eg91_status.h"

struct Eg91Config {
    std::string apn;
    std::string endpoint;
    int port = 0;
    std::string client_id;
    std::string pub_topic;
    std::string sub_topic;
    std::string auth_type; // "passwd" oppure "certs"
    std::string mqtt_user;
    std::string mqtt_pass;
    std::string ca;
    std::string cert;
    std::string key;
};

struct HttpsResult {
    std::string body;
    int http_status = 0;
};

// Header HTTP custom passati per riferimento leggero (nessuna copia di stringhe
// finche' non serve costruire la richiesta), al posto di uno std::map<string,string>.
struct HttpHeader {
    std::string_view name;
    std::string_view value;
};
using HeaderList = std::vector<HttpHeader>;

// Equivalente di Eg91Sender, senza eccezioni:
//  - creazione tramite create() statico (il costruttore non puo' fallire in
//    modo pulito senza eccezioni, quindi resta privato e "leggero");
//  - ogni operazione fallibile ritorna Eg91Status (o riempie out via
//    riferimento e ritorna bool), mai un throw.
class Eg91Sender {
public:
    [[nodiscard]] static std::unique_ptr<Eg91Sender> create(
        uart_port_t uart_num,
        gpio_num_t uart_tx_pin, gpio_num_t uart_rx_pin,
        gpio_num_t lte_power_pin, gpio_num_t lte_reset_pin,
        const Eg91Config& config);

    ~Eg91Sender();

    Eg91Sender(const Eg91Sender&) = delete;
    Eg91Sender& operator=(const Eg91Sender&) = delete;

    // timeout_s = 0 e' un valore simbolico: significa "usa il timeout di
    // default", che viene risolto internamente, comando per comando, a
    // partire da max_resp_time_ (vedi implementazione). Passare un valore
    // > 0 continua a fissare esplicitamente il timeout per l'intera
    // richiesta, come prima.
    [[nodiscard]] Eg91Status https_get(std::string_view url, HttpsResult& out,
                                        int timeout_s = 0,
                                        const HeaderList* headers = nullptr);

    [[nodiscard]] Eg91Status https_post(std::string_view url, std::string_view body,
                                         HttpsResult& out, int timeout_s = 0,
                                         std::string_view content_type = "text/plain",
                                         const HeaderList* headers = nullptr);

    // Ritorna false se il comando fallisce; su successo out_time/out_dst sono valorizzati.
    [[nodiscard]] bool get_time(std::string& out_time, std::string& out_dst) ;

    void close_https_connection();

    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

private:
    Eg91Sender(uart_port_t uart_num, gpio_num_t tx, gpio_num_t rx,
               gpio_num_t power_pin, gpio_num_t reset_pin, const Eg91Config& config);

    [[nodiscard]] bool init();
    void deinit();

    // Ritorna true se il modulo e' effettivamente acceso e/o restato tale
    // dopo l'operazione (vedi implementazione in .cpp per il motivo:
    // PWRKEY commuta lo stato, non lo forza, quindi va sempre verificato).
    [[nodiscard]] bool power_on();

    // Spegnimento "morbido": tenta AT+QPOWD (via raccomandata da Quectel,
    // richiede l'UART ancora attiva) e ricorre all'impulso hardware su
    // PWRKEY solo come fallback. In entrambi i casi attende la procedura
    // di power-down del modulo e ne verifica l'esito prima di ritornare.
    [[nodiscard]] bool shut_down();

    // Reset hardware via RESET_N. Da Quectel: "Use RESET_N only when
    // turning off the module by AT+QPOWD command and PWRKEY pin failed" —
    // e' un'estrema ratio per un modulo non rispondente, non un
    // meccanismo di retry di routine (vedi enable()).
    [[nodiscard]] bool hard_reset();

    // Probe non distruttivo dello stato di alimentazione: invia "AT" e
    // verifica se il modulo risponde. Usato da power_on()/shut_down()
    // per sapere se un impulso PWRKEY va davvero inviato, dato che PWRKEY
    // commuta lo stato del modulo invece di forzarlo.
    [[nodiscard]] bool is_module_on();

    [[nodiscard]] Eg91Status send_command(std::string_view command, std::string& out_response,
                                           int wait_time_ms = -1, bool quiet_on_timeout = false);
    [[nodiscard]] bool check_network_registration();
    [[nodiscard]] bool enable();
    void disable();
    [[nodiscard]] Eg91Status activate_pdp_context();
    [[nodiscard]] Eg91Status deactivate_pdp_context();
    // Stessa semantica di timeout_s = 0 vista sopra: risolta internamente
    // su "AT+QHTTPPOST" se il chiamante passa 0.
    [[nodiscard]] Eg91Status send_https_post_body(std::string_view body, std::string& out_response,
                                                   int timeout_s);

    [[nodiscard]] uint32_t resp_timeout_ms(std::string_view cmd_key, uint32_t fallback_ms) const;
    [[nodiscard]] static std::string clean_http_payload(std::string_view raw);

    uart_port_t uart_num_;
    gpio_num_t uart_tx_pin_, uart_rx_pin_, lte_power_pin_, lte_reset_pin_;

    // Tempistiche hardware verificate sul datasheet Quectel EG91 Hardware
    // Design (PWRKEY/RESET_N pilotati "alto = asserito" via il driver del
    // micro, come confermato per questo schema).
    //
    // Impulso PWRKEY per l'accensione: richiesto >=500ms.
    static constexpr uint32_t kPowerOnPulseMs = 1000;
    // Attesa dopo l'impulso di accensione prima che il modulo sia pronto
    // (boot completo): il datasheet indica il modulo "Active" attorno ai
    // 12-13s dall'impulso; teniamo un margine.
    static constexpr uint32_t kPowerOnSettleMs = 15000;
    // Impulso PWRKEY per lo spegnimento via hardware (usato solo come
    // fallback se AT+QPOWD fallisce, vedi shut_down()): richiesto >=650ms.
    static constexpr uint32_t kShutdownPulseMs = 1000;
    // Attesa dopo AT+QPOWD / impulso PWRKEY prima che il modulo sia
    // realmente spento: il datasheet mostra una procedura di power-down
    // che puo' richiedere fino a ~30s. Attendere meno rischia di tagliare
    // l'alimentazione a procedura non conclusa, il che puo' danneggiare la
    // flash interna del modulo (nota esplicita del datasheet).
    static constexpr uint32_t kShutdownSettleMs = 30000;
    // Impulso RESET_N: il datasheet richiede una finestra precisa,
    // 150ms-460ms (non solo un minimo: c'e' anche un massimo).
    static constexpr uint32_t kResetPulseMs = 300;
    // Attesa dopo il rilascio di RESET_N prima che il modulo abbia
    // riavviato e sia di nuovo pronto.
    static constexpr uint32_t kResetSettleMs = 15000;
    static constexpr int kPdpCtxId = 1;
    static constexpr int kHttpsSslCtxId = 2;
    static constexpr int kMaxRetries = 2;
    static constexpr size_t kMaxBufferSize = 2048;
    static constexpr uint32_t kMinWaitTimeMs = 7000;
    // Tempo massimo di attesa del lock di trasmissione quando un altro thread
    // ha gia' una transazione in corso: deve coprire il caso peggiore tra
    // tutti i timeout di max_resp_time_ (il piu' lungo e' AT+QIACT, 150s),
    // con un margine, altrimenti un comando breve in coda a uno lungo
    // riceverebbe Busy anche se quello lungo sta procedendo normalmente.
    static constexpr uint32_t kMaxLockWaitMs = 160000;

    bool uart_installed_ = false;
    bool powered_on_ = false;
    bool enabled_ = false;
    bool network_registered_ = false;

    Eg91Config config_;
    UartRxManager rx_manager_;

    const std::unordered_map<std::string_view, uint32_t> max_resp_time_ = {
        {"AT+CPIN", 5000}, {"AT+QICSGP", 2000}, {"AT+QIACT", 150000}, {"AT+QIDEACT", 40000},
        {"AT+QLTS", 5000}, {"AT+QHTTPCFG", 2000}, {"AT+CFUN", 20000}, {"AT+QHTTPURL", 5000},
        {"AT+QHTTPGET", 80000}, {"AT+QHTTPREAD", 10000}, {"AT+QHTTPPOST", 80000},
        {"AT+QHTTPSTOP", 5000}, {"AT+QPOWD", 60000},{"AT+CSQ", 2000}, {"AT+COPS?", 5000}, {"AT+COPS=?", 60000}, {"AT+CREG?", 5000},
        {"AT+QICGSP", 2000}
    };

    const std::unordered_map<std::string_view, int> http_content_types_ = {
        {"application/x-www-form-urlencoded", 0}, {"text/plain", 1},
        {"application/octet-stream", 2}, {"multipart/form-data", 3},
        {"application/json", 4}, {"image/jpeg", 5},
    };
};