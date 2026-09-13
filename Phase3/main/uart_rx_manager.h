#pragma once

#include <string>
#include <string_view>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"

#include "at_commands_parser.h"

// Risposta ricevuta per un comando AT. text e' una copia proprietaria
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
// task chiama funzioni del driver UART che leggono il buffer
// (uart_read_bytes, uart_get_buffered_data_len). Il main task non lo fa
// mai direttamente.
//
// Sincronizzazione fra i due task:
//  - un mutex (state_mutex_) protegge pending_command_/last_response_ ED e'
//    tenuto dal listener per l'INTERA durata di lettura+parsing+scrittura
//    di un evento, non solo per uno snapshot iniziale. Questo rende
//    l'elaborazione di un evento UART atomica rispetto ad acquire()/
//    release(): il main task non puo' cambiare pending_command_ mentre il
//    listener sta ancora processando l'evento corrente. Il costo e' che
//    acquire()/release()/wait_response() possono restare bloccati per la
//    durata di un parsing (in pratica trascurabile: il listener gira a
//    priorita' piu' alta del main task, quindi non viene mai prelazionato
//    da task intermedi mentre tiene il lock);
//  - un semaforo binario (data_ready_sem_) e' il punto di rendezvous fra il
//    listener (produttore) e il main task (consumatore) quando arriva una
//    risposta valida.
//
// Risveglio del listener e shutdown (deinit()): il listener resta bloccato
// a tempo indefinito sulla coda eventi nativa del driver UART
// (uart_event_queue_), niente piu' polling periodico. Per farlo uscire dal
// ciclo, deinit() inserisce un evento sentinella in quella stessa coda:
// il listener si risveglia, controlla running_ PRIMA di processare
// l'evento e, se falso, esce dal ciclo (il contenuto della sentinella e'
// irrilevante).
//
// NOTA IMPORTANTE: questa versione NON svuota il buffer hardware della
// UART all'inizio di ogni comando (nessun flush). Una risposta arrivata in
// ritardo rispetto a un timeout precedente (es. un retry con lo STESSO
// comando subito dopo) puo' quindi, in teoria, essere scambiata per la
// risposta al comando nuovo: il protocollo AT non porta con se' alcun
// identificatore di correlazione che permetta di distinguerle. Se in
// futuro questo rischio residuo risultasse un problema reale, l'unica leva
// efficace e' tornare a svuotare il buffer hardware prima di ogni nuovo
// comando.
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

    // Ferma il listener task e libera le risorse. Sveglia subito il
    // listener (che e' bloccato a tempo indefinito sulla coda eventi UART)
    // inserendo un evento sentinella in testa alla coda, poi attende che
    // sia il task stesso a uscire dal proprio ciclo e a cancellarsi (vedi
    // listener_task()). Questo e' l'unico modo sicuro di far girare i
    // distruttori C++ dei suoi oggetti locali (accumulated_data, il buffer
    // di lettura, ecc.): un vTaskDelete() chiamato dall'esterno su un task
    // ancora vivo NON esegue lo stack unwinding, quindi la memoria heap
    // posseduta da quegli oggetti locali resterebbe persa per sempre
    // (memory leak) ad ogni deinit()/reinit, e se il task fosse stato
    // interrotto a meta' di una sezione protetta da state_mutex_ il mutex
    // resterebbe "preso" per sempre, condannando qualunque futura
    // xSemaphoreTake su di esso.
    void deinit();

    // Svuota lo stato software (pending_command_/last_response_). Sicura
    // da chiamare in qualsiasi momento: essendoci un solo possibile
    // chiamante (il main task), non serve alcuna serializzazione con
    // altre chiamate.
    void clear_state();

    // --- API a basso livello, usata da Eg91AtTransaction ---

    // Segna l'inizio di un comando. Ritorna false solo se un comando e'
    // gia' in corso (uso scorretto dell'API: acquire() senza il
    // release() corrispondente).
    [[nodiscard]] bool acquire(std::string_view command);
    // Chiude il comando corrente. Va chiamato sempre dopo acquire(), in coppia.
    void release() noexcept;
    // Attende la risposta del comando corrente (deve essere chiamato tra acquire/release).
    [[nodiscard]] bool wait_response(uint32_t wait_time_ms, AtResponse& out);

private:
    void listener_task();
    static void listener_task_trampoline(void* arg);

    uart_port_t uart_num_ = UART_NUM_MAX;
    QueueHandle_t uart_event_queue_ = nullptr;
    TaskHandle_t listener_task_handle_ = nullptr;

    SemaphoreHandle_t state_mutex_ = nullptr;   // protegge pending_command_/last_response_
    SemaphoreHandle_t data_ready_sem_ = nullptr;
    SemaphoreHandle_t task_exited_sem_ = nullptr; // dato dal listener appena prima di auto-cancellarsi

    ATCommandsParser parser_;

    // NOTA: il listener task filtra dal buffer di accumulo solo gli URC
    // non ambigui (es. "+QIURC:", "RDY" — mai usati come risposta solicited
    // in at_commands_parser.cpp). URC ambigui come "+CREG:" o "+CPIN:" non
    // richiesti (stesso prefisso di una risposta solicited ad AT+CREG?/
    // AT+CPIN?) restano deliberatamente nel buffer per non rischiare di
    // scartare una risposta legittima.

    std::string pending_command_;
    std::string last_response_;
    bool response_is_error_ = false;

    std::atomic<bool> running_{false};
};

// RAII: garantisce la chiusura del comando (release()) in ogni caso
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
