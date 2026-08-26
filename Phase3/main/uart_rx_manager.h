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

// Gestore della ricezione UART, basato su multithreading FreeRTOS "classico":
//
//  - un task dedicato (listener_task_) resta bloccato sulla coda eventi nativa
//    del driver UART e processa i byte in arrivo;
//  - un mutex (command_mutex_) serializza le transazioni AT complete (invio +
//    attesa risposta) tra thread chiamanti concorrenti: sulla EG91 c'e' UNA
//    sola UART fisica, quindi due AT command in volo contemporaneamente
//    corromperebbero lo stato. Nella versione precedente questo caso non era
//    gestito correttamente;
//  - un semaforo binario (data_ready_sem_) e' il punto di rendezvous tra il
//    listener task (produttore) e il thread chiamante (consumatore) — lo
//    stesso pattern di una condition variable, ma con il primitivo nativo
//    FreeRTOS piu' leggero;
//  - un secondo mutex, piu' fine (state_mutex_), protegge le poche variabili
//    condivise (comando atteso, ultima risposta) scritte dal chiamante e
//    lette dal listener task, o viceversa.
//
// Uso tipico (vedi Eg91AtTransaction sotto per la RAII guard):
//   AtResponse resp;
//   {
//       Eg91AtTransaction txn(rx_mgr, "AT+CREG?");
//       uart_write_bytes(...);
//       if (!rx_mgr.wait_response(5000, resp)) { /* timeout */ }
//   } // il mutex viene rilasciato automaticamente qui, anche in caso di timeout
class UartRxManager {
public:
    UartRxManager() = default;
    ~UartRxManager();

    UartRxManager(const UartRxManager&) = delete;
    UartRxManager& operator=(const UartRxManager&) = delete;

    // Crea mutex/semafori/task. Il chiamante deve aver gia' fatto
    // uart_driver_install() e passare qui la coda eventi ottenuta.
    [[nodiscard]] bool init(uart_port_t uart_num, QueueHandle_t uart_event_queue);
    void deinit();

    // Svuota il buffer RX hardware e lo stato interno. Sicura da chiamare in
    // qualsiasi momento anche con altri thread attivi: acquisisce essa stessa
    // command_mutex_, quindi attende che una transazione in corso su un altro
    // thread finisca prima di procedere (e blocca nuove acquisizioni nel frattempo).
    void clear_state();

    // --- API a basso livello, usata da Eg91AtTransaction ---

    // Blocca finche' non ottiene il diritto esclusivo di trasmissione (o scade
    // lock_timeout_ms). Svuota anche il buffer hardware della UART prima di
    // iniziare la nuova transazione, per scartare eventuali byte residui di
    // una risposta arrivata in ritardo dopo un timeout (vedi commento nel .cpp).
    [[nodiscard]] bool acquire(std::string_view command, uint32_t lock_timeout_ms = portMAX_DELAY);
    // Rilascia il diritto di trasmissione. Va chiamato sempre dopo acquire(), in coppia.
    void release() noexcept;
    // Attende la risposta della transazione corrente (deve essere chiamato tra acquire/release).
    [[nodiscard]] bool wait_response(uint32_t wait_time_ms, AtResponse& out);

private:
    void listener_task();
    static void listener_task_trampoline(void* arg);

    uart_port_t uart_num_ = UART_NUM_MAX;
    QueueHandle_t uart_event_queue_ = nullptr;
    TaskHandle_t listener_task_handle_ = nullptr;

    SemaphoreHandle_t command_mutex_ = nullptr; // serializza le transazioni intere
    SemaphoreHandle_t state_mutex_ = nullptr;   // protegge pending_command_/last_response_
    SemaphoreHandle_t data_ready_sem_ = nullptr;

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
    // Incrementato ad ogni acquire(): permette al listener task di scartare
    // un match "in ritardo" che appartiene a una transazione gia' conclusa
    // (timeout) invece di attribuirlo per errore a quella successiva.
    uint32_t transaction_id_ = 0;

    std::atomic<bool> running_{false};
};

// RAII: sostituisce il pattern try/finally che si userebbe con le eccezioni
// per garantire il rilascio del lock di trasmissione in ogni caso (successo,
// errore, timeout). Con le eccezioni disattivate questo e' l'unico modo
// corretto e sicuro per garantirlo.
class Eg91AtTransaction {
public:
    Eg91AtTransaction(UartRxManager& mgr, std::string_view command,
                       uint32_t lock_timeout_ms = portMAX_DELAY)
        : mgr_(mgr), acquired_(mgr.acquire(command, lock_timeout_ms)) {}

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