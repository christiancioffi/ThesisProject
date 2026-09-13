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

// Timeout con cui deinit() attende che il listener task esca dal proprio
// ciclo e si auto-cancelli, prima di ricorrere (come ultima spiaggia) a un
// vTaskDelete esterno.
constexpr uint32_t kShutdownWaitMs = 2000;

// Timeout con cui deinit() prova a inserire l'evento sentinella nella coda
// eventi UART per svegliare il listener. Basso: se la coda fosse piena non
// ha senso bloccare a lungo lo spegnimento, ci pensa comunque
// kShutdownWaitMs/vTaskDelete come rete di sicurezza.
constexpr uint32_t kShutdownSignalWaitMs = 20;

// Limite di sicurezza per accumulated_data: se il pattern atteso non
// matcha mai (comando non riconosciuto, desync, rumore sulla linea), senza
// un tetto il buffer crescerebbe senza limite fino al prossimo cambio di
// comando, rischiando di esaurire la RAM su un ESP32. Se sforato si
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

    state_mutex_ = xSemaphoreCreateMutex();
    data_ready_sem_ = xSemaphoreCreateBinary();
    task_exited_sem_ = xSemaphoreCreateBinary();

    if (!state_mutex_ || !data_ready_sem_ || !task_exited_sem_) {
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
        running_ = false;
        deinit();
        return false;
    }

    Logger::instance().debug(TAG, "UART listener task created successfully");
    return true;
}

void UartRxManager::deinit() {
    if (running_) {
        // Segnala l'uscita...
        running_ = false;

        // ...e sveglia subito il listener, che e' bloccato a tempo
        // indefinito su xQueueReceive: inserisce un evento sentinella in
        // testa alla coda eventi UART. Il contenuto dell'evento e'
        // irrilevante: il listener controlla running_ PRIMA di guardare
        // l'evento, appena si risveglia.
        if (uart_event_queue_) {
            uart_event_t wake_event{};
            if (xQueueSendToFront(uart_event_queue_, &wake_event, pdMS_TO_TICKS(kShutdownSignalWaitMs)) != pdTRUE) {
                Logger::instance().error(TAG, "Failed to enqueue shutdown signal for listener task");
                // Non fatale: il timeout di task_exited_sem_ qui sotto, e
                // il vTaskDelete forzato in ultima istanza, restano come
                // rete di sicurezza.
            }
        }
    }

    if (listener_task_handle_) {
        if (xSemaphoreTake(task_exited_sem_, pdMS_TO_TICKS(kShutdownWaitMs)) != pdTRUE) {
            // Non dovrebbe mai succedere in condizioni normali: significa
            // che il listener non e' riuscito a uscire dal proprio ciclo
            // entro un tempo ragionevole. Come ultima spiaggia si forza la
            // cancellazione dall'esterno, accettando il rischio di leak
            // discusso nell'header, piuttosto che bloccare per sempre lo
            // spegnimento del sistema.
            Logger::instance().error(TAG, "Listener task did not exit in time, forcing deletion");
            vTaskDelete(listener_task_handle_);
        }
        listener_task_handle_ = nullptr;
    }

    if (state_mutex_) { vSemaphoreDelete(state_mutex_); state_mutex_ = nullptr; }
    if (data_ready_sem_) { vSemaphoreDelete(data_ready_sem_); data_ready_sem_ = nullptr; }
    if (task_exited_sem_) { vSemaphoreDelete(task_exited_sem_); task_exited_sem_ = nullptr; }
}

void UartRxManager::listener_task_trampoline(void* arg) {
    static_cast<UartRxManager*>(arg)->listener_task();
}

