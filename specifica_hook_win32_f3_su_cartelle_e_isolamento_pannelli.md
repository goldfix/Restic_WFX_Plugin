# Specifica Tecnica: Supporto `F3` su Cartelle tramite Hook Win32 In-Process e Isolamento Pannelli (`v1.3.2`)

## 1. Contesto e Obiettivo

Dai test live effettuati su **Total Commander 11.58 x64** (`2026-10-31`, documentati in `MEMORY.md`) è emerso che:
1. Quando il cursore si trova su una cartella (`FILE_ATTRIBUTE_DIRECTORY`) all'interno del plugin WFX (`\\\Restic Backups\...`), la pressione di **`Alt+Invio`** (`FsExecuteFileW` con `Verb="properties"`) e il comando **`stats`** (`Verb="quote stats"`) funzionano correttamente e aprono il report Markdown nel Lister.
2. Quando invece si preme **`F3`** su una cartella del plugin, Total Commander blocca l'operazione a monte mostrando il popup nativo `"No files selected!"` (*"Nessun file selezionato!"*), **senza invocare alcuna callback WFX** (`FsStatusInfoW`, `FsFindFirstW` o `FsGetFileW`). Di conseguenza, l'euristica basata su `FS_STATUS_OP_CALCSIZE` introdotta in `v1.3.1` non viene mai raggiunta.
3. Inoltre, quando il Lister viene aperto su un file `.md` tramite `/O /S=L`, eventuali plugin Lister di terze parti installati in Total Commander (es. `sumatrapdf.wlx64`) possono intercettare l'estensione `.md` invece di lasciare il rendering al visualizzatore di testo UTF-8 nativo del Lister.

### Obiettivo dell'intervento (`v1.3.2`)
Implementare un meccanismo **In-Process Win32 Hook (`WH_KEYBOARD` + `WH_CBT`)** confinato al thread UI di Total Commander che:
1. **Renda funzionante `F3` su tutte le cartelle del plugin** (Repository a livello 0, Snapshot a livello 1, Sottocartelle a livello 2+), aprendo il report Markdown nel Lister senza far apparire il popup `"No files selected!"`.
2. **Garantisca l'isolamento totale fuori dal plugin (*Path Guard*)**: se il pannello attivo (quello che ha il focus della tastiera) si trova su una normale cartella locale (`C:\...`, `D:\...`), su una share di rete, in un archivio `.zip` o in un altro plugin — **anche se l'altro pannello inattivo mostra ancora `\\\Restic Backups\...`** — il plugin Restic **non deve intervenire in alcun modo** e `F3` deve mantenere il comportamento nativo di Total Commander.
3. **Preservi `F3` sui file normali dentro gli snapshot**: se il cursore è su un file normale dentro uno snapshot, `F3` deve continuare a scaricare e aprire il file nel Lister tramite `FsGetFileW`.
4. **Forzi la modalità testo UTF-8 nativa nel Lister (`/S=L:T7`)** per i report `.md`, evitando che plugin WLX esterni (come `sumatrapdf`) intercettino il report.
5. **Rimuova il codice morto** relativo a `FS_STATUS_OP_CALCSIZE` e `restic_test_force_f3`.

---

## 2. Architettura della Soluzione: Hook In-Process + Path Guard

Poiché `restic.wfx64` è una DLL caricata *in-process* dentro `TOTALCMD64.EXE`, possiamo installare due hook **thread-local** (associati esclusivamente a `GetCurrentThreadId()` del thread principale di Total Commander, `hMod = NULL`, zero iniezione di sistema):
- `SetWindowsHookExW(WH_KEYBOARD, restic_keyboard_hook_proc, NULL, main_tid)`
- `SetWindowsHookExW(WH_CBT, restic_cbt_hook_proc, NULL, main_tid)`

