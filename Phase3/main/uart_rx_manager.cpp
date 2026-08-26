#include "uart_rx_manager.h"

#include <vector>
#include <algorithm>
#include <array>
#include "esp_log.h"
#include "Logger.h"

#define LISTENER_STACK_SIZE 32768      //32kb

static const char* TAG = "UartRxManager";

namespace {
constexpr size_t kReadChunkSize = 512;

// Limite di sicurezza per accumulated_data: se il pattern atteso non
// matcha mai (comando non riconosciuto, desync, rumore sulla linea), senza
// un tetto il buffer crescerebbe senza limite fino al prossimo cambio di
// transazione, rischiando di esaurire la RAM su un ESP32. Se sforato si
// scartano i byte piu' vecchi: la risposta vera arriva comunque per intero
// in un'unica sequenza contigua, quindi tenere solo la coda piu' recente
// non pregiudica il match una volta arrivata.
constexpr size_t kMaxAccumulatedBytes = 4096;

// Prefissi di URC (Unsolicited Result Code) che l'EG91 puo' inviare in modo
// spontaneo, senza che siano MAI la risposta solicited ad un comando gestito
// da ATCommandsParser (nessuna entry in at_commands_parser.cpp li usa come
// response_pattern). Sicuri da rimuovere dal buffer di accumulo prima del
// match: se il modulo li intercala durante una transazione lunga (es.
// +QIURC durante un AT+QHTTPGET/READ) restano altrimenti a sporcare
// l'accumulo anche dopo che la risposta vera e' stata trovata e consumata.
//
// NB: prefissi ambigui come "+CREG:" o "+CPIN:" (usati SIA come URC SIA
// come risposta solicited di AT+CREG?/AT+CPIN?) sono deliberatamente
// esclusi: rimuoverli alla cieca rischierebbe di cancellare una risposta
// legittima. Per ora vengono lasciati nel buffer (vedi nota in header).
constexpr std::array<const char*, 2> kUnsolicitedOnlyPrefixes = {
    "+QIURC:",
    "RDY",
};

// Rimuove da buf le righe COMPLETE (terminate da "\r\n") che iniziano con
// uno dei prefissi sopra. Una riga ancora troncata (senza "\r\n" finale)
// viene lasciata intatta: potrebbe non essere ancora arrivata per intero,
// e comunque il prossimo giro la ritroverebbe completa.
void strip_known_urcs(std::string& buf) {
    size_t line_start = 0;
    while (line_start < buf.size()) {
        size_t line_end = buf.find("\r\n", line_start);
        if (line_end == std::string::npos) break;
        std::string_view line(buf.data() + line_start, line_end - line_start);

        bool is_urc = false;
        for (const char* prefix : kUnsolicitedOnlyPrefixes) {
            if (line.rfind(prefix, 0) == 0) {
                is_urc = true;
                break;
            }
        }

        if (is_urc) {
            buf.erase(line_start, (line_end + 2) - line_start);
            // Non avanzare line_start: la riga successiva e' scivolata qui.
        } else {
            line_start = line_end + 2;
        }
    }
}
}

UartRxManager::~UartRxManager() {
    deinit();
}

bool UartRxManager::init(uart_port_t uart_num, QueueHandle_t uart_event_queue) {
    uart_num_ = uart_num;
    uart_event_queue_ = uart_event_queue;

    command_mutex_ = xSemaphoreCreateMutex();
    state_mutex_ = xSemaphoreCreateMutex();
    data_ready_sem_ = xSemaphoreCreateBinary();

    if (!command_mutex_ || !state_mutex_ || !data_ready_sem_) {
        Logger::instance().error(TAG, "Failed to create synchronization primitives");
        deinit();
        return false;
    }

    running_ = true;

    // Task pinnato al core APP (1): lascia il core PRO (0) piu' libero per
    // WiFi/BT interni se mai usati insieme; su single-core (ESP32-S2/C3) la
    // affinity viene ignorata dal framework.
    BaseType_t ok = xTaskCreatePinnedToCore(
        &UartRxManager::listener_task_trampoline,
        "eg91_uart_rx",
        LISTENER_STACK_SIZE,
        this,
        configMAX_PRIORITIES - 3,
        &listener_task_handle_,
        tskNO_AFFINITY);

    if (ok != pdPASS) {
        Logger::instance().error(TAG, "Failed to create UART listener task");
        deinit();
        return false;
    }

    Logger::instance().debug(TAG, "UART listener task created successfully");
    return true;
}

