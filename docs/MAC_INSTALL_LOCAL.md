# ALiS su macOS

Per l'installer completo con Python incluso, i requisiti, le autorizzazioni macOS e gli aggiornamenti GitHub, leggere `MAC_INSTALLER.md`.

## Build di sviluppo

Prerequisiti Homebrew: `cmake ninja qt@5 eigen laszip python@3.12 libomp` e Xcode Command Line Tools.
`bash tools/build_macos.sh` verifica il commit CloudCompare 2.13.2, applica le patch tracciate, esegue i test nativi e prepara un'app di sviluppo con le librerie native incluse.

Solo per usare la build di sviluppo senza creare il PKG, `ALIS_WITH_TORCH=1 bash tools/setup_macos_runtime.sh /opt/homebrew/opt/python@3.12/bin/python3.12` crea il runtime esterno in `~/.alis/runtime-macos`.

`python3 tools/build_macos_installer.py` crea invece il pacchetto autosufficiente, con runtime portabile dentro ALiS.app. Il Python incluso ha priorità sul vecchio runtime personale. Le variabili ALIS_PYTHON e QARCHAEOLIDAR_PYTHON restano override espliciti per sviluppatori.

La build nativa usa un deployment target 15, ma ciò non determina il requisito dell'intero pacchetto: l'audit verifica anche ogni libreria inclusa. Le librerie della consegna attuale richiedono macOS 27.

## Conservazione dati

L'installer sostituisce solo ALiS.app e conserva il vecchio bundle nel backup indicato dalla guida. Non rimuovere dipendenze condivise, cartelle Library, altre installazioni CloudCompare, dati, modelli o report. Un plugin di un'altra versione di CloudCompare va verificato prima di usarlo con l'host 2.13.2.

CUDA non è disponibile; PointNet usa CPU, MPS non è abilitato. I test software non certificano l'accuratezza scientifica dei modelli.