```
                 [ Pressione tasto F3 (VK_F3) in Total Commander ]
                                        │
                                        ▼
             ┌─────────────────────────────────────────────────────┐
             │ 1. Controllo Modificatori e Stato Tasto             │
             │    - Solo KeyDown ((lParam & 0x80000000) == 0)      │
             │    - Nessun Shift / Ctrl / Alt premuto              │
             └──────────────────────────┬──────────────────────────┘
                                        │
                                        ▼
             ┌─────────────────────────────────────────────────────┐
             │ 2. PATH GUARD (Isolamento Pannello Attivo)          │
             │    Il pannello con il focus (GetFocus) appartiene   │
             │    a \\\Restic Backups\... ?                        │
             └───────────────┬─────────────────────────┬───────────┘
                          NO │                         │ SÌ
                             ▼                         ▼
        ┌────────────────────────────────┐   ┌──────────────────────────────────────┐
        │ IGNORA IMMEDIATAMENTE          │   │ 3. In quale livello del plugin siamo?│
        │ CallNextHookEx(...)            │   └─────────┬──────────────────┬─────────┘
        │ TC gestisce F3 normalmente     │             │                  │
        │ sulle cartelle C:\, D:\, ecc.  │    Livello 0 o 1      Livello 2+ (Snapshot)
        └────────────────────────────────┘  (Solo cartelle:      (Cartelle + File)
                                            Repo / Snapshot)              │
                                                       │                  ▼
                                                       │   ┌────────────────────────────────┐
                                                       │   │ Arma guardia temporale (150ms):│
                                                       │   │ g_f3_armed_tick = Now()        │
                                                       │   │ g_f3_file_op_seen = FALSE      │
                                                       │   │ Lascia passare VK_F3 a TC      │
                                                       │   └──────┬──────────────────┬──────┘
                                                       │          │                  │
                                                       │     Su un FILE        Su una CARTELLA
                                                       │          │                  │
                                                       │          ▼                  ▼
                                                       │   ┌─────────────┐  ┌─────────────────────────┐
                                                       │   │ TC chiama   │  │ TC tenta di creare il   │
                                                       │   │ FsGetFileW: │  │ MessageBox (#32770)     │
                                                       │   │ disarma hook│  │ "No files selected!":   │
                                                       │   │ e apre file │  │ WH_CBT intercetta       │
                                                       │   └─────────────┘  │ HCBT_CREATEWND, annulla │
                                                       │                    │ il popup (return 1)     │
                                                       │                    └────────┬────────────────┘
                                                       ▼                             │
                                            ┌────────────────────────────────────────▼──────┐
                                            │ 4. Invia comando Proprietà al pannello attivo │
                                            │    PostMessageW(hwndMain, WM_USER+51, 510, 0) │
                                            │    (cm_VersionInfo = Alt+Invio nativo di TC)  │
                                            │    -> TC invoca FsExecuteFileW("properties")  │
                                            │    -> Si apre il report Markdown nel Lister!  │
                                            └───────────────────────────────────────────────┘
```

---

## 3. Dettaglio Tecnico dei Componenti

### 3.1. Come funziona il *Path Guard* (Isolamento al 100% delle cartelle normali)

Il requisito più critico è che **quando il focus si trova in un pannello con una cartella normale (es. `C:\AMD`), il plugin non deve mai entrare in funzione**, nemmeno se l'altro pannello di Total Commander (inattivo) si trova ancora dentro `\\\Restic Backups\...`.

In Total Commander 11.x x64 (finestra principale di classe `TTotal_CMD`), l'interfaccia utente offre **tre indicatori convergenti** per determinare con certezza assoluta il percorso del **solo pannello attivo**:

1. **Verifica del controllo con focus (`GetFocus()`)**:
   - Quando l'utente naviga tra file e cartelle e preme `F3`, il focus della tastiera (`HWND hwndFocus = GetFocus()`) appartiene alla listbox del pannello attivo (classe `LCLListbox` o `TMyListBox`).
   - Se il focus è nella riga di comando (`Edit`), in un box di ricerca o in una finestra di dialogo, `hwndFocus` non è una listbox di pannello e l'hook esce subito.

2. **Indicatore 1 — Barra del prompt dei comandi di Total Commander**:
   - Total Commander mantiene in basso (accanto alla casella di testo della riga di comando) un controllo figlio il cui testo (`GetWindowTextW`) riporta **sempre e solo il percorso del pannello attualmente attivo** seguito dal carattere `>`:
     - Se il pannello attivo è nel plugin: `\\\Restic Backups\>` oppure `\\\Restic Backups\Locale\>`
     - Non appena l'utente preme `Tab` o clicca sull'altro pannello (es. `C:\AMD`), il testo di questo controllo cambia istantaneamente in: `c:\AMD>`

3. **Indicatore 2 — Allineamento geometrico (`GetWindowRect`) tra `GetFocus()` e l'intestazione del pannello**:
   - Sopra ciascuno dei due pannelli file (sinistro e destro) si trova la barra del percorso corrente di quel pannello, il cui testo contiene il percorso con la maschera (es. `\\\Restic Backups\*.*` a sinistra e `c:\AMD\*.*` a destra).
   - Calcolando la coordinata X centrale del controllo che ha il focus (`focus_center_x = (rcFocus.left + rcFocus.right) / 2`), possiamo leggere il testo della sola barra del percorso che si trova **sullo stesso lato (sinistro o destro)** di `hwndFocus` (`rcChild.left <= focus_center_x && focus_center_x <= rcChild.right`).

