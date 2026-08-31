#pragma once

#include <string>
#include <string_view>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

#include "at_commands_parser.h"

// Risposta ricevuta per una transazione AT. text e' una copia proprietaria
// (a differenza di ParseResult::text che punta al buffer temporaneo di ricezione).
struct AtResponse {
    bool is_error = false;
    std::string text;
};

// Gestore della ricezione UART. Il sistema ha ESATTAMENTE due task che lo
// toccano, mai di piu':
//  - il "main task", che esegue un comando AT alla volta e ne attende la
//    risposta (acquire -> uart_write_bytes -> wait_response -> release);
//  - il "listener task" (listener_task_), l'unico thread che legge mai
//    dall'hardware UART: resta bloccato sulla coda eventi nativa del
//    driver e fa il parsing dei byte in arrivo.
//
// Regola d'oro che tutto il resto della classe rispetta: SOLO il listener
// task chiama funzioni del driver UART che leggono/svuotano il buffer
// (uart_read_bytes, uart_flush_input, uart_get_buffered_data_len). Il main
// task non lo fa mai direttamente, nemmeno per "ripulire" lo stato prima di
// una nuova transazione: se lo facesse, le due chiamate potrebbero
// interfogliarsi in modo indefinito sullo stesso buffer del driver
// (comportamento non documentato/non sicuro per chiamate concorrenti sulla
// stessa porta), con il rischio concreto di perdere o duplicare byte della
// risposta vera. Per questo una richiesta di pulizia del buffer hardware
// (vedi flush_requested_ sotto) viene sempre delegata al listener e
// attesa in modo sincrono dal main task, invece di essere eseguita in loco.
//
// Sincronizzazione fra i due task:
//  - un semaforo binario (data_ready_sem_) e' il punto di rendezvous fra il
//    listener (produttore) e il main task (consumatore) quando arriva una
//    risposta valida;
//  - un secondo semaforo binario (flush_done_sem_) e' l'analogo per la
//    richiesta di pulizia del buffer hardware: il main task la richiede
//    (flush_requested_ = true) e attende l'ack del listener;
//  - un mutex leggero (state_mutex_) protegge le poche variabili condivise
//    (comando atteso, ultima risposta) scritte dal main task e lette dal
//    listener, o viceversa. Non serializza transazioni: con un solo
//    chiamante, acquire()/release() sono gia' intrinsecamente sequenziali,
//    quindi qui non serve (e non c'e' piu') un mutex "a grana grossa" per
//    quello scopo.
//
// Uso tipico (vedi Eg91AtTransaction sotto per la RAII guard):
//   AtResponse resp;
//   {
//       Eg91AtTransaction txn(rx_mgr, "AT+CREG?");
//       uart_write_bytes(...);
//       if (!rx_mgr.wait_response(5000, resp)) { /* timeout */ }
//   } // il "diritto di trasmissione" viene rilasciato qui, anche in caso di timeout
class UartRxManager {
public:
    UartRxManager() = default;
    ~UartRxManager();

    UartRxManager(const UartRxManager&) = delete;
    UartRxManager& operator=(const UartRxManager&) = delete;

    // Crea mutex/semafori/task. Il chiamante deve aver gia' fatto
    // uart_driver_install() e passare qui la coda eventi ottenuta.
    [[nodiscard]] bool init(uart_port_t uart_num, QueueHandle_t uart_event_queue);

    // Ferma il listener task e libera le risorse. A differenza della
    // versione precedente NON forza la cancellazione del task dall'esterno:
    // segnala l'uscita (running_ = false) e attende che sia il task stesso
    // a terminare il proprio ciclo e a cancellarsi (vedi listener_task()).
    // Questo e' l'unico modo sicuro di far girare i distruttori C++ dei
    // suoi oggetti locali (accumulated_data, il buffer di lettura, ecc.):
    // un vTaskDelete() chiamato dall'esterno su un task ancora vivo NON
    // esegue lo stack unwinding, quindi la memoria heap posseduta da quegli
    // oggetti locali resterebbe persa per sempre (memory leak) ad ogni
    // deinit()/reinit, e se il task fosse stato interrotto a meta' di una
    // sezione protetta da state_mutex_ il mutex resterebbe "preso" per
    // sempre, condannando qualunque futura xSemaphoreTake su di esso.
    void deinit();

    // Svuota lo stato software (pending_command_/last_response_) e chiede
    // al listener di svuotare anche il buffer hardware della UART (vedi
    // flush_requested_). Sicura da chiamare in qualsiasi momento: essendoci
    // un solo possibile chiamante (il main task), non serve alcuna
    // serializzazione con altre transazioni.
    void clear_state();