void UartRxManager::listener_task() {

    {
        uart_event_t event;
        std::vector<uint8_t> buffer(kReadChunkSize);

        // Dati accumulati per il comando corrente: una risposta puo' arrivare
        // spezzata su piu' eventi UART_DATA (piu' letture separate), quindi il
        // match va tentato sul totale ricevuto finora, non sul solo ultimo
        // chunk. Vive come variabile locale del task (thread singolo, nessun
        // lock necessario). accumulated_for_command tiene traccia di quale
        // comando si riferisce l'accumulo corrente, cosi' da svuotarlo quando
        // il comando pending cambia.
        std::string accumulated_data;
        std::string accumulated_for_command;

        while (true) {
            // Attesa a tempo indefinito: nessun polling periodico. Il listener
            // si risveglia solo per un evento UART reale o per l'evento
            // sentinella inserito da deinit().
            if (xQueueReceive(uart_event_queue_, &event, portMAX_DELAY) != pdTRUE) {
                continue; // non dovrebbe accadere con attesa infinita
            }

            if (!running_) {
                // Richiesta di uscita: non importa se l'evento appena
                // ricevuto sia la sentinella o un evento UART reale arrivato
                // nel frattempo, si esce comunque senza processarlo.
                break;
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

            // Sezione critica ESTESA: dalla lettura del comando atteso fino
            // alla scrittura del risultato, tutto sotto lo stesso lock. Questo
            // rende l'intera elaborazione di questo evento atomica rispetto ad
            // acquire()/release(): il main task non puo' modificare
            // pending_command_ finche' non abbiamo finito di processare
            // l'evento corrente.
            xSemaphoreTake(state_mutex_, portMAX_DELAY);

            if (pending_command_.empty()) {
                xSemaphoreGive(state_mutex_);
                Logger::instance().error(TAG, "No command pending, data ignored");
                accumulated_data.clear();
                accumulated_for_command.clear();
                continue;
            }

            // Nuovo comando rispetto all'ultimo chunk accumulato: scarta
            // eventuali residui del precedente invece di mescolarli con
            // quello corrente.
            if (pending_command_ != accumulated_for_command) {
                accumulated_data.clear();
                accumulated_for_command = pending_command_;
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
                        (unsigned)kMaxAccumulatedBytes, pending_command_.c_str(), (unsigned)drop);
            }

            ParseResult result = parser_.parse_response(pending_command_, accumulated_data);
            if (!result.matched) {
                //Logger::instance().error(TAG, "Data did not match expected response for command '%s' (%zu bytes accumulated so far)", pending_command_.c_str(), accumulated_data.size());
                xSemaphoreGive(state_mutex_);
                continue;
            }

            last_response_.assign(result.text);
            response_is_error_ = result.is_error;
            Logger::instance().debug(TAG, "Matched response for command '%s'", pending_command_.c_str());

            xSemaphoreGive(state_mutex_);

            // Match trovato: il comando per questo accumulo e' concluso, si
            // riparte puliti dal prossimo evento. Va fatto solo ora perche'
            // result.text punta dentro accumulated_data e serviva ancora
            // valido fino a qui.
            accumulated_data.clear();
            accumulated_for_command.clear();

            // Il "give" fa da barriera di sincronizzazione: il consumer che
            // si risveglia da wait_response vede sempre lo stato scritto sopra.
            xSemaphoreGive(data_ready_sem_);
        }

    }

    Logger::instance().debug(TAG, "Listener task exiting normally");

    // Uscita pulita: gli oggetti locali (accumulated_data, buffer)
    // vengono distrutti qui dal normale stack unwinding, PRIMA che il
    // task si auto-cancelli. E' questo che deinit() aspetta
    // (task_exited_sem_) invece di forzare un vTaskDelete dall'esterno.
    xSemaphoreGive(task_exited_sem_);
    vTaskDelete(nullptr);
}

void UartRxManager::clear_state() {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    pending_command_.clear();
    last_response_.clear();
    response_is_error_ = false;
    xSemaphoreGive(state_mutex_);

    xSemaphoreTake(data_ready_sem_, 0); // drena eventuali segnalazioni residue
}

bool UartRxManager::acquire(std::string_view command) {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    if (!pending_command_.empty()) {
        xSemaphoreGive(state_mutex_);
        // Uso scorretto dell'API (acquire() senza il release() precedente):
        // con un solo chiamante e' sempre e solo un bug del chiamante.
        Logger::instance().error(TAG, "acquire() called while a command is already pending");
        return false;
    }
    pending_command_ = command;
    last_response_.clear();
    response_is_error_ = false;
    xSemaphoreGive(state_mutex_);

    // Drena un eventuale "give" residuo di un comando precedente andato in
    // timeout: se il listener l'avesse comunque completato DOPO che qui
    // avevamo gia' smesso di aspettare, quel token resterebbe pendente e
    // verrebbe consumato per errore dalla wait_response() di QUESTO nuovo
    // comando, facendola tornare come "risposta pronta" istantaneamente.
    xSemaphoreTake(data_ready_sem_, 0);

    return true;
}

void UartRxManager::release() noexcept {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    pending_command_.clear();
    xSemaphoreGive(state_mutex_);
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