Combinando questi controlli in `restic_f3_get_active_panel_plugin_path()`:
- Se il pannello attivo **non** inizia con `\\\Restic Backups\` (case-insensitive), la funzione restituisce `FALSE` e `WH_KEYBOARD` chiama semplicemente `CallNextHookEx` senza armare nulla.
- Se il pannello attivo inizia con `\\\Restic Backups\`, la funzione restituisce `TRUE` e popola il percorso corrente del pannello attivo, permettendo anche di sapere a quale profondità (`depth`) ci troviamo:
  - `depth == 0`: `\\\Restic Backups\` (elenco Repository — esistono **solo cartelle**)
  - `depth == 1`: `\\\Restic Backups\<Repo>\` (elenco Snapshot — esistono **solo cartelle**)
  - `depth >= 2`: `\\\Restic Backups\<Repo>\<Snapshot>\...` (dentro uno Snapshot — esistono **sia cartelle sia file**)

---

### 3.2. Cosa succede nei Livelli 0/1 vs Livello 2+

1. **Livello 0 (`\\\Restic Backups\`) e Livello 1 (`\\\Restic Backups\<Repo>\`)**:
   - Poiché in questi livelli non esistono mai file ma solo cartelle (Repository e Snapshot), non c'è alcun bisogno di lasciar proseguire `VK_F3` fino al tentativo di apertura del `MessageBox`.
   - `restic_keyboard_hook_proc` intercetta direttamente `VK_F3`, invia il comando di apertura proprietà a Total Commander (`PostMessageW(hwndMain, WM_USER + 51, 510, 0)`, dove `510` è `cm_VersionInfo`, cioè l'azione nativa di `Alt+Invio` in Total Commander) e restituisce `1` per sopprimere `VK_F3`.
   - Risultato: risposta istantanea, nessun popup tentato.

2. **Livello 2+ (`\\\Restic Backups\<Repo>\<Snapshot>\...`)**:
   - Qui possono esserci sia sottocartelle sia file regolari.
   - Quando viene premuto `VK_F3`:
     - `restic_keyboard_hook_proc` arma una finestra temporale stretta (`g_f3_armed_tick = GetTickCount64()`, valida per `150 ms`, con `g_f3_file_op_seen = FALSE`) e lascia proseguire `VK_F3` a Total Commander (`CallNextHookEx`).
   - **Caso A — Il cursore è su un FILE**:
     - Total Commander chiama immediatamente (sullo stesso thread, entro pochi millisecondi) `FsStatusInfoW(..., FS_STATUS_START, FS_STATUS_OP_GET_SINGLE)` seguito da `FsGetFileW`.
     - All'ingresso di `FsStatusInfoW` (per `FS_STATUS_OP_GET_SINGLE` / `FS_STATUS_OP_GET_MULTI`) e di `FsGetFileW`, il plugin chiama `restic_f3_hook_disarm()`, che azzera `g_f3_armed_tick = 0` e imposta `g_f3_file_op_seen = TRUE`.
     - Il file viene scaricato e aperto normalmente nel Lister.
   - **Caso B — Il cursore è su una SOTTOCARTELLA**:
     - Total Commander non chiama `FsGetFileW` e invoca invece `MessageBoxW` (`"No files selected!"`) sullo stesso thread.
     - Prima che la finestra del `MessageBox` (`#32770`) venga mostrata o emetta alcun suono, Windows chiama `restic_cbt_hook_proc` con `nCode == HCBT_CREATEWND`.
     - `restic_cbt_hook_proc` verifica che:
       1. `g_f3_armed_tick != 0` e `(GetTickCount64() - g_f3_armed_tick) <= 150`
       2. `!g_f3_file_op_seen`
       3. La finestra in creazione sia un dialogo top-level (`#32770` / popup di `hwndMain`).
     - A quel punto `restic_cbt_hook_proc`:
       1. Disarma `g_f3_armed_tick = 0`.
       2. Invia `PostMessageW(hwndMain, WM_USER + 51, 510, 0)` (`cm_VersionInfo` / `Alt+Invio`) per far invocare a Total Commander `FsExecuteFileW(MainWin, RemoteName, L"properties")` sulla sottocartella selezionata.
       3. Restituisce `1` da `HCBT_CREATEWND`, annullando la creazione del `MessageBoxW` prima ancora che compaia a schermo!

> **Nota su `WM_USER + 51` (`1075`) con `wParam = 510` (`cm_VersionInfo`) vs `SendInput` (`Alt+Invio`)**:
> In Total Commander, il messaggio `WM_USER + 51` (`1075`) è il canale ufficiale per eseguire comandi interni (`TOTALCMD.INC`), e `510` (`cm_VersionInfo`) è esattamente il comando associato ad `Alt+Invio` (*File -> Properties*). Usare `PostMessageW(hwndMain, 1075, 510, 0)` evita di dover simulare la pressione fisica del tasto `Alt` (`VK_MENU`), prevenendo qualsiasi conflitto con lo stato della tastiera dell'utente. Come fallback (o se in fase di test con `tc_probe.py` emergesse un caso in cui `1075` non risponde), è possibile inviare la sequenza `Alt+Invio` tramite `SendInput`.

---

## 4. Piano di Implementazione Passo-Passo

### Step 1: Verifica preliminare delle classi finestra di Total Commander con `tmp/tc_probe.py`

