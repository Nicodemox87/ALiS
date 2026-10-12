# Verifica installer Mac — completata il 12 ottobre 2026

Versione 0.1.0-alpha.5.7, Mac build 1. Pacchetto completo Apple Silicon, macOS 27+. Preview non notarizzata; firma dell'app ad hoc, installer senza Developer ID.

## Esito automatico

- Compilazione nativa: riuscita; 7 test C++ superati.
- Updater: 16 test superati (versioni prerelease, revisioni Mac, requisito OS, repository errato, checksum, dimensioni, errori di rete, download incompleti).
- Il PKG estratto contiene 20.175 file/link identici all'app preparata, confrontati per SHA256 o destinazione del collegamento.
- App estratta in una cartella diversa con spazi nel percorso, firma verificata prima e dopo l'esecuzione.
- Tutti i moduli Python e le librerie di calcolo caricati provengono dal bundle; un solo runtime OpenMP, condiviso da XGBoost, scikit-learn e PyTorch. Nessun collegamento nativo richiede Homebrew esterno.
- 33 test Python sul payload effettivo: 32 superati, 1 saltato perché richiede CUDA. Inclusi addestramento e predizione dei sei classificatori, preparazione LAS/LAZ, isolamento spaziale, gestione dei dati non finiti, conservazione degli attributi e riapertura degli output.
- Prova updater con il vero PKG da 172.691.805 byte, trasporto locale simulato: dimensione/SHA256 verificati e attributo di quarantena macOS presente. Nessuna release fittizia pubblicata.
- Collegamento HTTPS reale al repository GitHub principale riuscito. Esito prima della pubblicazione: nessuna release Mac ancora pubblicata.
- Installer riconosce il Mac e la destinazione; il controllo preliminare rifiuta l'aggiornamento mentre ALiS è aperta.
- Nessun LAS/LAZ, modello joblib, backup personale o risultato della nuvola certificata nel pacchetto. I test di questa consegna usano dati sintetici; la nuvola grande non viene elaborata.

## Limiti

Host di verifica: M4 Max, macOS 27.0.1. Non è stato usato un secondo Mac pulito. macOS 15/26 e Intel non sono supportati da questo pacchetto. Installazione tramite Installer completata con autenticazione dell'utente. Ricevuta `org.alis.mac.install`, versione `0.1.507.1`. Tutti i 20.175 file/link installati coincidono con il payload collaudato; firma valida. Avvio GUI riuscito, versione Mac build 1 corretta, controllo automatico giornaliero attivo e controllo manuale GitHub verificato nell'interfaccia prima della pubblicazione: nessuna release Mac ancora pubblicata.

SHA256 PKG: `4b7bf8c5064989b1951899ed2687af1451d4394aa5205ca62462669fd6dd10bc`.

## Riproduzione

`python3 tools/verify_macos_installer.py output/mac-installer/ALiS-0.1.0-alpha.5.7-macOS-arm64-Setup.pkg /percorso/nuovo/estrazione --stage output/mac-installer/stage/Applications/ALiS.app`

La cartella di estrazione deve essere nuova. I log locali includono l'inventario, gli audit delle dipendenze e i risultati dei test. Non vengono pubblicati automaticamente.