    // --- API a basso livello, usata da Eg91AtTransaction ---

    // Segna l'inizio di una transazione per "command" e chiede al listener
    // di ripulire il buffer hardware prima di ritornare, cosi' un'eventuale
    // risposta "in ritardo" della transazione precedente (tipicamente un
    // retry con lo STESSO comando dopo un timeout) non puo' soddisfare per
    // sbaglio il pattern atteso della nuova transazione. Ritorna false solo
    // se una transazione e' gia' attiva (uso scorretto dell'API: acquire()
    // senza il release() corrispondente), non essendoci piu' concorrenza da
    // arbitrare con un lock.
    [[nodiscard]] bool acquire(std::string_view command);
    // Chiude la transazione corrente. Va chiamato sempre dopo acquire(), in coppia.
    void release() noexcept;
    // Attende la risposta della transazione corrente (deve essere chiamato tra acquire/release).
    [[nodiscard]] bool wait_response(uint32_t wait_time_ms, AtResponse& out);

private:
    void listener_task();
    static void listener_task_trampoline(void* arg);

    // Richiede al listener di svuotare il buffer hardware della UART e
    // attende il suo ack (con timeout, per non rischiare un deadlock
    // permanente del main task se il listener fosse inaspettatamente
    // bloccato). E' l'UNICO punto in cui il main task interagisce -
    // indirettamente, tramite il listener - con il buffer hardware.
    void request_flush_and_wait();

    uart_port_t uart_num_ = UART_NUM_MAX;
    QueueHandle_t uart_event_queue_ = nullptr;
    TaskHandle_t listener_task_handle_ = nullptr;

    SemaphoreHandle_t state_mutex_ = nullptr;   // protegge pending_command_/last_response_
    SemaphoreHandle_t data_ready_sem_ = nullptr;
    SemaphoreHandle_t flush_done_sem_ = nullptr; // ack del listener a una richiesta di flush
    SemaphoreHandle_t task_exited_sem_ = nullptr; // dato dal listener appena prima di auto-cancellarsi

    ATCommandsParser parser_;

    // NOTA: il listener task filtra dal buffer di accumulo solo gli URC
    // non ambigui (es. "+QIURC:", "RDY" — mai usati come risposta solicited
    // in at_commands_parser.cpp). URC ambigui come "+CREG:" o "+CPIN:" non
    // richiesti (stesso prefisso di una risposta solicited ad AT+CREG?/
    // AT+CPIN?) restano deliberatamente nel buffer per non rischiare di
    // scartare una risposta legittima: se in futuro serve distinguerli
    // (es. per reagire a un cambio di stato di rete durante un'altra
    // transazione) va aggiunta una logica dedicata, non un filtro cieco
    // per prefisso.

    std::string pending_command_;
    std::string last_response_;
    bool response_is_error_ = false;
    // Incrementato ad ogni acquire()/clear_state(): permette al listener
    // task di scartare un match "in ritardo" che appartiene a una
    // transazione gia' conclusa (timeout) invece di attribuirlo per errore
    // a quella successiva.
    uint32_t transaction_id_ = 0;

    // Vero fra un acquire() e il release() corrispondente. Letta e scritta
    // solo dal main task (unico chiamante di acquire/release), quindi non
    // e' un dato condiviso con il listener e non necessita di alcun lock:
    // serve solo a impedire un uso scorretto dell'API (acquire() annidati).
    bool transaction_active_ = false;

    // Richiesta di pulizia del buffer hardware: scritta dal main task
    // (request_flush_and_wait) e letta/azzerata dal listener task, quindi
    // DEVE essere atomica per garantire la visibilita' tra i due thread
    // (qui non e' protetta da state_mutex_ perche' va controllata dal
    // listener ad ogni giro di loop, anche quando non c'e' nessun evento
    // UART da processare).
    std::atomic<bool> flush_requested_{false};

    std::atomic<bool> running_{false};
};

// RAII: garantisce la chiusura della transazione (release()) in ogni caso
// (successo, errore, timeout), senza bisogno di eccezioni.
class Eg91AtTransaction {
public:
    Eg91AtTransaction(UartRxManager& mgr, std::string_view command)
        : mgr_(mgr), acquired_(mgr.acquire(command)) {}

    ~Eg91AtTransaction() {
        if (acquired_) mgr_.release();
    }

    Eg91AtTransaction(const Eg91AtTransaction&) = delete;
    Eg91AtTransaction& operator=(const Eg91AtTransaction&) = delete;

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    UartRxManager& mgr_;
    bool acquired_;
};