Prima di compilare il codice C definitivo, esegui un breve controllo con `tmp/tc_probe.py` (o uno script Python equivalente) su Total Commander 11.58 x64 per verificare:
1. I nomi delle classi e i testi (`GetWindowTextW`) dei controlli figli di `TTotal_CMD` quando un pannello è su `\\\Restic Backups\` e l'altro è su `c:\...`.
2. Che `PostMessageW(hwnd_tc, 1075, 510, 0)` (comando `cm_VersionInfo = 510`) sul pannello del plugin apra effettivamente il report Markdown nel Lister invocando `FsExecuteFileW(..., L"properties")`.

---

### Step 2: Riscrittura di `src/wfx_status.h` e `src/wfx_status.c`

Sostituisci completamente la vecchia logica di `FS_STATUS_OP_CALCSIZE` in `src/wfx_status.h` e `src/wfx_status.c` con il modulo di gestione dell'hook `F3` e del *Path Guard*.

#### Contenuto di `src/wfx_status.h`

```c
#ifndef WFX_STATUS_H
#define WFX_STATUS_H

#include <windows.h>

/**
 * Installa (in modo lazy e thread-safe sul thread UI principale di TC)
 * gli hook thread-local WH_KEYBOARD e WH_CBT.
 * Da chiamare dentro FsFindFirstW.
 */
void restic_f3_hook_ensure_installed(void);

/**
 * Rimuove gli hook WH_KEYBOARD e WH_CBT se installati.
 * Da chiamare in DllMain(DLL_PROCESS_DETACH).
 */
void restic_f3_hook_shutdown(void);

/**
 * Disarma immediatamente la finestra temporale di intercettazione F3
 * quando Total Commander avvia un'operazione su un file reale (es. FsGetFileW).
 */
void restic_f3_hook_disarm(void);

/**
 * Helper puro (esposto per gli unit test) che verifica se una stringa
 * di percorso appartiene al pannello del plugin Restic e ne calcola
 * la profondità (0 = root, 1 = repo, 2+ = snapshot/subfolder, -1 = non del plugin).
 */
BOOL restic_f3_parse_plugin_panel_path(const WCHAR* text, int* out_depth);

#endif /* WFX_STATUS_H */
```

#### Contenuto di `src/wfx_status.c`

```c
#include "wfx_status.h"
#include "wfx_common.h"
#include "restic_log.h"
#include <wchar.h>

/* Costanti Total Commander per comandi interni (TOTALCMD.INC) */
#define TC_WM_USER_CMD     (WM_USER + 51) /* 1075 */
#define TC_CM_VERSIONINFO  510            /* Alt+Enter / File -> Properties */
#define F3_ARM_TIMEOUT_MS  200            /* Finestra massima in ms tra VK_F3 e MessageBox */

static HHOOK     g_hKeyboardHook   = NULL;
static HHOOK     g_hCbtHook        = NULL;
static DWORD     g_hookThreadId    = 0;
static ULONGLONG g_f3_armed_tick   = 0;
static BOOL      g_f3_file_op_seen = FALSE;

/* Prefisso standard del plugin in Total Commander Network Neighborhood */
static const WCHAR* kPluginPrefixes[] = {
    L"\\\\\\Restic Backups\\",
    L"\\\\\\Restic Backups",
    NULL
};

BOOL restic_f3_parse_plugin_panel_path(const WCHAR* text, int* out_depth)
{
    if (out_depth) *out_depth = -1;
    if (!text || !text[0]) return FALSE;

    /* Ignora spazi iniziali eventuali */
    while (*text == L' ' || *text == L'\t') {
        text++;
    }

    const WCHAR* after_prefix = NULL;
    for (int i = 0; kPluginPrefixes[i] != NULL; i++) {
        size_t len = wcslen(kPluginPrefixes[i]);
        if (_wcsnicmp(text, kPluginPrefixes[i], len) == 0) {
            after_prefix = text + len;
            /* Se il prefisso non terminava con '\', verifica che finisca o segua '\' o '>' */
            if (kPluginPrefixes[i][len - 1] != L'\\') {
                if (*after_prefix != L'\0' && *after_prefix != L'\\' && *after_prefix != L'>') {
                    after_prefix = NULL;
                    continue;
                }
            }
            break;
        }
    }

    if (!after_prefix) {
        return FALSE;
    }

    /* Salta eventuali backslash iniziali */
    while (*after_prefix == L'\\') {
        after_prefix++;
    }

    /* Calcola il numero di segmenti di directory reali (ignorando maschere tipo *.* o '>' finale) */
    int depth = 0;
    const WCHAR* p = after_prefix;
    while (*p && *p != L'>') {
        /* Se il segmento corrente è una maschera wildcard (es. "*.*"), non conta come cartella */
        const WCHAR* seg_start = p;
        BOOL has_wildcard = FALSE;
        while (*p && *p != L'\\' && *p != L'>') {
            if (*p == L'*' || *p == L'?') {
                has_wildcard = TRUE;
            }
            p++;
        }
        if (p > seg_start && !has_wildcard) {
            depth++;
        }
        while (*p == L'\\') {
            p++;
        }
    }

    if (out_depth) *out_depth = depth;
    return TRUE;
}

typedef struct {
    HWND focus_hwnd;
    int  focus_center_x;
    BOOL prompt_bar_found;
    BOOL prompt_bar_is_plugin;
    int  prompt_bar_depth;
    BOOL header_on_focus_side_found;
    BOOL header_on_focus_side_is_plugin;
    int  header_on_focus_side_depth;
} TcPanelScanCtx;