void UartRxManager::deinit() {
    running_ = false;
    if (listener_task_handle_) {
        vTaskDelete(listener_task_handle_);
        listener_task_handle_ = nullptr;
    }
    if (command_mutex_) { vSemaphoreDelete(command_mutex_); command_mutex_ = nullptr; }
    if (state_mutex_) { vSemaphoreDelete(state_mutex_); state_mutex_ = nullptr; }
    if (data_ready_sem_) { vSemaphoreDelete(data_ready_sem_); data_ready_sem_ = nullptr; }
}

void UartRxManager::listener_task_trampoline(void* arg) {
    static_cast<UartRxManager*>(arg)->listener_task();
}

void UartRxManager::listener_task() {
    uart_event_t event;
    std::vector<uint8_t> buffer(kReadChunkSize);

    // Dati accumulati per la transazione corrente: una risposta puo' arrivare
    // spezzata su piu' eventi UART_DATA (piu' letture separate), quindi il
    // match va tentato sul totale ricevuto finora, non sul solo ultimo chunk.
    // Vive come variabile locale del task (thread singolo, nessun lock
    // necessario) e viene svuotata quando cambia transazione o dopo un match.
    std::string accumulated_data;
    uint32_t accumulated_transaction_id = 0;
    bool has_accumulated_transaction = false;

    while (running_) {
        if (xQueueReceive(uart_event_queue_, &event, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        if (event.type != UART_DATA) {
            // FIFO overflow / buffer full: come nell'originale, non gestiti
            // esplicitamente; si scarta l'evento e si prosegue.
            continue;
        }

        size_t available = 0;
        uart_get_buffered_data_len(uart_num_, &available);
        if (available == 0) continue;

        if (buffer.size() < available) buffer.resize(available);
        int len = uart_read_bytes(uart_num_, buffer.data(),
                                   std::min(available, buffer.size()),
                                   pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        std::string_view data(reinterpret_cast<char*>(buffer.data()), static_cast<size_t>(len));
        Logger::instance().debug(TAG, "%d bytes read from UART", len);
        Logger::instance().debug(TAG, "Data: %.*s", len, data.data());

        // Sezione critica breve: legge il comando atteso e l'id della
        // transazione corrente (scritti dal thread chiamante in acquire()).
        xSemaphoreTake(state_mutex_, portMAX_DELAY);
        std::string cmd_copy = pending_command_;
        uint32_t id_snapshot = transaction_id_;
        bool has_pending = !cmd_copy.empty();
        xSemaphoreGive(state_mutex_);

        if (!has_pending) {
            Logger::instance().error(TAG, "No command pending, data ignored");
            accumulated_data.clear();
            has_accumulated_transaction = false;
            continue;
        }

        // Nuova transazione rispetto all'ultimo chunk accumulato: scarta
        // eventuali residui della precedente (es. coda di una risposta
        // andata in timeout) invece di mescolarli con quella corrente.
        if (!has_accumulated_transaction || id_snapshot != accumulated_transaction_id) {
            accumulated_data.clear();
            accumulated_transaction_id = id_snapshot;
            has_accumulated_transaction = true;
        }
        accumulated_data.append(reinterpret_cast<char*>(buffer.data()), static_cast<size_t>(len));

        // Rimuove eventuali URC non ambigui (es. +QIURC:, RDY) intercalati
        // dal modulo durante la transazione, cosi' non restano a sporcare
        // il buffer ne' a confondere pattern generici come POST_BODY/GET_HEADERS.
        strip_known_urcs(accumulated_data);

        if (accumulated_data.size() > kMaxAccumulatedBytes) {
            size_t drop = accumulated_data.size() - kMaxAccumulatedBytes;
            accumulated_data.erase(0, drop);
            Logger::instance().error(TAG, "accumulated_data exceeded %u bytes for command '%s', dropped %u oldest bytes",
                      (unsigned)kMaxAccumulatedBytes, cmd_copy.c_str(), (unsigned)drop);
        }

        ParseResult result = parser_.parse_response(cmd_copy, accumulated_data);
        if (!result.matched){
            //Logger::instance().error(TAG, "Data did not match expected response for command '%s' (%zu bytes accumulated so far)",cmd_copy.c_str(), accumulated_data.size());
            continue;
        }

        // Prima di pubblicare il risultato, verifica che la transazione per
        // cui e' stato calcolato sia ANCORA quella corrente: se nel frattempo
        // c'e' stato un release()+acquire() (nuovo transaction_id_), questo
        // match e' una risposta "in ritardo" di una transazione gia' conclusa
        // (es. andata in timeout) e va scartato, non attribuito alla nuova.
        bool still_current = false;
        xSemaphoreTake(state_mutex_, portMAX_DELAY);
        still_current = (id_snapshot == transaction_id_) && !pending_command_.empty();
        if (still_current) {
            last_response_.assign(result.text);
            response_is_error_ = result.is_error;
            Logger::instance().debug(TAG, "Matched response for command '%s'", cmd_copy.c_str());
        }
        xSemaphoreGive(state_mutex_);

        // Match trovato (pubblicato o scartato come stale): la transazione
        // per questo accumulo e' comunque conclusa, si riparte puliti dal
        // prossimo evento. Va fatto solo ora perche' result.text punta
        // dentro accumulated_data e serviva ancora valido fino a qui.
        accumulated_data.clear();
        has_accumulated_transaction = false;

        if (still_current) {
            // Il "give" fa da barriera di sincronizzazione: il consumer che
            // si risveglia da wait_response vede sempre lo stato scritto sopra.
            xSemaphoreGive(data_ready_sem_);
        } else {
            Logger::instance().debug(TAG, "Discarded stale response (transaction already ended)");
        }
    }

    vTaskDelete(nullptr);
}

void UartRxManager::clear_state() {
    // Serializzata con command_mutex_: se un altro thread ha una transazione
    // in corso, questa chiamata attende che finisca invece di corromperne lo
    // stato (bug presente nella versione precedente).
    xSemaphoreTake(command_mutex_, portMAX_DELAY);

    uint8_t dump[256];
    int len;
    do {
        len = uart_read_bytes(uart_num_, dump, sizeof(dump), 0);
    } while (len > 0);

    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    pending_command_.clear();
    last_response_.clear();
    response_is_error_ = false;
    ++transaction_id_; // invalida eventuali match "in ritardo" gia' in volo
    xSemaphoreGive(state_mutex_);

    xSemaphoreTake(data_ready_sem_, 0); // drena eventuali segnalazioni residue

    xSemaphoreGive(command_mutex_);
}

bool UartRxManager::acquire(std::string_view command, uint32_t lock_timeout_ms) {
    TickType_t ticks = (lock_timeout_ms == portMAX_DELAY)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(lock_timeout_ms);

    if (xSemaphoreTake(command_mutex_, ticks) != pdTRUE) {
        return false; // un'altra transazione e' gia' in corso su un altro thread
    }

    // Scarta eventuali byte gia' presenti nel buffer hardware della UART:
    // residuo di una risposta arrivata in ritardo per la transazione
    // precedente (es. dopo un timeout). transaction_id_ protegge gia' dal
    // caso in cui il nuovo comando sia DIVERSO dal precedente (still_current
    // scarta il match se la transazione e' cambiata), ma non basta se il
    // nuovo comando e' IDENTICO al vecchio (retry dopo timeout): in quel
    // caso una risposta stale soddisferebbe comunque il pattern atteso e
    // verrebbe scambiata per la risposta fresca. Svuotare qui il buffer
    // hardware prima di iniziare la nuova transazione elimina anche
    // l'eventuale evento UART_DATA gia' in coda per quei byte: quando il
    // listener task lo processera', uart_get_buffered_data_len() trovera'
    // 0 byte disponibili e lo scartera' prima di arrivare all'accumulo.
    uint8_t drain_buf[256];
    int drain_len;
    do {
        drain_len = uart_read_bytes(uart_num_, drain_buf, sizeof(drain_buf), 0);
    } while (drain_len > 0);

    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    pending_command_ = command;
    last_response_.clear();
    response_is_error_ = false;
    ++transaction_id_;
    xSemaphoreGive(state_mutex_);

    xSemaphoreTake(data_ready_sem_, 0); // drena eventuali segnalazioni residue di una precedente transazione

    return true;
}

void UartRxManager::release() noexcept {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    pending_command_.clear();
    xSemaphoreGive(state_mutex_);

    xSemaphoreGive(command_mutex_);
}

bool UartRxManager::wait_response(uint32_t wait_time_ms, AtResponse& out) {
    Logger::instance().debug(TAG, "Waiting for response for %u ms...", (unsigned)wait_time_ms);

    if (xSemaphoreTake(data_ready_sem_, pdMS_TO_TICKS(wait_time_ms)) != pdTRUE) {
        Logger::instance().error(TAG, "TIMEOUT");
        return false;
    }

    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    out.text = last_response_;
    out.is_error = response_is_error_;
    xSemaphoreGive(state_mutex_);

    return true;
}