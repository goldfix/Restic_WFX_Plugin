# Specifica Tecnica di Refactoring: Report Statistiche Diretto su Cartella (`F3` / `Alt+Invio`) in Markdown e Ottimizzazione Performance

**Progetto:** `restic.wfx64` — Total Commander WFX File System Plugin per Restic  
**Target Version:** `v1.3.0` (Refactoring architetturale e performance)  
**Linguaggio e Toolchain:** Pure C11 (`/TC /std:c11 /W4 /WX`), Win32 API, NMake (`Makefile` + `./build.sh`)  
**Destinatario:** Agente LLM di sviluppo / Software Engineer

---

## Capitolo 1: Obiettivi del Refactoring e Diagnosi dei Problemi

L'implementazione precedente del report statistiche (`v1.3.0` preliminare) utilizzava un file virtuale `!restic_stats.html` iniettato dentro ogni elenco directory. I test reali in Total Commander hanno evidenziato tre esigenze fondamentali che guidano questo refactoring:

### 1.1 Obiettivo 1: Eliminazione del file virtuale e attivazione diretta con `F3` / `Alt+Invio` sulla cartella
* **Requisito:** L'utente non vuole un file fittizio (`!restic_stats.html` o `!restic_stats.md`) elencato dentro le cartelle dei repository e degli snapshot. Il report deve aprirsi posizionando il cursore **direttamente sulla cartella** (un repository `\Alias`, uno snapshot `\Alias\Snapshot` o una sottocartella `\Alias\Snapshot\SubDir`) e premendo **`F3`** (oppure **`Alt+Invio`**, anche sulla riga `[..]`).
* **Vantaggi:**
  1. Il listing delle cartelle torna **puro al 100%** (nessun file artificiale in cima alla lista, nessun rischio di copiarlo con `F5` o di alterare il conteggio dei file in una directory vuota).
  2. Funziona anche al **Livello 0 (`\`)**: posizionandosi sul nome di un repository (es. `BackupLocale`) e premendo **`F3`** o **`Alt+Invio`**, si apre immediatamente il report globale di quel repository.
  3. Posizionandosi su uno snapshot in `\<Alias>` e premendo **`F3`**, si apre subito il report dello snapshot senza dover prima entrare nella cartella.

### 1.2 Obiettivo 2: Passaggio da HTML a Markdown (`.md`) con grafica ASCII
* **Problema riscontrato con l'HTML:** Nel Lister di Total Commander, i blocchi Unicode `█` (`U+2588`) e `░` (`U+2591`) toccano i bordi verticali della riga sovrapponendosi tra righe adiacenti e, in caso di font fallback, hanno larghezze diverse che rompono l'allineamento delle colonne. Inoltre il parser HTML Mode 5 di Total Commander altera spaziature e tag.
* **Soluzione:**
  1. Generare il report come file **Markdown puro (`.md`)** salvato in `%TEMP%` (es. `%TEMP%\restic_stats_<alias>_<id>.md`).
  2. Eliminare completamente HTML e CSS (`<!DOCTYPE html>`, `<style>`, `<pre>`, `<span>`, `&lt;`, `&quot;`) usando tabelle Markdown (`| ... |`) allineate con padding fisso monospazio.
  3. Sostituire i blocchi Unicode `█`/`░` con **caratteri ASCII puri a 1 byte**: **`#`** per la parte piena e **`.`** per la parte vuota (`[##########..........]`).

### 1.3 Obiettivo 3: Risoluzione della lentezza e del "loop" della barra di progresso con molte sottocartelle
L'analisi del codice sorgente (`src/main.c`, `src/restic_cli.c`, `src/restic_report.c`) ha individuato le **4 cause esatte** per cui la generazione del report su cartelle ricche di sottocartelle è lenta e mostra una barra di progresso che sembra ciclare all'infinito:

1. **Bug del reset a `0%` ogni 100 ms (`src/main.c`):**
   * Mentre `restic_report_generate()` invia percentuali crescenti (`5%`, `20%`, `45%`, `85%`) tramite `wfx_report_progress_callback`, ogni **100 ms** il loop di lettura pipe chiama `wfx_cancel_callback()`, che invoca `g_ProgressProc(g_PluginNr, virtual_path, repo->alias, 0)` passando **`0%`**!
   * La barra di progresso di Total Commander oscilla quindi 10 volte al secondo tra la percentuale reale e `0%`. Inoltre, superate $17.500$ righe in `subtree_line_callback`, `percent` resta fisso a `85%` (`percent <= ctx->last_percent`), `report_step()` smette di inviare aggiornamenti e `wfx_cancel_callback()` inchioda la barra a `0%`.
2. **Bug di `progressed == FALSE` e stalli di 20 ms su pipe da 4 KB (`src/restic_cli.c`):**
   * In `collect_stream_lines()` (`src/restic_cli.c`, righe 649-653):
     ```c
     size_t before_pending = stream->pending.len;
     ResticResult res = line_stream_drain(out_pipe, stream, &out_eof);
     if (stream->pending.len != before_pending) progressed = TRUE;
     ```
     Poiché `line_stream_feed()` compatta il buffer azzerando `stream->pending.len = 0` ogni volta che l'ultimo byte letto è `\n`, se `before_pending` era `0` e vengono lette righe complete, `stream->pending.len` a fine lettura è **ancora `0`**!
   * Di conseguenza `progressed` risulta **`FALSE`** anche mentre `restic.exe` sta inviando megabyte di NDJSON, e il ciclo entra in **`WaitForSingleObject(process, 20)` dormendo 20 ms ad ogni singolo blocco da 4 KB letto dalla pipe**! Su una cartella con molte sottocartelle questo bug introduce decine di secondi di puro `Sleep(20)`.
3. **Processi `restic.exe` ridondanti (`src/restic_report.c`):**
   * I report di Livello 1 e Livello 2 rilanciano `restic snapshots --json` anche quando i dati sono già nella cache RAM (`restic_stats.c`).
   * Ancora più costoso: lanciano **`restic stats --mode restore-size`**, che costringe `restic.exe` a scansionare ricorsivamente ogni *tree blob* di ogni sottocartella, quando la dimensione logica (`total_bytes_processed`) e il conteggio file (`total_files_processed`) sono **già presenti a costo zero** nel blocco `summary` di `ResticSnapshotInfo`.
4. **Assenza di cache in RAM per l'ultimo report generato:**
   * Riaprire `F3` sulla stessa cartella o snapshot riesegue da capo tutti i processi `restic.exe`.

---

## Capitolo 2: Architettura di Attivazione Diretta (`F3` e `Alt+Invio` su Cartelle)

### 2.1 Come intercettare `F3` su un Repository, Snapshot o Sottocartella
In Total Commander, i repository (`\<Alias>`), gli snapshot (`\<Alias>\<Snapshot>`) e le sottocartelle (`\<Alias>\<Snapshot>\<SubDir>`) hanno l'attributo `FILE_ATTRIBUTE_DIRECTORY`. Quando l'utente preme **`F3`** (o **`Spazio`**) con il cursore su una directory WFX, Total Commander **non** chiama `FsGetFileW`, ma esegue sul thread principale della GUI questa sequenza deterministica:

1. Chiama **`FsStatusInfoW(RemoteDir, FS_STATUS_START, FS_STATUS_OP_CALCSIZE)`**.
2. Immediatamente dopo chiama **`FsFindFirstW(TargetFolderPath, &FindData)`**, dove **`TargetFolderPath`** è il percorso completo della cartella su cui si trova il cursore (es. `\BackupLocale`, `\BackupLocale\2026-10-18_16-28-15_[5e395062]` oppure `\BackupLocale\2026-10-18_16-28-15_[5e395062]\D\data\projects`).
3. Al termine chiama **`FsStatusInfoW(RemoteDir, FS_STATUS_END, FS_STATUS_OP_CALCSIZE)`**.

Distinguendo la pressione di **`F3`** da quella di **`Spazio`** (tramite `(GetAsyncKeyState(VK_F3) & 0x8000) != 0` nel momento esatto in cui Total Commander invoca `FsStatusInfoW(..., FS_STATUS_START, FS_STATUS_OP_CALCSIZE)`):
* Se l'utente ha premuto **`Spazio`**, `VK_F3` non è premuto $\rightarrow$ Total Commander esegue il normale calcolo dello spazio occupato.
* Se l'utente ha premuto **`F3`**, il plugin attiva un flag thread-local (`tls_f3_calcsize_pending = TRUE`):
  1. Nella chiamata immediatamente successiva a `FsFindFirstW(Path, &FindData)`, il plugin consuma il flag, genera il report Markdown per `Path` (con barra di progresso `ProgressProcW` e supporto `ESC`) e apre il Lister nativo di Total Commander sul file `.md` generato.
  2. `FsFindFirstW` restituisce `INVALID_HANDLE_VALUE` con `SetLastError(ERROR_NO_MORE_FILES)`, facendo terminare istantaneamente `FS_STATUS_OP_CALCSIZE` senza attraversare alcuna sottocartella!

### 2.2 Come supportare anche `Alt+Invio` (e `[..]`) tramite `FsExecuteFileW`
Esportando `FsExecuteFileW` e `FsExecuteFile` (WFX SDK):
* Quando l'utente preme **`Alt+Invio`** su un repository, uno snapshot, una sottocartella oppure sulla riga **`[..]`** (mentre si trova già dentro una cartella), Total Commander chiama:
  ```c
  FsExecuteFileW(MainWin, RemoteName, L"properties");
  ```
* Quando l'utente digita **`stats`** nella riga di comando di Total Commander, TC chiama:
  ```c
  FsExecuteFileW(MainWin, RemoteName, L"quote stats");
  ```
* In entrambi i casi, se `RemoteName` punta a un repository, uno snapshot o una sottocartella valida, il plugin genera il report Markdown per `RemoteName`, apre il Lister nativo di Total Commander e restituisce **`FS_EXEC_OK` (`0`)**.
* Quando invece `Verb` è `L"open"` (es. l'utente preme `Invio` su un file normale per aprirlo), `FsExecuteFileW` restituisce **`FS_EXEC_YOURSELF` (`-1`)**, dicendo a Total Commander di scaricare e aprire il file normalmente.

---

### 2.3 Implementazione del Modulo `wfx_status.h` e `wfx_status.c`

Sostituire il contenuto di `include/wfx_status.h` e `src/wfx_status.c` con la gestione dell'intercettazione `F3` su `FS_STATUS_OP_CALCSIZE` (includendo un hook per i test automatici in cui `VK_F3` non è premuto fisicamente):

#### `include/wfx_status.h`
```c
#pragma once

#include <windows.h>

/* Maximum nesting depth of FS_STATUS_START/FS_STATUS_END pairs tracked per thread */
#define WFX_STATUS_STACK_MAX 8

void wfx_status_begin(int operation);
void wfx_status_end(void);
int  wfx_status_current_operation(void);
void wfx_status_reset(void);

/*
 * Returns TRUE once per F3-triggered FS_STATUS_OP_CALCSIZE scope when FsFindFirstW is called
 * for the selected folder. Subsequent calls within the same CALCSIZE scope return FALSE, and
 * wfx_status_is_f3_calcsize_aborted() returns TRUE until FS_STATUS_END.
 */
BOOL wfx_status_consume_f3_calcsize(void);
BOOL wfx_status_is_f3_calcsize_aborted(void);

/*
 * Test helper: when set to TRUE, the next FS_STATUS_OP_CALCSIZE start is treated as if VK_F3
 * was held down, allowing deterministic unit and DLL tests without physical keyboard input.
 */
void wfx_status_force_f3_next_calcsize(BOOL force);
```

#### `src/wfx_status.c`
```c
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include "fsplugin.h"
#include "wfx_status.h"
#include "wfx_trace.h"

static __declspec(thread) int  tls_operations[WFX_STATUS_STACK_MAX];
static __declspec(thread) int  tls_depth = 0;
static __declspec(thread) BOOL tls_f3_pending = FALSE;
static __declspec(thread) BOOL tls_f3_aborted = FALSE;
static __declspec(thread) BOOL tls_force_f3_next = FALSE;

void wfx_status_force_f3_next_calcsize(BOOL force) {
    tls_force_f3_next = force;
}

void wfx_status_begin(int operation) {
    if (tls_depth < WFX_STATUS_STACK_MAX) {
        tls_operations[tls_depth++] = operation;
    }
    if (operation == FS_STATUS_OP_CALCSIZE) {
        BOOL f3_down = tls_force_f3_next || ((GetAsyncKeyState(VK_F3) & 0x8000) != 0);
        tls_force_f3_next = FALSE;
        if (f3_down) {
            tls_f3_pending = TRUE;
            tls_f3_aborted = FALSE;
            wfx_trace_printf("F3 intercepted on FS_STATUS_OP_CALCSIZE");
        }
    }
}

void wfx_status_end(void) {
    if (tls_depth > 0) {
        int ended = tls_operations[--tls_depth];
        if (ended == FS_STATUS_OP_CALCSIZE) {
            tls_f3_pending = FALSE;
            tls_f3_aborted = FALSE;
        }
    } else {
        tls_f3_pending = FALSE;
        tls_f3_aborted = FALSE;
    }
}

int wfx_status_current_operation(void) {
    return (tls_depth > 0) ? tls_operations[tls_depth - 1] : 0;
}

BOOL wfx_status_consume_f3_calcsize(void) {
    if (tls_f3_pending && wfx_status_current_operation() == FS_STATUS_OP_CALCSIZE) {
        tls_f3_pending = FALSE;
        tls_f3_aborted = TRUE;
        return TRUE;
    }
    return FALSE;
}

BOOL wfx_status_is_f3_calcsize_aborted(void) {
    return tls_f3_aborted && wfx_status_current_operation() == FS_STATUS_OP_CALCSIZE;
}

void wfx_status_reset(void) {
    tls_depth         = 0;
    tls_f3_pending    = FALSE;
    tls_f3_aborted    = FALSE;
    tls_force_f3_next = FALSE;
}

static void apply_status(int start_end, int operation) {
    wfx_trace_printf("status %s operation=%d", start_end == FS_STATUS_START ? "start" : "end", operation);
    if (start_end == FS_STATUS_START) {
        wfx_status_begin(operation);
    } else if (start_end == FS_STATUS_END) {
        wfx_status_end();
    }
}

void __stdcall FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation) {
    (void)RemoteDir;
    apply_status(InfoStartEnd, InfoOperation);
}

void __stdcall FsStatusInfoW(WCHAR* RemoteDir, int InfoStartEnd, int InfoOperation) {
    (void)RemoteDir;
    apply_status(InfoStartEnd, InfoOperation);
}
```
*(Nota: linkare `User32.lib` in `Makefile` sotto `SYSLIBS = User32.lib` per `GetAsyncKeyState`, oppure risolvere `GetAsyncKeyState` o verificare `test_plugin_dll.c` — `User32.dll` è sempre caricata in qualsiasi processo GUI Windows come Total Commander).*

---

### 2.4 Avvio del Lister di Total Commander (`restic_report_open_in_lister`)

Aggiungere in `include/restic_report.h` e `src/restic_report.c`:
1. Una funzione `restic_report_build_temp_path(const WCHAR* alias, const char* snapshot_id, const WCHAR* subpath, WCHAR* out_path, size_t out_cap)` che costruisce un percorso deterministico in `%TEMP%`, ad esempio:
   * Repository: `%TEMP%\restic_stats_<alias>.md`
   * Snapshot: `%TEMP%\restic_stats_<alias>_<short_id>.md`
   * Cartella: `%TEMP%\restic_stats_<alias>_<short_id>_< sanitized_leaf >.md`
   *(Avere il nome parlante nel titolo del file fa sì che la barra del titolo del Lister di Total Commander mostri chiaramente a quale snapshot/cartella appartiene il report!)*
2. Una funzione `restic_report_open_in_lister(const WCHAR* local_report_path)`:
   * Se è registrata una callback/override per i test (`restic_report_set_lister_test_hook`) oppure una variabile d'ambiente `RESTIC_WFX_NO_LISTER_GUI=1` è attiva, registra l'apertura del file e ritorna `TRUE` **senza** lanciare processi grafici durante i test automatici.
   * In produzione in Total Commander:
     1. Legge la variabile d'ambiente `%COMMANDER_EXE%` (impostata nativamente da Total Commander col percorso completo di `TOTALCMD64.EXE` / `TOTALCMD.EXE`).
     2. Se `%COMMANDER_EXE%` non è definita, usa `GetModuleFileNameW(NULL, tc_exe, MAX_PATH)`.
     3. Costruisce la riga di comando:
        ```c
        "\"<tc_exe>\" /S=L \"<local_report_path>\""
        ```
     4. Avvia il processo con `CreateProcessW` (senza attendere la chiusura della finestra Lister, chiudendo subito `pi.hProcess` e `pi.hThread`).

---

### 2.5 Integrazione in `src/main.c` (`FsFindFirstW` e `FsExecuteFileW`)

#### A. Rimozione totale del file virtuale dalle directory
In `src/main.c`:
1. **Rimuovere** `inject_stats_report_item()` da `execute_repo_query()`.
2. **Rimuovere** il blocco `wfx_path_strip_report_segment` da `FsFindFirstW()` e da `FsGetFileW()`.
3. `FsGetFileW()` torna ad occuparsi esclusivamente dell'estrazione di file reali con `restic_dump_file()`.

#### B. Funzione condivisa `show_stats_report_for_path(const WCHAR* virtual_path)` in `src/main.c`
Scrivere in `src/main.c` un helper `static BOOL show_stats_report_for_path(const WCHAR* virtual_path)` che:
1. Analizza `virtual_path` con `wfx_parse_path(virtual_path, &parsed)`.
   * Se `parsed.level != WFX_PATH_REPO && parsed.level != WFX_PATH_SNAPSHOT_DIR`, ritorna `FALSE` (la root `\` non ha un singolo repository).
2. Carica la configurazione (`config_load`), trova `repo = config_find_repo(&cfg, parsed.repo_alias)` e verifica che `cfg.restic_path` sia valido.
3. Determina il percorso locale del report in `%TEMP%` tramite `restic_report_build_temp_path(repo->alias, snapshot_id, subpath, local_report_path, _countof(local_report_path))`.
4. Chiama `g_ProgressProc(g_PluginNr, (WCHAR*)virtual_path, local_report_path, 0)` (se ritorna `!= 0`, l'utente ha annullato).
5. Esegue il ciclo di autenticazione password (identico a `execute_repo_query` / `FsGetFileW`, riusando `session_get` / `prompt_repository_password` / `session_put`) invocando:
   ```c
   res = restic_report_generate(
       cfg.restic_path,
       repo->target,
       password,
       repo->alias,
       snapshot_id,
       subpath,
       local_report_path,
       &report_exec_opts,
       wfx_report_progress_callback,
       &report_prog_ctx,
       err_msg,
       _countof(err_msg)
   );
   ```
6. Chiude la progress bar (`g_ProgressProc(g_PluginNr, (WCHAR*)virtual_path, local_report_path, 100)`).
7. Se `res == RESTIC_OK`, lancia il Lister con `restic_report_open_in_lister(local_report_path)` e ritorna `TRUE`. Se `res != RESTIC_OK` (e non `RESTIC_ERR_CANCELLED`), mostra l'errore con `handle_restic_failure()`.

#### C. Intercettazione `F3` dentro `FsFindFirstW` (`src/main.c`)
All'inizio di `FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData)`:
```c
    /* Se un calcolo spazio (CALCSIZE) attivato da F3 è già stato intercettato,
       ignora qualsiasi ulteriore chiamata nello stesso ciclo CALCSIZE */
    if (wfx_status_is_f3_calcsize_aborted()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    /* Se l'utente ha premuto F3 su una cartella/snapshot/repository, intercetta CALCSIZE,
       genera il report Markdown e aprilo nel Lister invece di calcolare lo spazio */
    if (wfx_status_consume_f3_calcsize()) {
        (void)show_stats_report_for_path(Path);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }
```

#### D. Esportazione di `FsExecuteFileW` e `FsExecuteFile` (`src/main.c`, `include/fsplugin.h`, `restic_wfx.def`)
Aggiungere in `include/fsplugin.h`, `restic_wfx.def` e `src/main.c`:
```c
int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    (void)MainWin;
    (void)RemoteName;
    if (Verb && _stricmp(Verb, "open") == 0) {
        return FS_EXEC_YOURSELF;
    }
    return FS_EXEC_ERROR;
}

int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb) {
    (void)MainWin;
    if (!RemoteName || !Verb) return FS_EXEC_ERROR;

    /* Invio su un file normale -> Total Commander lo scarica e lo apre normalmente */
    if (_wcsicmp(Verb, L"open") == 0) {
        return FS_EXEC_YOURSELF;
    }

    /* Alt+Invio (Verb == "properties") su cartella/snapshot/repo/[..] oppure comando "quote stats" */
    if (_wcsicmp(Verb, L"properties") == 0 ||
        _wcsnicmp(Verb, L"quote stats", 11) == 0) {
        if (show_stats_report_for_path(RemoteName)) {
            return FS_EXEC_OK;
        }
        return FS_EXEC_ERROR;
    }

    return FS_EXEC_ERROR;
}
```

---

## Capitolo 3: Fix #1 — Correzione del "Loop" della Progress Bar (`src/main.c` e `src/restic_report.c`)

### 3.1 Unificazione del Contesto di Progresso e Cancellazione in `src/main.c`

Come diagnosticato nel Capitolo 1.3, durante la generazione del report non bisogna **mai** usare `wfx_cancel_callback` (che passa `PercentDone = 0` ogni 100 ms azzerando la barra di avanzamento), ma una coppia di callback che condividono l'ultima percentuale nota (`current_percent`):

```c
typedef struct {
    const WCHAR* remote_name;
    const WCHAR* local_name;
    volatile LONG current_percent; /* Mantiene l'ultima percentuale inviata (evita il reset a 0%) */
} WfxReportProgressContext;

static BOOL wfx_report_progress_callback(int percent, void* user_data) {
    if (!g_ProgressProc) return FALSE;
    WfxReportProgressContext* ctx = (WfxReportProgressContext*)user_data;
    if (ctx) {
        InterlockedExchange(&ctx->current_percent, (LONG)percent);
    }
    WCHAR* src = (ctx && ctx->remote_name) ? (WCHAR*)ctx->remote_name : L"";
    WCHAR* dst = (ctx && ctx->local_name) ? (WCHAR*)ctx->local_name : L"";
    return g_ProgressProc(g_PluginNr, src, dst, percent) != 0;
}

static BOOL wfx_report_cancel_callback(void* user_data) {
    if (!g_ProgressProc) return FALSE;
    const WfxReportProgressContext* ctx = (const WfxReportProgressContext*)user_data;
    WCHAR* src = (ctx && ctx->remote_name) ? (WCHAR*)ctx->remote_name : L"";
    WCHAR* dst = (ctx && ctx->local_name) ? (WCHAR*)ctx->local_name : L"";
    int pct    = ctx ? (int)InterlockedCompareExchange((volatile LONG*)&ctx->current_percent, 0, 0) : 0;
    return g_ProgressProc(g_PluginNr, src, dst, pct) != 0;
}
```

### 3.2 Throttling Temporale in `subtree_line_callback` (`src/restic_report.c`)

In `src/restic_report.c`, aggiungere `ULONGLONG last_progress_tick;` a `SubtreeScan` e limitare le chiamate a `progress()` durante `restic ls --recursive` ad **al massimo una ogni 150 ms**:

```c
    scan->line_count++;
    if ((scan->line_count & 0xFFULL) == 0) {
        ULONGLONG now = GetTickCount64();
        if (now - scan->last_progress_tick >= 150ULL) {
            scan->last_progress_tick = now;
            int percent = 15 + (int)(scan->line_count / 500ULL);
            if (percent > 85) percent = 85;
            if (scan->ctx->progress != NULL) {
                scan->ctx->last_percent = percent;
                if (scan->ctx->progress(percent, scan->ctx->progress_ctx)) {
                    return FALSE; /* ESC premuto */
                }
            }
        }
    }
```

---

## Capitolo 4: Fix #2 — Ottimizzazione Pipe Win32 e Rimozione Stalli di 20 ms (`src/restic_cli.c`)

### 4.1 Buffer Pipe da 64 KB in `spawn_hidden()`
In `src/restic_cli.c`, dentro `spawn_hidden()`, impostare la dimensione del buffer della pipe `stdout` a **64 KB (`65536`)**:
```c
    if (!CreatePipe(&out_r, &out_w, &sa, 65536) || !CreatePipe(&err_r, &err_w, &sa, 0)) goto done;
```

### 4.2 Correzione del Bug di `progressed` in `collect_stream_lines()` e Inactivity Timeout
In `src/restic_cli.c`, modificare `line_stream_drain()` affinché segnali esplicitamente se ha letto byte dalla pipe (`BOOL* out_read_any`), evitando che la compattazione di `stream->pending.len` a `0` inganni `collect_stream_lines()` facendogli eseguire `WaitForSingleObject(process, 20)` mentre i dati stanno scorrendo:

```c
static ResticResult line_stream_drain(
    HANDLE pipe,
    LineStream* stream,
    BOOL* eof,
    BOOL* out_read_any
) {
    char chunk[16384];
    *out_read_any = FALSE;
    while (!*eof) {
        DWORD avail = 0;
        DWORD got   = 0;
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL)) {
            *eof = TRUE;
            break;
        }
        if (avail == 0) break;
        if (!ReadFile(pipe, chunk, avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk), &got, NULL) ||
            got == 0) {
            *eof = TRUE;
            break;
        }
        *out_read_any = TRUE;
        ResticResult res = line_stream_feed(stream, chunk, got);
        if (res != RESTIC_OK) return res;
    }
    return RESTIC_OK;
}
```
 *(Nota per `/analyze` C6262: se `chunk[16384]` supera il budget di stack della funzione, mantenere `char chunk[4096]` ma con il flag `*out_read_any = TRUE` — poiché il ciclo `while (!*eof)` continua a svuotare la pipe finché `avail > 0`, un `chunk[4096]` svuota comunque tutti i 64 KB in memoria senza alcuna attesa!)*

E in `collect_stream_lines()`:
```c
static ResticResult collect_stream_lines(
    HANDLE process,
    HANDLE out_pipe,
    HANDLE err_pipe,
    const ResticExecOptions* options,
    LineStream* stream,
    ByteBuf* err
) {
    ULONGLONG last_activity = GetTickCount64();
    ULONGLONG last_poll     = last_activity;
    BOOL out_eof = FALSE, err_eof = FALSE;

    for (;;) {
        BOOL out_read = FALSE;
        ResticResult res = line_stream_drain(out_pipe, stream, &out_eof, &out_read);
        if (res != RESTIC_OK) return res;

        size_t before_err = err->len;
        if (!drain_pipe(err_pipe, err, &err_eof)) return RESTIC_ERR_OUT_OF_MEMORY;
        BOOL progressed = out_read || (err->len != before_err);

        if (progressed) {
            last_activity = GetTickCount64();
        } else if (WaitForSingleObject(process, 1) == WAIT_OBJECT_0) {
            res = line_stream_drain(out_pipe, stream, &out_eof, &out_read);
            if (res != RESTIC_OK) return res;
            if (!drain_pipe(err_pipe, err, &err_eof)) return RESTIC_ERR_OUT_OF_MEMORY;
            return RESTIC_OK;
        }

        if (options) {
            ULONGLONG now = GetTickCount64();
            if (options->timeout_ms != 0 && now - last_activity >= options->timeout_ms) {
                return RESTIC_ERR_TIMEOUT;
            }
            if (options->cancel_proc && now - last_poll >= CANCEL_POLL_MS) {
                last_poll = now;
                if (options->cancel_proc(options->cancel_ctx)) return RESTIC_ERR_CANCELLED;
            }
        }
    }
}
```

---

## Capitolo 5: Fix #3 — Eliminazione dei Processi `restic.exe` Ridondanti e Cache Report (`src/restic_report.c`)

### 5.1 Riuso della Cache `ResticSnapshotInfo` ed Eliminazione di `stats --mode restore-size`

1. **In `ReportContext` (`src/restic_report.c`):** salvare anche `const WCHAR* alias_w;`.
2. **In `generate_repo_report()` (Livello 1):**
   * Controllare prima la cache RAM con `snapshot_stats_lookup_alias(ctx->alias_w, NULL, 0, &cached_count)`. Se `cached_count > 0`, copiare i record direttamente dalla cache senza lanciare `restic snapshots --json`. Se la cache è fredda, eseguire `load_snapshots()` e popolare la cache con `snapshot_stats_store(ctx->alias_w, snapshots, snapshot_count)`.
   * Se almeno uno snapshot ha `has_summary == TRUE`, calcolare `report.restore.total_size` e `report.restore.total_file_count` dai campi `summary` (usando l'ultimo snapshot o la somma/massimo senza lanciare `restic stats --mode restore-size`, che su un repository con molti snapshot e sottocartelle è lentissimo!). Eseguire `read_storage_stats(ctx, L"restore-size", ...)` **solo se** nessun snapshot ha `has_summary`.
   * Eseguire `read_storage_stats(ctx, L"raw-data", NULL, &report.stored, ...)` per ottenere la dimensione fisica deduplicata e compressa su disco.
3. **In `generate_snapshot_report()` (Livello 2):**
   * Cercare prima lo snapshot nella cache RAM tramite `snapshot_stats_lookup(ctx->alias_w, snapshot_id, &cached_info)`. Se presente, usarlo direttamente senza lanciare `restic snapshots --json <id>`.
   * Se `info->has_summary` è `TRUE`, popolare direttamente:
     ```c
     report.restore.valid            = TRUE;
     report.restore.total_size       = report.info->total_bytes_processed;
     report.restore.total_file_count = report.info->total_files_processed;
     ```
     saltando completamente il processo `restic stats --mode restore-size <id>`!
   * Eseguire solo `read_storage_stats(ctx, L"raw-data", snapshot_w, &report.stored, ...)` e `read_diff(...)`.

### 5.2 Cache in RAM dell'Ultimo Report Generato (TTL 60 secondi)
In `src/restic_report.c`, aggiungere una cache statica protetta da `SRWLOCK` (`g_ReportCacheLock = SRWLOCK_INIT`) che memorizza l'ultimo report Markdown generato (chiave: `repo_target + snapshot_id + subpath_w`, contenuto `char* markdown`, timestamp `ULONGLONG tick`).
* Se `restic_report_generate()` viene invocata per la stessa chiave entro `60000` ms (60 secondi), scrive direttamente `cached_markdown` nel file di destinazione e ritorna `RESTIC_OK` in **0 ms**.
* Esporre `void restic_report_clear_cache(void);` e chiamarla in `FsInitW`, `FsContentPluginUnloading` e `DllMain(DLL_PROCESS_DETACH)`.

---

## Capitolo 6: Fix #4 — Formato Markdown Puro e Grafica ASCII (`src/restic_report.c`)

### 6.1 Barre Progressive in Puro ASCII (`[##########..........]`)
In `src/restic_report.c`, sostituire `BAR_BLOCK_UTF8` e `BAR_EMPTY_UTF8` con caratteri ASCII a 1 byte:
```c
#define BAR_FILLED_CHAR '#'
#define BAR_EMPTY_CHAR  '.'

void restic_report_format_bar(char* out, size_t out_capacity, double ratio, int width) {
    if (out == NULL || out_capacity == 0) return;
    out[0] = '\0';
    if (width <= 0) width = 1;
    if (width > 40) width = 40;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;

    int filled = (int)(ratio * (double)width + 0.5);
    if (ratio > 0.0 && filled == 0) filled = 1;
    if (filled > width) filled = width;

    size_t pos = 0;
    if (pos + 1 < out_capacity) out[pos++] = '[';
    for (int i = 0; i < width; ++i) {
        if (pos + 2 >= out_capacity) break;
        out[pos++] = (i < filled) ? BAR_FILLED_CHAR : BAR_EMPTY_CHAR;
    }
    if (pos + 1 < out_capacity) out[pos++] = ']';
    out[pos] = '\0';
}
```

### 6.2 Sanitizzazione Testo per Tabelle Markdown (`restic_report_escape_write`)
Poiché l'output è Markdown (letto sia come testo monospazio nel Lister di Total Commander sia in eventuali visualizzatori Markdown), `restic_report_escape_write()` non deve emettere entità HTML (`&lt;`, `&quot;`, `&amp;`), ma deve solo impedire che un carattere `|` o un a-capo dentro un nome di file rompa la tabella Markdown:
```c
void restic_report_escape_write(FILE* fp, const char* text) {
    if (fp == NULL) return;
    if (text == NULL) return;
    for (const char* p = text; *p != '\0'; ++p) {
        char c = *p;
        if (c == '|') {
            fputc('/', fp);
        } else if (c == '\r' || c == '\n' || c == '\t') {
            fputc(' ', fp);
        } else {
            fputc((unsigned char)c, fp);
        }
    }
}
```

### 6.3 Struttura Markdown dei 3 Report
I tre writer (`restic_report_write_repo_html`, `restic_report_write_snapshot_html`, `restic_report_write_subtree_html` — i cui nomi possono rimanere invariati nell'header per compatibilità coi test o avere alias `_md`) devono produrre documenti Markdown puliti, senza alcun tag HTML:

1. **Intestazione documento (`write_md_begin`):**
   ```markdown
   # Restic repository report — <alias>

   | Property                     | Value |
   | ---------------------------- | ----- |
   | Repository                   | <alias> |
   | Location                     | <redacted_target> |
   ...
   ```
   *(Usare `write_labeled(FILE* fp, const char* label, const char* value)` che formatta `| %-28s | <value> |\n` così le colonne sono allineate anche in modalità testo puro).*
2. **Sezioni (`<h2>` $\rightarrow$ `## <Titolo Sezione>`):**
   * `## Storage and deduplication`
   * `## Snapshot history` (Livello 1)
   * `## Files and directories` (Livello 2)
   * `## Changes since the parent snapshot` (Livello 2, con la lista dei file modificati racchiusa in un blocco ```` ```text ... ``` ```` e prefissi `[+]`, `[-]`, `[M]` senza `<span>`)
   * `## Size by subfolder` (Livello 3)
   * `## Largest file types` (Livello 3)
   * `## File size distribution` (Livello 3)
   * `## Largest files` (Livello 3)
3. **Tutte le righe di tabella** iniziano e finiscono con `|` (es. `| %-10s | %6s | %6s | %-22s | <name> |\n`).

---

## Capitolo 7: Piano di Aggiornamento Test, Export Surface e Documentazione

### 7.1 Export Surface della DLL (`restic_wfx.def` e `tests/test_plugin_dll.c`)
1. In `restic_wfx.def`, aggiungere `FsExecuteFile` e `FsExecuteFileW` (totale: **25 funzioni esportate**).
2. In `tests/test_plugin_dll.c` (`test_export_surface`):
   * Spostare `"FsExecuteFile"` dall'array `forbidden[]` all'array `required[]` e aggiungere `"FsExecuteFileW"` in `required[]`.
   * Aggiungere `PFN_ExecuteFile` e `PFN_ExecuteFileW` nella struct `Plugin`.

### 7.2 Aggiornamento dei Test (`tests/test_report.c` e `tests/test_plugin_dll.c`)
1. **In `tests/test_report.c`:**
   * Aggiornare `test_unicode_bar_formatting()` per verificare `'#'` e `'.'`.
   * Aggiornare `test_html_escaping()` per verificare la sostituzione di `'|'` con `'/'` e `'\n'` con `' '`, senza entità `&lt;`.
   * Aggiornare `test_status_info_filtering()` per testare `wfx_status_force_f3_next_calcsize(TRUE)`, `wfx_status_consume_f3_calcsize()` e `wfx_status_is_f3_calcsize_aborted()`.
   * Aggiornare `test_html_report_generation()` e `test_live_report_generation()` per verificare l'output Markdown (`"# Restic repository report"`, `"## Storage and deduplication"`, barre `#`, assenza di `<!DOCTYPE html>` e `<script>`).
2. **In `tests/test_plugin_dll.c`:**
   * Ripristinare i conteggi delle directory (poiché `!restic_stats.html` non viene più iniettato nelle liste file: es. `entries == 2` in `mt_list_worker`, e `empty_dir` torna a restituire direttamente `INVALID_HANDLE_VALUE` con `ERROR_NO_MORE_FILES` anche durante la navigazione normale!).
   * In `test_e2e_stats_report()`:
     * Attivare l'hook di test del Lister (così non si aprono finestre grafiche durante i test).
     * Testare la pressione di **`F3`** su `\MainRepo`, su `snapshot_root` e su `data_path` eseguendo la sequenza reale di Total Commander:
       ```c
       wfx_F3_simulation:
       p->status_info_w(path, FS_STATUS_START, FS_STATUS_OP_CALCSIZE);
       HANDLE h = p->find_first_w(path, &fd); /* ritorna INVALID_HANDLE_VALUE e genera il report .md */
       p->status_info_w(path, FS_STATUS_END, FS_STATUS_OP_CALCSIZE);
       ```
       e verificando che il file `.md` generato contenga `"# Restic repository report"`, `"# Restic snapshot report"` e `"# Restic folder report"`.
     * Testare anche `p->execute_file_w(NULL, data_path, L"properties") == FS_EXEC_OK` (`Alt+Invio`) e `p->execute_file_w(NULL, readme_path, L"open") == FS_EXEC_YOURSELF`.

### 7.3 Checklist Finale di Validazione
1. Aggiornare eventuali pattern di mutazione in `tmp/mut_check.sh` / `tmp/check_mutations.py`.
2. Eseguire `./build.sh clean && ./build.sh test` (0 leak, tutte le suite verdi).
3. Eseguire `./build.sh analyze` (0 warning MSVC `/analyze`).
4. Eseguire `./build.sh asan` (0 errori AddressSanitizer).
5. Eseguire `./build.sh release-test`.
6. Aggiornare `AGENT.md`, `README.md`, `CHANGELOG.md` e `MEMORY.md` documentando l'apertura diretta delle statistiche con `F3` / `Alt+Invio` sulle cartelle e il formato Markdown.