static BOOL CALLBACK enum_tc_child_proc(HWND hwnd, LPARAM lParam)
{
    TcPanelScanCtx* ctx = (TcPanelScanCtx*)lParam;
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    WCHAR text[1024];
    int len = GetWindowTextW(hwnd, text, (int)(sizeof(text) / sizeof(text[0])));
    if (len <= 0) {
        return TRUE;
    }
    text[len] = L'\0';

    /* 1. Barra del prompt dei comandi di TC: termina con '>' e contiene ':' oppure '\\\' */
    if (len >= 3 && text[len - 1] == L'>' &&
        (wcsncmp(text, L"\\\\\\", 3) == 0 || (len >= 3 && text[1] == L':' && text[2] == L'\\'))) {
        ctx->prompt_bar_found = TRUE;
        int d = -1;
        if (restic_f3_parse_plugin_panel_path(text, &d)) {
            ctx->prompt_bar_is_plugin = TRUE;
            ctx->prompt_bar_depth = d;
        } else {
            ctx->prompt_bar_is_plugin = FALSE;
            ctx->prompt_bar_depth = -1;
        }
    }

    /* 2. Barra del percorso sopra il pannello attivo (stesso lato orizzontale di focus_hwnd) */
    if (wcsncmp(text, L"\\\\\\", 3) == 0 || (len >= 3 && text[1] == L':' && text[2] == L'\\')) {
        if (text[len - 1] != L'>') {
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                if (rc.left <= ctx->focus_center_x && ctx->focus_center_x <= rc.right) {
                    ctx->header_on_focus_side_found = TRUE;
                    int d = -1;
                    if (restic_f3_parse_plugin_panel_path(text, &d)) {
                        ctx->header_on_focus_side_is_plugin = TRUE;
                        ctx->header_on_focus_side_depth = d;
                    }
                }
            }
        }
    }

    return TRUE;
}

/**
 * Verifica rigorosa (Path Guard) che:
 * 1. La finestra in primo piano sia Total Commander (TTotal_CMD).
 * 2. Il controllo con il focus sia una listbox dei file (LCLListbox / TMyListBox).
 * 3. Il pannello attivo sia effettivamente dentro \\\Restic Backups\...
 */
static BOOL restic_is_active_panel_in_plugin(HWND* out_main_hwnd, int* out_depth)
{
    if (out_main_hwnd) *out_main_hwnd = NULL;
    if (out_depth) *out_depth = -1;

    HWND hwndFocus = GetFocus();
    if (!hwndFocus) {
        return FALSE;
    }

    HWND hwndMain = GetAncestor(hwndFocus, GA_ROOT);
    if (!hwndMain) {
        return FALSE;
    }

    /* Verifica che la finestra principale sia Total Commander */
    WCHAR rootClass[64] = {0};
    GetClassNameW(hwndMain, rootClass, 63);
    if (_wcsicmp(rootClass, L"TTotal_CMD") != 0) {
        return FALSE;
    }

    /* Verifica che il focus sia nella lista file e NON nella command line (Edit) */
    WCHAR focusClass[64] = {0};
    GetClassNameW(hwndFocus, focusClass, 63);
    if (wcsstr(focusClass, L"ListBox") == NULL && wcsstr(focusClass, L"Listbox") == NULL) {
        return FALSE;
    }

    RECT rcFocus;
    if (!GetWindowRect(hwndFocus, &rcFocus)) {
        return FALSE;
    }

    TcPanelScanCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.focus_hwnd = hwndFocus;
    ctx.focus_center_x = (rcFocus.left + rcFocus.right) / 2;
    ctx.prompt_bar_depth = -1;
    ctx.header_on_focus_side_depth = -1;

    EnumChildWindows(hwndMain, enum_tc_child_proc, (LPARAM)&ctx);

    /* Se la barra del prompt dei comandi è visibile, essa indica SEMPRE il pannello attivo.
       Se dice che siamo fuori dal plugin (es. "c:\AMD>"), usciamo immediatamente! */
    if (ctx.prompt_bar_found && !ctx.prompt_bar_is_plugin) {
        return FALSE;
    }

    /* Se sia il prompt che l'header sul lato del focus confermano il plugin: */
    if (ctx.prompt_bar_is_plugin) {
        if (out_main_hwnd) *out_main_hwnd = hwndMain;
        if (out_depth) *out_depth = ctx.prompt_bar_depth;
        return TRUE;
    }

    if (ctx.header_on_focus_side_found && ctx.header_on_focus_side_is_plugin) {
        if (out_main_hwnd) *out_main_hwnd = hwndMain;
        if (out_depth) *out_depth = ctx.header_on_focus_side_depth;
        return TRUE;
    }

    return FALSE;
}

