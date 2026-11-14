---
name: release-packager
description: Prepara una nuova release del plugin Restic WFX (32 e 64 bit). Aggiorna la versione in tutti i file (version.h, pluginst.inf, pyproject.toml, README, AGENT.md, MEMORY.md), compila ed esegue tutti i test per le due architetture (Debug, /analyze, ASan, Release) e genera lo ZIP di installazione per Total Commander con verifica finale di entrambe le DLL.
---

# Release Packager

Automatizza l'intera procedura di rilascio con un unico script Python:
`.agents/skills/release-packager/scripts/release.py`

> Python è già attivo nella shell: usare direttamente `python`, senza creare un nuovo venv.
> Serve un ambiente MSVC inizializzato (cl, link, rc, nmake) che contenga **sia il toolset x86 sia quello x64**
> (in Visual Studio Installer: "MSVC v143 - VS 2022 C++ x86/x64 build tools"; con il toolset portabile:
> `python portable-msvc.py --target x64,x86 --accept-license`), più `bash` (Git Bash) nel PATH, come per `./build.sh`.
> La release copre **entrambe le architetture**: `./build.sh` con `ARCH=all` traduce da solo le variabili
> d'ambiente (`INCLUDE`/`LIB`/`PATH`) da una architettura all'altra, quindi si parte da un ambiente qualsiasi.

## Quando usare questa skill
- L'utente chiede di "rilasciare", "aggiornare la versione a X.Y.Z" o di "preparare lo ZIP di installazione".
- Mai per modifiche di codice: prima si completano e si committano le modifiche, poi si rilascia.

## Uso

```bash
# 1. Anteprima: mostra cosa verrebbe modificato, non scrive nulla
python .agents/skills/release-packager/scripts/release.py 1.1.0 --dry-run

# 2. Procedura completa: bump + docs + test (Debug, /analyze, ASan) + package Release + verifica ZIP
python .agents/skills/release-packager/scripts/release.py 1.1.0 --notes "Breve descrizione delle novità"

# Varianti
... 1.1.0 --skip-tests   # solo `./build.sh package` (Release build + Release tests + ZIP)
... 1.1.0 --no-build     # aggiorna solo file e documentazione
... 1.1.0 --mutation    # aggiunge i test di mutazione (`./build.sh mutate`, ~15 min) prima del package
```

Versione nel formato `MAJOR.MINOR.PATCH` (ogni componente ≤ 65535). Il quarto numero della DLL (`build`) è sempre `0`.

## Cosa fa lo script (in ordine)
1. **Bump versione** (rileva la root del repo da solo, mantiene i fine riga CRLF/LF originali, fallisce se un pattern non viene trovato):
   - `include/version.h` (`VER_MAJOR/MINOR/PATCH/BUILD`, `VERSION_CSV`, `VERSION_STR`, `PRODUCT_VER`) - unica fonte di verità per `VERSIONINFO` della DLL;
   - `pluginst.inf` (`version=`), controllato anche da `test_plugin_dll.c` e dal target `package`;
   - `pyproject.toml` (solo la versione del progetto, non le dipendenze);
   - `README.md` (badge versione);
   - `AGENT.md` (sezione 5.1 e albero dei sorgenti).
2. **Build e test** tramite `./build.sh` con `ARCH=all`: `analyze` → `asan` → `package`. `analyze` compila e rilancia già l'intera suite Debug (con `/analyze` in più), quindi non serve un `test` separato; `package` ripete la suite in Release. Ogni azione gira **prima su x64 e poi su x86** (artefatti in `build\x64` e `build\x86`, mai mescolati); `package` esegue per ogni architettura clean, build Release, test Release e staging dei file di installazione, poi crea lo ZIP con `tools/package.py`. Con `--mutation` si aggiunge `mutate` prima di `package` (i test devono rilevare tutti i difetti piantati di proposito in `tests/mutation/`). Si ferma al primo errore indicando l'azione fallita.
3. **Verifica ZIP** `build/wfx_restic_<versione>.zip`: contenuto esatto nella root (`pluginst.inf`, `restic.wfx`, `restic.wfx64`, `restic_plugin.ini`, `README.md`, `LICENSE`), integrità, `pluginst.inf` coerente (`type=wfx`, `file=restic.wfx` ossia il nome base, `version=`), tipo di macchina PE di ogni DLL (`restic.wfx` = `IMAGE_FILE_MACHINE_I386`, `restic.wfx64` = `IMAGE_FILE_MACHINE_AMD64`) e `FileVersion` di entrambe letto dalla risorsa `VERSIONINFO`, uguale a `<versione>.0`. Scrive anche `build/<zip>.sha256` e stampa lo SHA-256.
4. **MEMORY.md** (solo dopo build riuscita): riga "Release version", suggerimento "Next" e nuova voce nel log delle sessioni (con `--notes`).

## Dopo lo script (compiti dell'agente)
1. Rileggere `git diff --stat`: devono risultare modificati solo i file elencati sopra.
2. Se la release introduce funzionalità/cambiamenti, aggiornare a mano `README.md`, `AGENT.md` e `MEMORY.md` seguendo la skill `memory-updater` (lo script aggiorna solo i riferimenti di versione e un log minimale). Aggiornare anche i conteggi dei test se sono cambiati.
3. Eseguire il controllo di agnosticità del repo (deve non stampare nulla):
   `grep -rnI -E 'setup_x64|_progetti' --exclude='.env' --exclude-dir=.git --exclude-dir=tmp --exclude-dir=build --exclude-dir=.venv .`
4. Comunicare all'utente: percorso e SHA-256 dello ZIP. Commit, tag (`vX.Y.Z`) e upload su GitHub Releases li gestisce l'utente.

## Note e problemi noti
- Se Total Commander tiene caricata una delle DLL (`build\x64\restic.wfx64` o `build\x86\restic.wfx`), `build.sh` rinomina il file in `.old`; il `package` fa però `clean` sulla cartella dell'architettura e può fallire: chiudere Total Commander o eseguire `cm_UnloadPlugins`.
- Un pacchetto unico serve 32 e 64 bit, come vuole il WFX SDK: le due DLL hanno lo stesso nome base nella stessa cartella e `pluginst.inf` registra il **nome base** `restic.wfx`; Total Commander 64 bit cerca da solo `restic.wfx64`. Non mettere `file=restic.wfx64` in `pluginst.inf`, e non caricare `restic.wfx64` in un Total Commander a 32 bit (o viceversa): il caricamento fallisce.
- Se un passo di build fallisce, i file di versione sono già modificati: correggere e rilanciare lo stesso comando (è idempotente), oppure `git checkout` dei file.
- Rilanciare lo script con la stessa versione non cambia i file di versione ma aggiunge una nuova riga di log solo se manca la voce di quella versione.
- Per cambiare le regole del pacchetto (file nello ZIP) modificare `tools/package.py` (che crea lo ZIP, usato anche da `./build.sh package`) e `ZIP_EXPECTED`/`ZIP_DLLS` in `release.py` (che lo verifica).
- Il toolset portabile va installato con entrambe le architetture: se manca `build\x86`, `./build.sh` si ferma con un messaggio che chiede il toolset x86. È il caso di `portable-msvc.py --target x64,x86 --msvc-version <major.minor> --sdk-version <build>` (le versioni si vedono con `--show-versions`).
- Non usare heredoc lunghi nella shell dell'agente: per script più lunghi usare lo strumento `write`.
