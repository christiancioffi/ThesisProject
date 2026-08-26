#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "esp_dsp.h"
#include "filter_module.h"
#include "Logger.h"

/**
 * Applica una cascata di filtri Biquad a un buffer di dati (Passata singola).
 *
 * NOTA: nessuna eccezione C++ usata (progetto compilato con eccezioni
 * disabilitate). Le allocazioni usano malloc/free con controllo esplicito
 * del puntatore nullo, come da convenzione in ambienti embedded/ESP-IDF.
 *
 * Ritorna true se l'elaborazione è andata a buon fine, false se una
 * allocazione è fallita (nessun buffer viene lasciato "a metà": in caso
 * di fallimento si libera tutto quello già allocato prima di uscire).
 */
bool apply_sosfilt(const float* input, float* output, int n_samples, float* coeffs, float* w, int n_sections)
{
    // Alloca due buffer per alternare (ping-pong)
    float* buf1 = (float*)malloc(n_samples * sizeof(float));
    if (buf1 == nullptr) {
        printf("Error: allocation of buf1 failed (%d samples)\n", n_samples);
        return false;
    }

    float* buf2 = (float*)malloc(n_samples * sizeof(float));
    if (buf2 == nullptr) {
        printf("Error: allocation of buf2 failed (%d samples)\n", n_samples);
        free(buf1); // libera quanto già allocato prima di uscire
        return false;
    }

    const float* current_in = input;
    float* current_out;

    for (int s = 0; s < n_sections; ++s) {
        // Se è l'ultimo stadio, scriviamo nel buffer di output finale
        if (s == n_sections - 1) {
            current_out = output;
        } else {
            // Alterniamo tra buf1 e buf2 per non sovrascrivere l'input
            current_out = (s % 2 == 0) ? buf1 : buf2;
        }

        // Calcolo offset puntatori
        float* stage_coeffs = coeffs + (s * 5);
        float* stage_w = w + (s * 2);

        // Elaborazione (current_in è const float*, la libreria vuole float*
        // in input: cast esplicito, dato che dsps_biquad_f32_ansi non lo
        // modifica realmente, ma la sua firma non è const-correct)
        dsps_biquad_f32_ansi((float*)current_in, current_out, n_samples, stage_coeffs, stage_w);

        // L'output di questo stadio diventa l'input del prossimo
        current_in = current_out;
    }

    free(buf1);
    free(buf2);
    return true;
}

/**
 * Emula perfettamente la funzione scipy.signal.sosfiltfilt di Python.
 * Lavora direttamente con puntatori grezzi float* per l'input e l'output.
 *
 * NOTA: nessuna eccezione C++. Ogni allocazione viene verificata subito
 * dopo la chiamata a malloc; in caso di fallimento si liberano tutti i
 * buffer allocati fino a quel punto (goto/etichetta di cleanup) e si
 * ritorna false, senza mai lasciare memoria leakata.
 *
 * Ritorna true se il filtraggio è andato a buon fine, false in caso di
 * errore di allocazione (dettagli stampati su stdout).
 */