static void restic_trigger_tc_properties(HWND hwndMain)
{
    if (!hwndMain || !IsWindow(hwndMain)) {
        return;
    }
    restic_log_write("F3_HOOK: triggering TC cm_VersionInfo (510) on hwnd=%p", (void*)hwndMain);
    PostMessageW(hwndMain, TC_WM_USER_CMD, (WPARAM)TC_CM_VERSIONINFO, 0);
}

static LRESULT CALLBACK restic_keyboard_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == VK_F3) {
        BOOL is_keydown = ((lParam & 0x80000000UL) == 0);
        if (is_keydown) {
            /* Ignora Shift+F3, Ctrl+F3, Alt+F3 */
            BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            BOOL alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;

            if (!shift && !ctrl && !alt) {
                HWND hwndMain = NULL;
                int depth = -1;
                if (restic_is_active_panel_in_plugin(&hwndMain, &depth)) {
                    if (depth == 0 || depth == 1) {
                        /* Livello 0 (Repositories) e Livello 1 (Snapshots):
                           ci sono SOLO cartelle. Intercetta subito F3! */
                        g_f3_armed_tick = 0;
                        g_f3_file_op_seen = FALSE;
                        restic_log_write("F3_HOOK: F3 pressed at plugin depth=%d (folders only), redirecting directly to properties", depth);
                        restic_trigger_tc_properties(hwndMain);
                        return 1; /* Sopprime VK_F3 così TC non mostra "No files selected!" */
                    } else if (depth >= 2) {
                        /* Livello 2+ (dentro uno Snapshot): possono esserci sia cartelle sia file.
                           Arma la guardia temporale CBT e lascia passare VK_F3 a Total Commander. */
                        g_f3_armed_tick = GetTickCount64();
                        g_f3_file_op_seen = FALSE;
                        restic_log_write("F3_HOOK: F3 pressed at plugin depth=%d, armed CBT guard", depth);
                    }
                } else {
                    /* Pannello attivo FUORI dal plugin: disarma e non toccare nulla! */
                    g_f3_armed_tick = 0;
                    g_f3_file_op_seen = FALSE;
                }
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK restic_cbt_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HCBT_CREATEWND && g_f3_armed_tick != 0 && !g_f3_file_op_seen) {
        ULONGLONG elapsed = GetTickCount64() - g_f3_armed_tick;
        if (elapsed <= F3_ARM_TIMEOUT_MS) {
            HWND hwndCreated = (HWND)wParam;
            CBT_CREATEWNDW* cbt = (CBT_CREATEWNDW*)lParam;
            WCHAR className[64] = {0};
            GetClassNameW(hwndCreated, className, 63);

            /* Il MessageBox "No files selected!" di TC è un dialogo #32770 */
            if (wcscmp(className, L"#32770") == 0) {
                HWND hwndOwner = (cbt && cbt->lpcs) ? cbt->lpcs->hwndParent : NULL;
                if (!hwndOwner) {
                    hwndOwner = GetForegroundWindow();
                }
                g_f3_armed_tick = 0;
                g_f3_file_op_seen = FALSE;
                restic_log_write("F3_HOOK: intercepted #32770 popup (%llu ms after F3), aborting popup and triggering properties",
                                 (unsigned long long)elapsed);
                restic_trigger_tc_properties(hwndOwner);
                return 1; /* Annulla la creazione del MessageBox "No files selected!" */
            }
        } else {
            g_f3_armed_tick = 0;
        }
    }
    return CallNextHookEx(g_hCbtHook, nCode, wParam, lParam);
}

void restic_f3_hook_ensure_installed(void)
{
    if (g_hKeyboardHook != NULL && g_hCbtHook != NULL) {
        return;
    }

    /* Installa gli hook solo se siamo sul thread della finestra principale di TC */
    HWND hwndFg = GetForegroundWindow();
    if (hwndFg) {
        WCHAR cls[64] = {0};
        GetClassNameW(hwndFg, cls, 63);
        if (_wcsicmp(cls, L"TTotal_CMD") == 0) {
            DWORD fgTid = GetWindowThreadProcessId(hwndFg, NULL);
            DWORD curTid = GetCurrentThreadId();
            if (fgTid != curTid) {
                /* FsFindFirstW chiamato da un thread in background: aspetta una chiamata sul main thread */
                return;
            }
        }
    }

    DWORD tid = GetCurrentThreadId();
    if (!g_hKeyboardHook) {
        g_hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD, restic_keyboard_hook_proc, NULL, tid);
    }
    if (!g_hCbtHook) {
        g_hCbtHook = SetWindowsHookExW(WH_CBT, restic_cbt_hook_proc, NULL, tid);
    }
    g_hookThreadId = tid;
    restic_log_write("F3_HOOK: installed on thread=%lu (kbd=%p, cbt=%p)",
                     (unsigned long)tid, (void*)g_hKeyboardHook, (void*)g_hCbtHook);
}

void restic_f3_hook_shutdown(void)
{
    if (g_hKeyboardHook) {
        UnhookWindowsHookEx(g_hKeyboardHook);
        g_hKeyboardHook = NULL;
    }
    if (g_hCbtHook) {
        UnhookWindowsHookEx(g_hCbtHook);
        g_hCbtHook = NULL;
    }
    g_f3_armed_tick = 0;
    g_f3_file_op_seen = FALSE;
}

