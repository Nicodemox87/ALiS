# ALiS — Guida operativa completa

**Versione: 0.1.0-alpha.5.21 · Windows x64 · CloudCompare 2.13.2 / Qt 5.15.x**

ALiS è un plugin di CloudCompare. Questa guida descrive la versione Windows pubblicata, non il prototipo standalone. L'aggiornamento macOS alle nuove funzioni è il prossimo passo della roadmap; il precedente pacchetto Mac non corrisponde alla 5.21.

È una research alpha: una classificazione proposta non è una verità certificata. Verificare terreno, strutture e superfici prima dell'uso scientifico. Non sono inclusi rilievi privati, etichette di training o modelli preaddestrati.

## Indice

1. [Installazione e aggiornamento](#1-installazione-e-aggiornamento)
2. [Come sono organizzati dati e pannelli](#2-come-sono-organizzati-dati-e-pannelli)
3. [Caricare, controllare e salvare](#3-caricare-controllare-e-salvare)
4. [Visualizzare attributi e lavorare su un sottoinsieme](#4-visualizzare-attributi-e-lavorare-su-un-sottoinsieme)
5. [Rumore, outlier e ricampionamento](#5-rumore-outlier-e-ricampionamento)
6. [Ground: CSF, PMF e preset](#6-ground-csf-pmf-e-preset)
7. [DTM e Height Above Ground](#7-dtm-e-height-above-ground)
8. [Feature geometriche e scale](#8-feature-geometriche-e-scale)
9. [Feature a blocchi, salvataggio, recupero e resume](#9-feature-a-blocchi-salvataggio-recupero-e-resume)
10. [Gestire, cercare ed eliminare campi scalari](#10-gestire-cercare-ed-eliminare-campi-scalari)
11. [Annotation Studio e percentuale punti per vista](#11-annotation-studio-e-percentuale-punti-per-vista)
12. [Classi automatiche: terreno, vegetazione e strutture](#12-classi-automatiche-terreno-vegetazione-e-strutture)
13. [Clustering esplorativo](#13-clustering-esplorativo)
14. [Classificazione supervisionata e validazione](#14-classificazione-supervisionata-e-validazione)
15. [Applicare modelli e preparare grandi file su disco](#15-applicare-modelli-e-preparare-grandi-file-su-disco)
16. [Cleaning Building Class](#16-cleaning-building-class)
17. [DFM: ricetta, conflitti e interpolazione](#17-dfm-ricetta-conflitti-e-interpolazione)
18. [Esportazione, riproducibilità e risoluzione problemi](#18-esportazione-riproducibilità-e-risoluzione-problemi)

## 1. Installazione e aggiornamento

Scaricare il pacchetto **Windows x64 della release 5.21** dalla [pagina della release](https://github.com/Nicodemox87/ALiS/releases/tag/v0.1.0-alpha.5.21). Non usare il vecchio installer 5.9 pensando che contenga queste modifiche.

Il pacchetto di aggiornamento richiede un'installazione esistente di **CloudCompare 2.13.2 x64 con Qt5**. Non è un installer di CloudCompare, Python o driver GPU. Un host Qt6 non è compatibile con questa DLL.

1. Salvare il progetto in BIN e chiudere tutte le finestre CloudCompare.
2. Controllare i checksum allegati alla release ed estrarre lo ZIP in una cartella normale, non eseguire dallo ZIP.
3. Avviare `Install_ALiS.cmd` e approvare personalmente l'eventuale richiesta amministratore di Windows. È possibile indicare una cartella CloudCompare diversa usando lo script PowerShell incluso.
4. Il programma verifica i file del pacchetto, conserva il backup dei file sostituiti e copia plugin, worker e documentazione. Non modifica rilievi o modelli dell'utente.
5. Riaprire CloudCompare e ALiS dal menu **Plugins** o dalla barra strumenti. Selezionare una nuvola nell'albero DB.

Per un'installazione manuale/gestita, consultare `INSTALL.txt` nel pacchetto. Non sovrascrivere DLL mentre CloudCompare è aperto. Il pacchetto è unsigned: controllare provenienza e checksum, senza disabilitare le protezioni del sistema.

Le operazioni native su terreno, feature e annotazione non richiedono Python. Classificazione ML/DL ed esportazione unificata delle feature esterne richiedono l'ambiente Python e le dipendenze appropriate: [runtime](RUNTIME.md). Un aggiornamento del plugin non installa automaticamente tali dipendenze.

## 2. Come sono organizzati dati e pannelli

Il Workspace ha cinque sezioni: **Session**, **Prepare**, **Annotate**, **Classification**, **History**. Il blocco **Display** rimane disponibile per cambiare campo, colori e filtri.

| Stato dei dati | Significato operativo |
| --- | --- |
| Original / imported | Riferimento importato; conservarne sempre il file sorgente. |
| Working | Classificazione corrente su cui proseguire il lavoro. |
| Derived / Prediction | Risultati e proposte da controllare prima di applicarli. |
| Manual / Trusted | Etichette dichiarate manualmente come dati attendibili per training. |

**Apply** è un cambiamento esplicito: una preview non deve essere scambiata per una modifica già applicata. Una previsione automatica non diventa automaticamente un'etichetta Trusted. Usare **History / Undo / Redo** quando disponibili per le operazioni ALiS; non presumere che ogni comando nativo esterno abbia lo stesso storico.

Percorso consigliato: controllare la nuvola → eventuale pulizia → Ground → DTM/HAG → feature → annotazione o classificazione → revisione → pulizia Building → DFM → salvataggio/esportazione. Non tutti i passaggi sono obbligatori per ogni analisi.

## 3. Caricare, controllare e salvare

1. Aprire LAS/LAZ oppure un progetto **CloudCompare BIN** con **File → Open**.
2. Selezionare la nuvola effettiva nell'albero DB, non soltanto il gruppo contenitore.
3. In **Session**, controllare numero di punti, estensione, densità indicativa, spaziatura, classi e metadati dei ritorni.
4. Confermare le unità metriche solo se le coordinate sono realmente in metri. ALiS conserva shift/scale globali; non inventa un CRS/EPSG assente.
5. Salvare il lavoro con **File → Save → CloudCompare BIN**. Il BIN conserva campi scalari e stato ALiS; un LAS caricato in precedenza non viene aggiornato soltanto perché è stato ricalcolato qualcosa in memoria.

Le statistiche sono memorizzate per evitare nuove scansioni a ogni selezione. **Analyse / refresh** forza una nuova analisi. La spaziatura campionata non misura automaticamente la densità del solo terreno.

Un progetto con feature **Disk only** dipende anche dalla cartella esterna dei checkpoint. Copiare soltanto il BIN non incorpora quelle matrici. Prima di eliminare o spostare una cartella di lavoro, verificare quali progetti la referenziano.

## 4. Visualizzare attributi e lavorare su un sottoinsieme

Nel blocco **Display → Show** scegliere RGB, Ground, Non-ground, HAG, Working, Prediction, Confidence o un campo `SF: …`. **Refresh** aggiorna l'elenco dei campi; la semplice presenza di un nome non dimostra che contenga valori calcolati.

**Palette** cambia la scala cromatica. **Colour range** imposta l'intervallo dei colori; **Auto** lo ripristina, **Legend** mostra la legenda. Cambiare l'intervallo cromatico non equivale a nascondere punti o cambiare la classificazione.

Aprire **Point filters / work only on selection…** per filtrare classi o valori di uno scalare, ad esempio Ground + Building oppure una fascia di HAG. Le condizioni consentono confronti e intervalli. Le classi di una scelta si combinano in OR; le diverse condizioni in AND.

- Il filtro di visualizzazione nasconde punti senza creare una nuova nuvola.
- L'opzione separata di elaborazione sul sottoinsieme congela gli ID scelti al momento di Apply. Cambiare poi il campo mostrato non ridefinisce silenziosamente il sottoinsieme operativo.
- Disattivare/ripristinare il filtro quando si vuole tornare a tutta la nuvola.

**Limiti importanti:** soltanto le operazioni che dichiarano supporto al sottoinsieme possono usarlo. In questa versione i percorsi ALiS per SOR/Noise/Resample nativi, feature a blocchi, diversi percorsi ML e alcuni strumenti legacy rifiutano esplicitamente una selezione operativa attiva, invece di fingere di rispettarla. I menu nativi di CloudCompare esterni ad ALiS non sono vincolati da questo controllo. Le feature in memoria possono limitare i punti di query, ma usano il supporto geometrico completo necessario ai vicinati.

## 5. Rumore, outlier e ricampionamento

In **Prepare**, gli strumenti di pulizia richiamano le operazioni native di CloudCompare:

- **SOR**: scarta outlier in base alle distanze statistiche dai vicini. Numero di vicini e soglia modificano il compromesso tra pulizia e perdita di dettaglio.
- **Noise filter**: controlla la coerenza locale; verificare le strutture sottili prima di scartarle.
- **Resampling / Subsample**: riduce la densità della nuvola. È un cambiamento dei dati, non la percentuale di visualizzazione di Annotation Studio.

Lavorare su una copia e confrontare prima/dopo. Pali, cavi e dettagli archeologici possono assomigliare a rumore. Dopo cancellazioni, riordini o ricampionamenti non riutilizzare checkpoint legati agli ID della geometria precedente.

## 6. Ground: CSF, PMF e preset

Aprire **Prepare → Terrain** e scegliere il metodo:

**CloudCompare CSF (native)** simula un telo per separare il terreno. I controlli includono risoluzione del telo, soglia verticale, tipo di terreno/rigidità, iterazioni e recupero delle pendenze. Il preset **CloudCompare reference** ripristina i valori iniziali della GUI CloudCompare: telo 2 m, soglia 0,5 m, Relief, 500 iterazioni, slope off. Gli altri preset sono punti di partenza modificabili, non ricette universalmente validate.

**PMF raster** usa finestre morfologiche progressive e soglie di quota. Sono disponibili preset Balanced/recover ground, esempio lidR ws/th, terreno complesso/ripido, basso rilievo dettagliato e Custom. È un'implementazione raster ispirata a Zhang/lidR, non una promessa di risultati identici a lidR.

Procedura:

1. Selezionare un preset e leggere la spiegazione dei parametri.
2. Eseguire la preview, alternando Ground e Non-ground.
3. Ispezionare pendii, margini, muri bassi, chiome e aree con pochi ritorni al suolo.
4. Modificare un controllo alla volta e confrontare il risultato.
5. Applicare il Ground scelto prima di classificare il resto o calcolare HAG.

**Last return non significa Ground.** Per un impulso con indice/count validi, il ritorno unico o l'ultimo ritorno è un vincolo geometrico utile, non una prova che il punto sia terreno. Dati dei ritorni mancanti non vengono inventati.

La libreria delle ricette permette **Save as new**, **Import JSON**, caricamento e **Suggest / adapt to this cloud**. Caricare ripristina i valori salvati; l'adattamento è un'azione distinta ed esplicita. Controllare sempre le scale proposte e le condizioni del rilievo.

## 7. DTM e Height Above Ground

Dopo avere applicato e verificato Ground, usare i comandi **DTM/HAG** nel pannello Terrain. Definire passo della griglia e impostazioni disponibili; verificare i vuoti, evitando di interpretare l'interpolazione come una misura del terreno.

Il **DTM** rappresenta il terreno. **HAG** è la differenza verticale dal modello di terreno e viene usato per distinguere fasce di vegetazione e valutare pali/cavi. Il DFM è un prodotto diverso, trattato più avanti.

Le regole automatiche verificano che HAG sia coerente con il Ground applicato corrente. Dopo una modifica al Ground, un HAG vecchio può risultare stale. Un HAG legacy privo di provenienza non è considerato verificato solo perché esiste un campo con quel nome.

Se compare `Vegetation, poles and wires need HAG verified against the current applied Ground`, tornare a Terrain, controllare/applicare Ground, poi calcolare nuovamente DTM/HAG. Non correggere l'errore rinominando campi o riempiendo valori mancanti con zero.

## 8. Feature geometriche e scale

In **Prepare → Features**, usare **Scale Manager** per definire i raggi e selezionare i descrittori desiderati. Le scale ALiS sono **raggi**, non diametri: un diametro di 4 m corrisponde a un raggio ALiS di 2 m.

La selezione comprende descrittori geometrici di covarianza/forma, orientamento, rugosità, quota e statistiche di vicinato. L'elenco operativo nel pannello è il riferimento per le feature selezionabili. Il costo cresce con punti, densità, raggi e numero di colonne; selezionare soltanto ciò che serve.

Un nome come `planarity@2` senza valori finiti non è una feature utilizzabile. La gestione scalari permette di verificare conteggio valido, valori mancanti e intervallo. I vecchi campi placeholder vuoti possono coesistere con campi realmente recuperati: controllare contenuto e provenienza, non soltanto l'etichetta.

È possibile aggiungere feature o nuove scale conservando i campi esistenti. Nuovi schemi/raggi richiedono una nuova elaborazione coerente; non cambiare il significato di un vecchio job in corso. Salvare il BIN dopo l'aggiunta dei campi in memoria.

## 9. Feature a blocchi, salvataggio, recupero e resume

Aprire **Spatial blocks** dal pannello Features. La mappa permette di scegliere blocchi, passo/griglia e cartella del job. L'**halo** conserva il supporto oltre il bordo del core, evitando di trattare il limite del blocco come limite del vicinato. Il core assegna le righe di output; l'halo è supporto temporaneo.

### Destinazione: scegliere prima di partire

| Destinazione | Dove sono i valori | Come conservarli |
| --- | --- | --- |
| Current cloud (add/update SFs) | Campi scalari nella nuvola aperta, con progressi su disco | Salvare BIN; conservare il job per resume/audit. |
| One new full-cloud copy | Campi su una copia completa | Richiede ulteriore RAM; salvare la copia in BIN. |
| Disk only (bounded RAM; linked features) | Checkpoint esterni, senza nuovi array SF full-cloud | Conservare cartella del job e BIN; esportare un LAS unificato quando serve. |

**Non sono la stessa cosa.** In Disk only non ci si deve aspettare che tutte le feature appaiano nei menu scalari della nuvola caricata. Per averle lì usare il recupero esplicito, solo se la RAM lo consente.

La sola matrice di K campi float richiede circa `4 × N × K` byte, esclusi geometria, indici, copie e interfaccia. Blocchi più piccoli non eliminano il costo degli array full-cloud quando si sceglie di aggiungerli alla nuvola.

### Elaborare

1. Impostare feature/raggi, griglia e cartella su un disco capiente.
2. Usare RAM automatica e thread appropriati al computer; gli altri programmi devono avere margine.
3. Se necessario eseguire il pilot su un parent rappresentativo, poi continuare sui blocchi incompleti. Non lanciare due elaborazioni dello stesso job.
4. Leggere le stime come intervalli provvisori: un preflight breve non è una promessa di durata. I tempi sono espressi in ore/minuti.
5. Attendere il commit del parent. Il manifest può rimanere invariato mentre un sub-blocco sta lavorando: da solo non dimostra uno stallo.

Il motore può suddividere internamente i blocchi densi per rispettare un budget prudente. Non riduce silenziosamente i raggi, non campiona ulteriormente i punti e non rende la RAM un limite imposto dal sistema operativo. Geometrie patologiche o halo troppo grandi possono richiedere una diagnosi esplicita.

### Riaprire senza ricalcolare

Aprire lo stesso BIN, selezionare la stessa nuvola e tornare al pannello Spatial blocks con la cartella del job. Riutilizzare le impostazioni salvate e proseguire soltanto sugli incompleti. I checkpoint completi vengono validati prima del riuso; non mescolare schemi, griglie o nuvole differenti.

Per trasferire checkpoint già calcolati nei menu scalari usare **Recover saved blocks as Scalar Fields…**. Scegliere la cartella con `blocks.json` e checkpoint. Il recupero controlla geometria/ordine, schema, integrità e ID; non lancia il calcolo delle feature. Se i campi non entrano in RAM, restare su Disk only e usare l'esportazione unificata. Dopo il recupero, salvare il BIN.

### File da riconoscere

- `blocks.json`: impostazioni, blocchi e stato del lavoro.
- `disk_features.json`: schema e integrità dei checkpoint esterni.
- `*.alsf`: ID delle righe originali e valori dei campi, non una seconda nuvola completa.
- `*.las` + `*.features.json`: esportazione unificata con mappatura dei nomi/scale e copertura.

**Export unified LAS from disk features…** crea un LAS con la rappresentazione corrente di CloudCompare e le feature esterne tramite lettura a blocchi. Verificare punti, nomi, scale e copertura; un export parziale può contenere NaN per righe non elaborate. Il file non viene caricato automaticamente e non può recuperare attributi del sensore mai importati in CloudCompare. [Dettagli tecnici](DISK_FEATURES.md).

## 10. Gestire, cercare ed eliminare campi scalari

Aprire **Session → Manage scalar fields / memory…**, oppure il comando corrispondente nelle impostazioni generali ALiS.

1. Scegliere cosa cercare: nome del campo oppure statistiche/valori disponibili.
2. Usare testo o confronti numerici (`=`, `!=`, `<`, `<=`, `>`, `>=`, intervalli) secondo il controllo selezionato.
3. Eseguire l'analisi esplicita quando servono conteggi validi/mancanti, minimo, massimo e media: una scansione completa non è gratis.
4. **Select all / Deselect all** agiscono sulla selezione dei campi filtrati; controllare l'elenco prima di eliminare.
5. Eliminare solo i campi voluti e salvare il BIN aggiornato. Alcuni campi di stato ALiS o metadati essenziali sono protetti.

Una ricerca nelle statistiche di un campo non seleziona automaticamente punti della nuvola: per quello usare **Point filters**. Rimuovere un campo dalla scena non significa eliminare anche tutti i checkpoint esterni che lo hanno generato.

## 11. Annotation Studio e percentuale punti per vista

Aprire **Annotate → Studio → Open Annotation Studio**, oppure l'icona **ALiS Annotator**. Le quattro viste osservano la stessa nuvola e condividono la selezione, non quattro copie delle etichette.

### Densità di visualizzazione: novità 5.21

In alto scegliere **Points per view (% of loaded cloud)**: **10%, 20%, …, 100%**, poi premere **Apply display density**. Il valore iniziale è 10%. La percentuale **non viene divisa tra le viste**: al 100% Top, Bottom, Front o 3D ricevono ciascuna tutti i punti ammissibili. Ogni intestazione mostra percentuale e numero di punti.

Se Live section o un filtro di visibilità limitano la nuvola, si applica la percentuale ai punti ammissibili, non si reintroducono quelli esclusi. Per vedere tutta l'estensione disattivare **Live section** e ripristinare eventuali filtri. Tooltip e conteggio distinguono totale caricato, punti nell'ambito e campioni visualizzati. Occlusione e inquadratura possono nascondere punti sullo schermo pur senza decimarli.

La percentuale cambia **solo il rendering**: non ricampiona il dataset, non riduce le righe delle feature e non limita la selezione manuale ai soli campioni visibili. Il vecchio tetto di circa 120.000 punti è stato rimosso. Valori elevati su quattro viste possono occupare molta RAM/VRAM e rallentare la navigazione; ALiS chiede conferma oltre un milione di punti per vista. Un errore di allocazione viene segnalato, non mascherato da un falso 100%.

### Viste, camere e sezioni

- Ogni menu di vista offre Top, Bottom, Front, Back, Right, Left, Section front/back e 3D orbit.
- **Fit** adatta il singolo pannello; **Fit all views** adatta le viste.
- **Sync** copia centro e scala dalla vista attiva mantenendo orientamenti indipendenti; **Live sync** continua il collegamento durante pan/zoom.
- **Live section** limita vista e nuove selezioni a uno spessore metrico. Scegliere normale X/Y/Z o piano verticale personalizzato.
- **Draw section (Top)** permette di tracciare una linea in una vista Top per definire un piano verticale; non è un piano 3D inclinabile arbitrariamente. Usare spessore, passo e posizione per scorrerlo.

### Selezionare e annotare

Rectangle, Polygon e Lasso selezionano gli ID originali lungo la proiezione attraverso lo slab, non soltanto la superficie frontale. **Replace**, **Add**, **Subtract** controllano la selezione; Ctrl all'inizio del gesto aggiunge temporaneamente. Muovere la sezione conserva i punti già selezionati, compresi quelli ora fuori slab. Usare Clear per svuotare.

La Wand parte da un punto visualizzato e seleziona gli originali entro un raggio 3D, con eventuale tolleranza sul valore grezzo dello scalare mostrato; non è un region growing connesso né una ricerca per semplice somiglianza di colore. Valori NaN non sono semi validi.

Scegliere una classe e premere **Assign to selected points**. È un'annotazione Manual/Trusted undoable su tutti gli ID selezionati. Verificare la selezione totale prima di assegnare e salvare il BIN. Navigare con rotazione 3D, pan e rotella senza confondere un movimento camera con un gesto di selezione.

## 12. Classi automatiche: terreno, vegetazione e strutture

In **Classification → Automatic classes**, il metodo **Requested classes — geometry rules (experimental)** propone le classi richieste mediante regole geometriche. Non è un modello universale addestrato e non garantisce la correttezza semantica.

| Classe | Codice |
| --- | --- |
| Ground esistente | 2 |
| Low / Medium / High vegetation | 3 / 4 / 5 |
| Building standard | 6 |
| Roof / Vertical wall ALiS | 128 / 129 |
| Utility pole / Wire-cable ALiS | 130 / 131 |
| Unclassified / Other da revisionare | 1 |

I codici 128–131 sono classi custom ALiS, non nuovi codici standard ASPRS universali. Verificare la mappatura quando si esporta verso altri software.

**Ground viene riutilizzato dallo stato applicato**, anche nei run per soli pali/fili: non viene ricalcolato dalla classificazione automatica. Per strutture, il metodo richiede ritorni singoli/ultimi validi e supporto planare/connesso a più scale; cerca sostegno di parete e tetto invece di chiamare Building ogni punto non vegetale. La vegetazione richiede geometria dispersa e HAG corrente. L'assenza di ritorni sotto una chioma non autorizza a inventare una parete o un tetto.

Prerequisiti geometrici: campi realmente calcolati di planarity/sphericity a **2 e 4 m**, più **normal_z_absolute a 2 m**. I controlli rifiutano campi mancanti o interamente vuoti. Vegetazione, pali e fili richiedono anche HAG verificato sul Ground corrente. Il messaggio di errore non avvia da solo nuove feature o nuovi array scalari.

### Controllo dell'operatore

Aprire il dialogo di parametri semantici: sono modificabili soglie di planarità/sfericità, normale verticale, fasce HAG, estensione minima di pareti/tetti e supporto connesso. Valori più conservativi riducono falsi positivi ma possono perdere strutture reali. Fasce altezza e soglie vanno documentate per ogni rilievo; non presumere che siano quelle di un vecchio esperimento.

1. Richiedere le classi desiderate e controllare i prerequisiti.
2. Modificare i parametri esplicitamente, se necessario; salvarli non lancia automaticamente il run.
3. Eseguire la proposta Derived.
4. Visualizzare **ASPRS Prediction (Derived)**, Confidence e il campo di motivazione `qAL_AutomaticReason` quando disponibile.
5. Filtrare una classe alla volta, ispezionare errori su chiome, fusti, muri bassi, tetti e tralicci.
6. Applicare a Working soltanto dopo la revisione. Correggere manualmente i casi importanti.

Gli score delle regole non sono probabilità calibrate. Molti punti in Other possono indicare prerequisiti insufficienti, HAG mancante, dispersione ambigua o soglie troppo conservative: non trasformarli indiscriminatamente in vegetazione o Building per migliorare l'aspetto della mappa.

## 13. Clustering esplorativo

MiniBatch K-Means, BIRCH e Gaussian Mixture raggruppano RGB/scalari scelti. Selezionare input significativi e verificare NaN, campi costanti e scala numerica. Le etichette manuali non devono diventare predittori della loro stessa classificazione.

Il numero di cluster è un parametro matematico, non una lista di classi semantiche. Nell'esplorazione con Ground applicato, Ground rimane riservato al gruppo 0 e gli altri cluster sono gruppi non-ground; non esportare questi ID interpretandoli automaticamente come codici ASPRS.

Gaussian Mixture può fallire con campioni collassati/covarianze mal definite. La versione include regolarizzazione e calcolo più robusti, ma dati degeneri richiedono comunque meno componenti o input migliori. I thread BLAS/OMP del worker sono limitati per evitare oversubscription; non significa che tutti i metodi diventino GPU.

## 14. Classificazione supervisionata e validazione

Seguire **Classification → Supervised workflow**, con i passaggi numerati:

1. Scegliere feature esistenti e verificate, senza includere il riferimento manuale tra gli input.
2. Impostare la validazione: hold-out spaziale interno oppure nuvola test esterna indipendente.
3. Definire libreria/cartella dei modelli e classi di riferimento attendibili.
4. Scegliere i classificatori e un profilo di costo adeguato.
5. Controllare parametri condivisi di training, validazione e predizione.
6. Addestrare, confrontare metriche/per-classe e matrice di confusione.
7. Classificare una nuvola compatibile con un modello salvato.

Metodi disponibili: **Random Forest**, **Extra Trees**, **Histogram Gradient Boosting**, **XGBoost**, **MLP su descrittori**, rete locale **PointNet-style**. Quest'ultima non è PointNet++ né un modello foundation preaddestrato; nessun peso esterno è incluso. Le dipendenze Python devono essere presenti.

CPU/GPU dipendono dal metodo. I percorsi CUDA supportati per XGBoost/MLP/rete locale richiedono hardware, driver e runtime compatibili. Feature geometriche e molti passaggi restano CPU; la VRAM occupata dalla visualizzazione non prova che il training usi la GPU.

Non valutare sui punti usati per addestrare. La validazione spaziale riduce, ma non annulla, dipendenze locali; un sito esterno è un test diverso. Confidence non significa accuratezza. Riportare numero e distribuzione delle classi, astensioni e limiti del riferimento: una vegetazione aggregata non permette di certificare separatamente Low/Medium/High.

## 15. Applicare modelli e preparare grandi file su disco

In **7. Classify a cloud → Prepare data for this model**, scegliere una nuvola caricata oppure LAS/LAZ su disco, modello attendibile e cartella del job.

Usare **Inspect model → Prepare / resume → Classify prepared → Show in CC**. ALiS controlla nomi, raggi e campi richiesti, prepara blocchi con overlap e permette di riprendere i completati. Riutilizzare soltanto feature realmente compatibili; non cambiare raggi per adattare i dati senza dichiararlo.

Questo percorso è distinto dal calcolo native Spatial blocks. Non serve caricare l'intera matrice di feature in RAM per preparare/predire un file su disco, ma il risultato di una grande nuvola può essere costoso da mostrare in CloudCompare. I limiti RAM sono stime di lavoro, non quote garantite dal sistema.

Caricare solo modelli da fonti attendibili: formati serializzati come joblib/pickle possono eseguire codice. Conservare modello, schema, parametri e report. [Guida specifica](MODEL_PREPARATION.md).

## 16. Cleaning Building Class

Prima di produrre un DFM, aprire **Cleaning Building Class** nell'area di pulizia/filtri Prepare. Questo passaggio serve a rivedere Building (6), Roof (128) e Wall (129), in particolare chiome erroneamente classificate come struttura.

Il controllo usa dispersione/planarità a più scale e vincoli dei ritorni disponibili. Evidenze forti di chioma possono proporre vegetazione se HAG è valido, altrimenti Other. Ritorni sicuramente non terminali possono essere rimossi dalle classi strutturali. Misure mancanti non autorizzano una cancellazione indiscriminata. Supporto planare debole rimane normalmente una segnalazione da rivedere; il comportamento aggressivo richiede scelta esplicita.

Eseguire **preview**, visualizzare proposte/stato, ispezionare e solo poi **Apply**. Ground e classi non target restano esclusi dall'intervento; Apply è undoable e non certifica automaticamente etichette per training. La preview viene invalidata se cambia lo stato/sottoinsieme su cui era stata calcolata.

Non è una garanzia che tutte le chiome vengano rimosse o tutti i tetti conservati. Controllare alberi grandi vicino a facciate, fusti rettilinei, coperture inclinate e strutture sottili.

## 17. DFM: ricetta, conflitti e interpolazione

Il **DFM di ALiS** combina terreno e classi strutturali selezionate. Non è sinonimo del DTM né una ricostruzione di edifici non osservati. Aprire **Prepare → Terrain → DFM** dopo avere verificato Working e ripulito Building.

### Impostare classi e passo

Scegliere Building 6, Roof 128 e/o Wall 129 e il passo comune della griglia in metri. Griglie fini aumentano memoria, vuoti e costo; griglie grosse possono cancellare dettagli. Tutte le fasi condividono origine e passo per confrontare le stesse celle.

Sono disponibili due ricette esplicite:

1. **MIN Ground + MIN Building → merged MIN**: quota minima Ground per cella senza fill; quota minima delle classi strutturali per cella senza fill; unione e minimo finale; riempimento soltanto dei vuoti residui. Le fasi sono valutate su griglie comuni senza creare tre copie full-cloud.
2. **MIN Ground / MAX Building (Building priority)**: conserva il minimo del terreno e il massimo strutturale, dando priorità alla struttura nelle sovrapposizioni.

**Attenzione:** il minimo strutturale può abbassare tetti e pareti; il minimo finale può cancellare un tetto quando sotto c'è Ground nella stessa cella. La ricetta MAX può invece conservare chiome non ancora pulite. Nessuna delle due è automaticamente corretta per ogni superficie.

### Mappa dei conflitti: scegliere Ground o struttura

Quando Ground e classi Building occupano la stessa cella, ALiS apre la mappa dei conflitti **prima della superficie definitiva e dell'interpolazione**.

1. Leggere legenda e scelta predefinita della ricetta.
2. Selezionare Ground, Building oppure ricetta.
3. Tracciare rettangoli sulle aree in cui imporre la scelta. L'ultimo rettangolo prevale nelle sovrapposizioni.
4. Usare Undo/Clear, pan e zoom/Fit per controllare il risultato.
5. Confermare per proseguire oppure annullare senza generare l'output.

Le celle non marcate seguono la scelta predefinita. La preview può aggregare visivamente le celle, ma non deve far perdere le decisioni sui conflitti originali. Parametri, rettangoli e scelte vengono registrati nei metadati/storico; è disponibile l'esportazione PNG della mappa.

### Riempire i vuoti finali

| Metodo | Quando considerarlo | Limite da controllare |
| --- | --- | --- |
| None | Conservare NODATA e non inventare quota | La superficie rimane incompleta. |
| Local mean | Preferire un valore vicino alla media dei supporti locali | Può appiattire bordi e dettagli. |
| IDW | Pesare maggiormente celle osservate vicine | Può creare effetti a bersaglio. |
| TIN / Delaunay linear | Interpolazione lineare su triangoli | Limitare lati lunghi e ponti sopra vuoti. |
| Ordinary Kriging | Modellare correlazione spaziale con variogramma | Serve supporto adeguato e revisione dei parametri; è più costoso. |

Controlli condivisi: raggio massimo di supporto, numero minimo/massimo di celle osservate, supporto circostante, escursione locale massima e distanza massima dalla media locale. Un valore respinto resta NODATA: non viene semplicemente tagliato entro soglia per fingere validità. Il fill usa celle originariamente osservate, non propaga a catena valori già interpolati, e non modifica le quote osservate.

Per Kriging scegliere Spherical/Exponential/Gaussian e stima automatica oppure parametri range/sill/nugget; controllare che siano sensati nelle unità del rilievo. Limiti di vicinato e costo evitano elaborazioni incontrollate; un caso degenerato non passa silenziosamente a un altro metodo.

Il risultato è una **nuvola-griglia DFM separata**, con provenienza Ground/Building/interpolazione. Verificarla in pianta, profilo e ai bordi dei tetti. La release non aggiunge un nuovo esportatore GeoTIFF diretto nel pannello ALiS: usare i percorsi nativi appropriati, dichiarando passo, NODATA e CRS. I test del codice su griglie sintetiche non certificano l'accuratezza del DFM del proprio sito.

## 18. Esportazione, riproducibilità e risoluzione problemi

### Cosa conservare

Conservare sorgente, BIN, cartelle dei job ancora referenziati, `.features.json`, modello/schema, ricette e report. Per ogni prodotto riportare versione ALiS/CloudCompare, unità/CRS noto, Ground applicato, raggi, feature, classi, filtri, split di validazione e impostazioni DFM. Un'immagine campionata è una preview, non prova di copertura completa.

Non cancellare checkpoint o intermedi prima di avere riaperto e verificato il risultato finale. Per pubblicare un bug, rimuovere percorsi personali e dati riservati dai log.

### Errori frequenti

| Sintomo | Cosa fare |
| --- | --- |
| Ho riaperto e non vedo le feature | Verificare di avere aperto il BIN salvato, non il LAS originale. Se erano Disk only, usare Recover saved blocks o LAS unificato; non ricalcolare subito. |
| `Required fields missing or entirely empty` | Controllare i valori finiti; recuperare i checkpoint giusti o calcolare esplicitamente le scale richieste. Il nome da solo non basta. |
| HAG missing/stale/legacy | Verificare Ground corrente e ricalcolare DTM/HAG da Terrain. |
| Molta vegetazione in Other | Controllare HAG, completezza delle feature, soglie/score e motivazioni Derived. Non forzare classi senza evidenza. |
| GMM covariance error | Evitare troppi componenti, campi costanti e campioni degeneri; verificare gli input. |
| RAM insufficiente per scalari | Scegliere Disk only; stimare `4×N×K`. Non forzare una copia full-cloud. |
| Studio sembra vuoto o mostra una linea | Controllare Live section, spessore/orientamento, filtri, Fit e campo attivo. |
| 100% rallenta o esaurisce VRAM | Ridurre la percentuale e Apply; ogni vista ha il proprio carico grafico. |
| Selezione operativa rifiutata | Il comando non supporta quel sottoinsieme. Disattivarlo consapevolmente oppure usare un percorso compatibile; non presumere che lavori solo sui punti visibili. |
| Plugin non caricato | Controllare host 2.13.2 x64 Qt5, DLL corretta, chiusura del vecchio processo e log dei plugin. |

Per bug e richieste: [GitHub Issues](https://github.com/Nicodemox87/ALiS/issues). Per novità e limiti di questa versione: [release 5.21](RELEASE_ALPHA521.md). **Prossimo passo: aggiornare e collaudare il pacchetto macOS.**