bool apply_sosfiltfilt(const float* input_data, float* output_data, int n_samples, const BiquadCoeffs* sos_cfs, int num_sections)
{
    // Puntatori inizializzati a nullptr: così il blocco di cleanup finale
    // può chiamare free() su tutti in sicurezza, anche su quelli mai allocati
    float* padded_data   = nullptr;
    float* coeffs         = nullptr;
    float* filter_buffer  = nullptr;
    float* w               = nullptr;
    bool success = false;

    // 1. Calcolo dinamico di padlen (esattamente come la logica Scipy)
    int zero_b2 = 0;
    int zero_a2 = 0;
    for (int i = 0; i < num_sections; ++i) {
        if (sos_cfs[i].b2 == 0.0f) zero_b2++;
        if (sos_cfs[i].a2 == 0.0f) zero_a2++;
    }
    int min_zero = std::min(zero_b2, zero_a2);
    int padlen = 3 * (2 * num_sections + 1 - min_zero);

    // Controllo di sicurezza: il padding non può superare la dimensione del segnale
    if (n_samples <= padlen) {
        padlen = n_samples - 1;
    }
    //printf("Padding calcolato: %d campioni (per %d campioni di input)\n", padlen, n_samples);

    // 2. Allocazione e costruzione del segnale con padding "odd" (speculare riflesso)
    int padded_len = n_samples + 2 * padlen;
    padded_data = (float*)malloc(padded_len * sizeof(float));
    if (padded_data == nullptr) {
        printf("Error: allocation of padded_data failed (%d samples)\n", padded_len);
        goto cleanup;
    }

    // Padding sinistro speculare rispetto al primo elemento
    {
        float first_val = input_data[0];
        for (int i = 0; i < padlen; ++i) {
            padded_data[i] = 2.0f * first_val - input_data[padlen - i];
        }
    }

    // Parte centrale: inserimento dei dati reali estratti dal float*
    memcpy(padded_data + padlen, input_data, n_samples * sizeof(float));

    // Padding destro speculare rispetto all'ultimo elemento
    {
        float last_val = input_data[n_samples - 1];
        for (int i = 0; i < padlen; ++i) {
            padded_data[padlen + n_samples + i] = 2.0f * last_val - input_data[n_samples - 2 - i];
        }
    }

    // Estraiamo il primissimo campione del segnale con padding (x0)
    //printf("Primo campione del segnale con padding (x0): %.6f\n", padded_data[0]);

    // Coefficienti
    coeffs = (float*)malloc(num_sections * 5 * sizeof(float));
    if (coeffs == nullptr) {
        printf("Error: allocation of coeffs failed\n");
        goto cleanup;
    }

    for (int s = 0; s < num_sections; s++) {
        // Copia i 5 coefficienti di ogni sezione
        coeffs[s * 5 + 0] = sos_cfs[s].b0;
        coeffs[s * 5 + 1] = sos_cfs[s].b1;
        coeffs[s * 5 + 2] = sos_cfs[s].b2;
        coeffs[s * 5 + 3] = sos_cfs[s].a1;
        coeffs[s * 5 + 4] = sos_cfs[s].a2;
    }

    // Buffer temporaneo per i risultati del filtraggio
    filter_buffer = (float*)malloc(padded_len * sizeof(float));
    if (filter_buffer == nullptr) {
        printf("Error: allocation of filter_buffer failed (%d samples)\n", padded_len);
        goto cleanup;
    }

    // Buffer di lavoro per gli stati di ciascuna sezione
    w = (float*)malloc(2 * num_sections * sizeof(float));
    if (w == nullptr) {
        printf("Error: allocation of w failed\n");
        goto cleanup;
    }

    // INIZIALIZZAZIONE DEI PARAMETRI DI STATO W[0] E W[1] PER OGNI SEZIONE DEL FILTRO (PASSO FORWARD)
    {
        float scale = padded_data[0];

        for (int s = 0; s < num_sections; s++) {
            const float* c = &coeffs[s * 5];

            float b0 = c[0];
            float b1 = c[1];
            float b2 = c[2];
            float a1 = c[3];
            float a2 = c[4];

            // Stato DF-II di ESP-DSP:
            // w[2*s]   = d[n-1]
            // w[2*s+1] = d[n-2]
            float d = scale / (1.0f + a1 + a2);

            w[2*s]     = d;
            w[2*s + 1] = d;

            // Guadagno DC della sezione
            float gain = (b0 + b1 + b2) / (1.0f + a1 + a2);

            // La sezione successiva riceverà questo livello DC
            scale *= gain;
        }
    }

    // 3. PASSO FORWARD (Filtraggio in avanti)
    if (!apply_sosfilt(padded_data, filter_buffer, padded_len, coeffs, w, num_sections)) {
        printf("Error: apply_sosfilt (forward) failed\n");
        goto cleanup;
    }

    // 4. INVERSIONE TEMPORALE (In-place reverse per preparare la passata all'indietro)
    std::reverse(filter_buffer, filter_buffer + padded_len);

    // INIZIALIZZAZIONE DEI PARAMETRI DI STATO W[0] E W[1] PER OGNI SEZIONE DEL FILTRO (PASSO BACKWARD)
    //printf("Primo campione del segnale con padding (y0): %.6f\n", filter_buffer[0]);

    {
        float scale = filter_buffer[0];

        for (int s = 0; s < num_sections; s++) {
            const float* c = &coeffs[s * 5];

            float b0 = c[0];
            float b1 = c[1];
            float b2 = c[2];
            float a1 = c[3];
            float a2 = c[4];

            // Stato DF-II di ESP-DSP:
            // w[2*s]   = d[n-1]
            // w[2*s+1] = d[n-2]
            float d = scale / (1.0f + a1 + a2);

            w[2*s]     = d;
            w[2*s + 1] = d;

            // Guadagno DC della sezione
            float gain = (b0 + b1 + b2) / (1.0f + a1 + a2);

            // La sezione successiva riceverà questo livello DC
            scale *= gain;
        }
    }

    // 5. PASSO BACKWARD (Filtraggio all'indietro)
    if (!apply_sosfilt(filter_buffer, filter_buffer, padded_len, coeffs, w, num_sections)) {
        printf("Error: apply_sosfilt (backward) failed\n");
        goto cleanup;
    }

    // 6. RIPRISTINO DELL'ORDINE ORIGINALE
    std::reverse(filter_buffer, filter_buffer + padded_len);

    // 7. COPIA E TRONCAMENTO: Estraiamo solo la porzione centrale filtrata nel puntatore di destinazione
    memcpy(output_data, filter_buffer + padlen, n_samples * sizeof(float));

    success = true;

cleanup:
    // free(nullptr) è sicuro e non fa nulla: possiamo chiamarlo su ogni
    // puntatore incondizionatamente, anche su quelli mai allocati
    free(padded_data);
    free(coeffs);
    free(filter_buffer);
    free(w);

    return success;
}