void restic_f3_hook_disarm(void)
{
    if (g_f3_armed_tick != 0) {
        restic_log_write("F3_HOOK: disarmed because TC started a file operation");
    }
    g_f3_armed_tick = 0;
    g_f3_file_op_seen = TRUE;
}

/* Export WFX: FsStatusInfoW e FsStatusInfo */
void __stdcall FsStatusInfoW(WCHAR* RemoteDir, int InfoStartEnd, int InfoOperation)
{
    (void)RemoteDir;
    if (InfoStartEnd == FS_STATUS_START) {
        if (InfoOperation == FS_STATUS_OP_GET_SINGLE ||
            InfoOperation == FS_STATUS_OP_GET_MULTI ||
            InfoOperation == FS_STATUS_OP_GET_SINGLE_THREAD ||
            InfoOperation == FS_STATUS_OP_GET_MULTI_THREAD) {
            /* L'utente ha premuto F3 (o F5) su un file reale: disarma subito l'hook CBT! */
            restic_f3_hook_disarm();
        }
    }
}

void __stdcall FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation)
{
    (void)RemoteDir;
    FsStatusInfoW(NULL, InfoStartEnd, InfoOperation);
}
```

---

### Step 3: Aggiornamento di `src/restic_wfx.c`

1. In `FsFindFirstW`:
   - Chiama `restic_f3_hook_ensure_installed();` all'inizio della funzione, in modo che non appena l'utente entra in `\\\Restic Backups\` gli hook thread-local sul thread principale di Total Commander siano attivi.
2. In `FsGetFileW`:
   - Chiama `restic_f3_hook_disarm();` all'inizio della funzione come ulteriore sicurezza oltre a `FsStatusInfoW`.
3. In `DllMain` (o aggiungendo `DllMain` se non presente in `src/restic_wfx.c`):
   - Su `DLL_PROCESS_DETACH`, chiama `restic_f3_hook_shutdown();` per rimuovere pulitamente gli hook quando il plugin viene scaricato (`cm_UnloadPlugins`).
4. Rimuovi qualsiasi riferimento residuo alla chiave di test `restic_test_force_f3` introdotta per il vecchio approccio `FS_STATUS_OP_CALCSIZE`.

---

### Step 4: Apertura nativa UTF-8 nel Lister senza plugin esterni (`src/restic_report.c`)

Durante il test live del `2026-10-31`, quando `restic_report_open_in_lister()` lanciava:
```c
swprintf(cmd_line, cmd_cap, L"\"%ls\" /O /S=L \"%ls\"", tc_exe, temp_file_path);
```
Total Commander passava il file `.md` al plugin Lister `sumatrapdf.wlx64` installato sul sistema.

Modifica `restic_report_open_in_lister()` in `src/restic_report.c` per usare il flag **`/S=L:T7`** (che in Total Commander forza l'apertura nel Lister interno in modalità **Testo UTF-8 puro**, disattivando i plugin `.wlx` di terze parti e visualizzando subito i caratteri Unicode `█`, `░`, `─` del report):

```c
swprintf(cmd_line, cmd_cap, L"\"%ls\" /O /S=L:T7 \"%ls\"", tc_exe, temp_file_path);
```

---

### Step 5: Unit Test in `tests/test_main.c`

Sostituisci i vecchi unit test di `FS_STATUS_OP_CALCSIZE` in `tests/test_main.c` con i nuovi test dedicati a `restic_f3_parse_plugin_panel_path()` e al disarmo dell'hook:

1. **Test percorsi dentro il plugin (`restic_f3_parse_plugin_panel_path`)**:
   - `L"\\\\\\Restic Backups\\>"` $\rightarrow$ `TRUE`, `depth = 0`
   - `L"\\\\\\Restic Backups\\*.*"` $\rightarrow$ `TRUE`, `depth = 0`
   - `L"\\\\\\Restic Backups\\Locale>"` $\rightarrow$ `TRUE`, `depth = 1`
   - `L"\\\\\\Restic Backups\\Locale\\*.*"` $\rightarrow$ `TRUE`, `depth = 1`
   - `L"\\\\\\Restic Backups\\Locale\\2026-03-31 19-40-52 [a1b2c3d4]>"` $\rightarrow$ `TRUE`, `depth = 2`
   - `L"\\\\\\Restic Backups\\Locale\\2026-03-31 19-40-52 [a1b2c3d4]\\Users\\*.*"` $\rightarrow$ `TRUE`, `depth = 3`
2. **Test percorsi FUORI dal plugin (devono restituire `FALSE` e `depth = -1`)**:
   - `L"c:\\AMD>"` $\rightarrow$ `FALSE`, `depth = -1`
   - `L"C:\\Windows\\System32\\*.*"` $\rightarrow$ `FALSE`, `depth = -1`
   - `L"\\\\server\\share\\folder>"` $\rightarrow$ `FALSE`, `depth = -1`
   - `L"\\\\\\OtherPlugin\\folder>"` $\rightarrow$ `FALSE`, `depth = -1`
3. **Test disarmo su operazione file (`FsStatusInfoW`)**:
   - Verifica che chiamando `FsStatusInfoW(L"\\Repo\\Snap\\file.txt", FS_STATUS_START, FS_STATUS_OP_GET_SINGLE)` la guardia venga disarmata senza aprire report.

---

### Step 6: Verifica Live Obbligatoria su Total Commander 11.58 x64 (`tmp/tc_probe.py`)

Dopo aver compilato il plugin (`cmake --build build --config Release`) e aver eseguito `ctest`, aggiorna ed esegui `tmp/tc_probe.py` su una vera istanza di Total Commander (`c:\totalcmd\TOTALCMD64.EXE`) per certificare **tutti e 5 gli scenari**:

1. **Scenario 1 — `F3` su un Repository (`\\\Restic Backups\Locale`)**:
   - Posiziona il cursore su `Locale` e invia `VK_F3`.
   - **Verifica**: NON deve apparire il popup `"No files selected!"` e deve aprirsi una finestra `TLister` con il report `.md` in modalità testo nativo (senza `sumatrapdf`).
2. **Scenario 2 — `F3` su uno Snapshot (`\\\Restic Backups\Locale\<Snapshot>`)**:
   - Entra in `Locale`, posiziona il cursore su uno snapshot e invia `VK_F3`.
   - **Verifica**: NON deve apparire `"No files selected!"` e deve aprirsi `TLister` con il report `.md` dello snapshot.
3. **Scenario 3 — `F3` su una Sottocartella dentro uno Snapshot (`depth >= 2`)**:
   - Entra nello snapshot, posiziona il cursore su una sottocartella e invia `VK_F3`.
   - **Verifica**: `WH_CBT` deve intercettare e sopprimere `#32770` (`"No files selected!"`) e deve aprirsi `TLister` con il report `.md` della sottocartella.
4. **Scenario 4 — `F3` su un File normale dentro uno Snapshot**:
   - Posiziona il cursore su un file normale e invia `VK_F3`.
   - **Verifica**: `FsStatusInfoW` disarma l'hook e `TLister` apre il contenuto del file reale.
5. **Scenario 5 (CRITICO) — Isolamento su cartella normale fuori dal plugin**:
   - Con `\\\Restic Backups\...` ancora aperto nel pannello sinistro, sposta il focus con `Tab` sul pannello destro posizionato su una normale cartella locale (es. `c:\AMD` o `c:\totalcmd`).
   - Premi `VK_F3` su una cartella locale.
   - **Verifica**: Nei log del plugin (`restic_wfx.log`) **NON** deve esserci alcun trigger di `cm_VersionInfo` né chiamate a `FsExecuteFileW`, e Total Commander deve eseguire il suo comportamento nativo indisturbato.

---

## 5. Punti di Attenzione per l'Agente LLM

1. **Comportamento di `HCBT_CREATEWND` quando restituisce `1` su `MessageBoxW`**:
   - In Win32, restituire `1` da `HCBT_CREATEWND` quando la classe è `#32770` fa fallire `CreateWindowExW` dentro `SoftModalMessageBox`, facendo ritornare immediatamente `MessageBoxW` senza mostrare la finestra a schermo e senza emettere il beep di sistema.
   - Se durante il test live con `tmp/tc_probe.py` su Total Commander 11.58 x64 dovessi notare che Total Commander mostra un secondo messaggio o ignora `PostMessageW` se `MessageBoxW` fallisce in `HCBT_CREATEWND`, passa semplicemente a chiudere il dialogo in `HCBT_ACTIVATE` (`PostMessageW((HWND)wParam, WM_CLOSE, 0, 0)` + `SetWindowPos` fuori schermo) seguito da `PostMessageW(hwndMain, TC_WM_USER_CMD, TC_CM_VERSIONINFO, 0)`. Verifica quale dei due approcci (`HCBT_CREATEWND` vs `HCBT_ACTIVATE`) si comporta in modo più pulito sul vero Total Commander con `tc_probe.py`.
2. **Focus post-chiusura del Lister**:
   - Quando il Lister viene chiuso (`Esc`), il focus torna al pannello di Total Commander e `g_f3_armed_tick` deve essere rigorosamente `0` così nessun altro dialogo legittimo di Total Commander (es. conferme di eliminazione o opzioni) verrà mai toccato da `WH_CBT`. La finestra temporale stretta di `200 ms` (`F3_ARM_TIMEOUT_MS`) garantisce già che qualunque dialogo aperto più di 200 ms dopo `F3` venga sempre ignorato.
3. **Aggiornamento Documentazione (`README.md`, `AGENT.md`, `MEMORY.md`)**:
   - Una volta superati tutti i test unitari e il test live con `tc_probe.py`, aggiorna `README.md`, `AGENT.md` e `MEMORY.md` documentando il funzionamento dell'hook `F3` con *Path Guard* e il flag `/S=L:T7`.