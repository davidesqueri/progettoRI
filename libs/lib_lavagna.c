#include "../include/include_lavagna.h"

int utentiConnessi = 0;
int mysock_lavagna; // mysock_lavagna è il socket di ascolto
int max_des;
int stdin_chiusa = 0;
char comando[256]; // buffer per il comando da terminale
uint16_t ID_LAVAGNA = 6969;
fd_set readfds; // set di descrittori da controllare per la lettura
card_t *testaToDo = NULL;
card_t *testaDoing = NULL;
card_t *testaDone = NULL;
char buf[MAX_LEN_COMANDO + 2]; // buffer per il comando da terminale, dimensionato per il comando più lungo possibile + \n + \0

/// Puntatore alla testa della lista degli utenti connessi
utente_des_t * testa_des_utente = NULL;

card_t *testaPendingCards = NULL;

struct sockaddr_in utente;
socklen_t utenteLen = sizeof(utente);

int attesa_ack = 0;

int num_carte_assegnate = 0;

/* @brief Legge esattamente len byte (salvo chiusura/errore). Ciclo portabile,
 * senza MSG_WAITALL (ignorato su macOS).
 * @return (ssize_t)len se ok, 0 se il peer ha chiuso, -1 in caso di errore
 */
static ssize_t recv_fisso(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, (char*)buf + got, len - got, 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        got += (size_t)n;
    }
    return (ssize_t)len;
}

void SHOW_LAVAGNA() {
    // stampa la lavagna, con le colonne e le card assegnate nelle giuste colonne
    printf("\n");
    printf("-------------------------------------------\n|                   %5d                 |\n-------------------------------------------\n|    ToDo    |     Doing    |     Done    |\n-------------------------------------------\n",ID_LAVAGNA);
    card_t*todo = testaToDo;
    card_t*doing = testaDoing;
    card_t*done = testaDone;
    if (todo == NULL && doing == NULL && done == NULL) {
        printf("(lavagna vuota)\n");
        printf("-------------------------------------------\n\n");
        return;
    }
    while (!(todo == NULL && doing == NULL && done == NULL)) {
        if (todo != NULL) {
            printf("| %10d |", todo->ID);
            todo = todo->prox;
        }
        else {
            printf("|            |");
        }

        if (doing != NULL) {
            printf(" %12d |", doing->ID);
            doing = doing->prox;
        }
        else {
            printf("              |");
        }

        if (done != NULL) {
            printf(" %11d |\n", done->ID);
            done = done->prox;
        }
        else {
            printf("             |\n");
        }
    }
    printf("-------------------------------------------\n");
    // Dettaglio card: descrizione, responsabile e timestamp (richiesti dalla spec)
    printf("--- Dettaglio card ---\n");
    const char *nomi[3] = {"ToDo", "Doing", "Done"};
    card_t *liste[3] = {testaToDo, testaDoing, testaDone};
    for (int i = 0; i < 3; i++) {
        for (card_t *c = liste[i]; c != NULL; c = c->prox) {
            char tbuf[26] = "-";
            if (c->timestampUltimaModifica != 0) {
                struct tm *tm = localtime(&c->timestampUltimaModifica);
                if (tm != NULL) {
                    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
                }
            }
            printf("[%s] ID=%u resp=%u mod=%s desc=\"%s\"\n",
                nomi[i], c->ID, c->responsabile, tbuf,
                c->descrizione ? c->descrizione : "");
        }
    }
    printf("\n");
}


utente_des_t* scorri_lista (utente_des_t* des_attuale) {
    if (des_attuale == NULL) {
        // gestisci errore
        return NULL;
    }
    while (des_attuale->prox != NULL) {
        des_attuale = des_attuale->prox;
    }
    return des_attuale;
}


void accetta_utente() {
    if (DEBUG) {
        printf("Richiesta di connessione sotto esame.\n");
    }
    utenteLen = sizeof(utente);
    int utente_sock = accept(mysock_lavagna, (struct sockaddr *)&utente, &utenteLen);
    if (utente_sock < 0) {
        perror("errore su accept: ");
        return;
    }
    if (DEBUG) {
        printf("richiesta di connessione accettata\n");
    }

    // La connessione TCP da sola NON registra l'utente: la registrazione
    // avviene alla ricezione di HELLO (gestisci_hello). Qui si crea solo
    // il descrittore pendente in attesa di HELLO.
    utente_des_t*nuovo_utente = malloc(sizeof(utente_des_t));
    if (nuovo_utente == NULL) {
        printf("malloc error\n");
        close(utente_sock);
        return;
    }
    nuovo_utente->utente = 0; // porta sconosciuta finché non arriva HELLO
    nuovo_utente->des = utente_sock; // salvo in utente_des il descrittore del socket relativo all'utente
    nuovo_utente->prox = NULL; // è l'ultimo elemento nella lista
    nuovo_utente->doing = 0;
    nuovo_utente->registrato = 0;

    if (DEBUG) {
        printf("nuova connessione TCP in attesa di HELLO (sock %d)\n", utente_sock);
    }

    if (testa_des_utente == NULL) {
        testa_des_utente = nuovo_utente;
    }
    else {
        utente_des_t*ultimo_utente = scorri_lista(testa_des_utente);
        ultimo_utente->prox = nuovo_utente;
    }

    // Nessuna risposta qui: "OK" viene inviato da gestisci_hello dopo HELLO.
    // Nessun AVAILABLE_CARD qui: scatta solo con >=2 utenti registrati.

    return;
}


void prepara_set() {
    FD_ZERO(&readfds);
    max_des = 0; // Azzera il massimo all'inizio di ogni ciclo

    // 1. Aggiungo il descrittore del socket di ascolto
    FD_SET(mysock_lavagna, &readfds);
    if (mysock_lavagna > max_des) {
        max_des = mysock_lavagna;
    }

    // 2. Aggiungo il descrittore di stdin (salvo EOF già rilevato)
    if (!stdin_chiusa) {
        FD_SET(STDIN_FILENO, &readfds);
        if (STDIN_FILENO > max_des) { // Di solito STDIN_FILENO è 0, ma è buona norma
            max_des = STDIN_FILENO;
        }
    }

    // 3. Aggiungo i descrittori dei socket connessi con gli utenti
    utente_des_t *des_attuale = testa_des_utente;
    while (des_attuale != NULL) {
        FD_SET(des_attuale->des, &readfds);
        if (des_attuale->des > max_des) {
            max_des = des_attuale->des;
        }
        des_attuale = des_attuale->prox;
    }
}

void STAMPA_UTENTI() {
    printf("\n");
    printf("====== Utenti Connessi ======\n");
    utente_des_t*utente_attuale = testa_des_utente;
    if (utente_attuale == NULL) {
        printf("nessun utente connesso.\n");
    }
    else {
        printf("numero di utenti connessi: %d\n",utentiConnessi);
        while (utente_attuale != NULL) {
            if (utente_attuale->registrato) {
                printf("\t%d\n",utente_attuale->utente);
            }
            else {
                printf("\t(sock %d, in attesa di HELLO)\n",utente_attuale->des);
            }
            utente_attuale = utente_attuale->prox;
        }
    }
    printf("=============================\n");
    printf("\n");
}

void input_stdin() {
    if (fgets(buf, sizeof(buf), stdin) == NULL) { // questo legge al più (numero di caratteri disponibili in buf - 1), ed in fondo aggiunge \0
        // EOF/errore su stdin (es. input ridiretto terminato): smetti di
        // monitorarlo, altrimenti select() lo segnala sempre pronto e si cicla
        stdin_chiusa = 1;
        clearerr(stdin);
        return;
    }
    // rimozione di \n:
    for (int i = 0; buf[i] != '\0'; i++) {
        if (buf[i] == '\n') {
            buf[i] = '\0';
        }
    }
    int ID_comando = VALIDA_INPUT(buf);
    switch (ID_comando) {
        case -1: {
            printf("Comando non riconosciuto.\n");
            break;
        }
        case ID_QUIT: {
            printf("terminazione...\n");
            exit(0);
        }
        case ID_SEND_USER_LIST: {
            SEND_USER_LIST();
            break;
        }
        case ID_SHOW_LAVAGNA: {
            SHOW_LAVAGNA();
            break;
        }
        default: {
            printf("Comando non disponibile alla lavagna.\n");
        }
    }
    printf("Inserire un comando:\n");
}
/*
    #define ID_CREATE_CARD 1
    #define ID_HELLO 2
    #define ID_QUIT 3
    #define ID_ACK_CARD 4
    #define ID_CARD_DONE 5
    ///comandi lavagna
    #define ID_SEND_USER_LIST 6
    #define ID_SHOW_LAVAGNA 7
    #define ID_AVAILABLE_CARD 8
 */

void gestisci_messaggio_utente() {
    // Serve TUTTI i descrittori pronti (non solo il primo): il main loop
    // chiama questa funzione una volta per ciclo di select().
    utente_des_t*attuale = testa_des_utente;
    while (attuale != NULL) {
        utente_des_t*prox = attuale->prox; // rimuovi_utente può liberare attuale
        if (FD_ISSET(attuale->des, &readfds)) {
            int des_utente = attuale->des;
            uint32_t net_cmd = 0;
            size_t received = 0;
            int chiuso = 0;
            while (received < sizeof(uint32_t)) {
                ssize_t n = recv(des_utente, (char*)&net_cmd + received,
                                 sizeof(uint32_t) - received, 0);
                if (n == 0) { // l'utente ha chiuso la connessione
                    chiuso = 1;
                    break;
                }
                else if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("errore recv: ");
                    chiuso = 1;
                    break;
                }
                received += (size_t)n;
            }
            if (chiuso) {
                if (attuale->registrato) {
                    gestisci_quit(attuale);
                }
                else {
                    rimuovi_utente(attuale);
                }
                attuale = prox;
                continue;
            }

            // controllo che comando è arrivato
            uint32_t id_comando = ntohl(net_cmd);
            if (DEBUG) {
                printf("ID Comando ricevuto: %u\n",id_comando);
            }

            switch (id_comando) {
                case ID_HELLO: {
                    gestisci_hello(attuale);
                    break;
                }
                case ID_QUIT: {
                    gestisci_quit(attuale);
                    break;
                }
                case ID_CREATE_CARD: {
                    if (!attuale->registrato) {
                        printf("CREATE_CARD da socket non registrato, ignorato.\n");
                        break;
                    }
                    crea_card(attuale); // attuale è l'utente che ha inviato il messaggio
                    break;
                }
                case ID_ACK_CARD: {
                    if (!attuale->registrato) {
                        printf("ACK_CARD da socket non registrato, ignorato.\n");
                        break;
                    }
                    ACK_CARD(attuale);
                    break;
                }
                case ID_CARD_DONE: {
                    if (!attuale->registrato) {
                        printf("CARD_DONE da socket non registrato, ignorato.\n");
                        break;
                    }
                    card_done(attuale);
                    break;
                }
                case ID_PONG_LAVAGNA: {
                    // PONG: [ID_PONG_LAVAGNA][ID_card]; azzera il PING in attesa
                    uint32_t net_id = 0;
                    ssize_t n = recv_fisso(des_utente, &net_id, sizeof(uint32_t));
                    if (n <= 0) {
                        if (attuale->registrato) {
                            gestisci_quit(attuale);
                        }
                        else {
                            rimuovi_utente(attuale);
                        }
                        break;
                    }
                    uint32_t id_card = ntohl(net_id);
                    card_t *prior = NULL;
                    card_t *card = trova_card(testaDoing, id_card, &prior);
                    if (card != NULL && card->responsabile == attuale->utente) {
                        card->pingInviato = 0;
                        card->timestampUltimaModifica = time(NULL);
                        if (DEBUG) {
                            printf("PONG ricevuto per card %u da utente %u.\n",
                                id_card, attuale->utente);
                        }
                    }
                    break;
                }
                default: {
                    DBG(gestisci_messaggio_utente: DEFAULT);
                    break;
                }
            }
        }
        attuale = prox;
    }
}

void crea_card(utente_des_t*utente) {
    if (DEBUG) {
        printf("creazione nuova card...\n");
    }

    // Wire: [des_len][ID_card][colonna][descrizione\0], des_len = strlen+1
    uint32_t net_des_len = 0;
    ssize_t n_len = recv_fisso(utente->des, &net_des_len, sizeof(uint32_t));
    if (n_len <= 0) {
        perror("errore recv des_len");
        gestisci_quit(utente);
        return;
    }
    uint32_t des_len = ntohl(net_des_len);
    if (des_len == 0 || des_len > DES_ATT_LEN + 1) {
        printf("crea_card(): des_len non valida (%u).\n", des_len);
        return;
    }

    size_t total = 2 * sizeof(uint32_t) + des_len; // ID CARD + colonna + descrizione+\0
    char*buf = malloc(total);
    if (buf == NULL) {
        perror("crea_card(): malloc");
        return;
    }
    size_t received = 0;
    while (received < total) {
        ssize_t n = recv(utente->des, buf + received, total - received, 0);
        if (n <= 0) {
            printf("crea_card(): errore recv().\n");
            if (n<0)
                perror("errore recv():");
            free(buf);
            gestisci_quit(utente);
            return;
        }
        received += (size_t)n;
    }

    DBG(ARRIVATO);

    // buf è il puntatore al messaggio ricevuto
    // il messaggio è strutturato come: ID_carta colonna descrizione_att '\0'
    card_t*new_card = malloc(sizeof(card_t)); // creo una nuova carta
    if (new_card == NULL) {
        perror("crea_card(): malloc");
        free(buf);
        return;
    }
    new_card->responsabile = 0;
    new_card->timestampUltimaModifica = time(NULL);
    new_card->pingInviato = 0;
    new_card->prox = NULL;
    size_t offset = 0;
    // ID della card
    uint32_t id_carta;
    memcpy(&id_carta,&buf[offset],sizeof(uint32_t));
    id_carta = ntohl(id_carta);
    // controllo che ID carta non sia già stato usato
    if (check_id_card(id_carta)) {
        if (DEBUG) {
            printf("tentativo di aggiunta card con ID già utilizzato.\n");
        }
        // rispondi a utente
        send(utente->des, "1", 1, 0);
        free(new_card);
        free(buf);
        return;
    }
    // id disponibile
    new_card->ID = id_carta;
    offset += sizeof(uint32_t);

    // colonna di destinazione: 0=ToDo, 1=Doing (solo se libera), 2=Done
    uint32_t colonna;
    memcpy(&colonna, &buf[offset], sizeof(uint32_t));
    colonna = ntohl(colonna);
    offset += sizeof(uint32_t);
    if (colonna > 2) {
        colonna = 0;
    }

    // new_card->descrizione
    if (memchr(buf + offset, '\0', des_len) == NULL) {
        printf("crea_card(): descrizione non terminata.\n");
        send(utente->des, "1", 1, 0);
        free(new_card);
        free(buf);
        return;
    }
    size_t str_len = strlen(buf+offset) + 1; // descrizione + '\0'
    char*buf_des = malloc(str_len); // alloco zona di memoria per la descrizione
    if (buf_des == NULL) {
        perror("crea_card(): malloc");
        send(utente->des, "1", 1, 0);
        free(new_card);
        free(buf);
        return;
    }
    memcpy(buf_des, buf+offset,str_len); // ricopio in buf_des la descrizione contenuta nel messaggio
    new_card->descrizione = buf_des; // new_card->descrizione è il puntatore alla zona di memoria dove è salvata la descrizione

    free(buf);

    new_card->timestampUltimaModifica = time(NULL);

    // aggiungo la card in fondo alla colonna richiesta (Doing solo se l'utente è libero)
    card_t **dest = &testaToDo;
    if (colonna == 1 && !utente->doing) {
        dest = &testaDoing;
        new_card->responsabile = utente->utente;
        utente->doing = 1;
        num_carte_assegnate++;
    }
    else if (colonna == 2) {
        dest = &testaDone;
    }
    inserimento_lista_card(dest, new_card);
    if (DEBUG) {
        printf("nuova card creata.\n");
    }
    send(utente->des, "0", 1, 0);
    SHOW_LAVAGNA();
    // Una nuova card in ToDo può far scattare AVAILABLE_CARD
    if (testaToDo && utentiConnessi - num_carte_assegnate > 1) {
        AVAILABLE_CARD();
    }
}

void rimuovi_utente(utente_des_t*utente) {
    if (DEBUG) {
        printf("rimozione utente %d...\n", utente->utente);
    }
    utente_des_t *old = NULL;
    utente_des_t *this = testa_des_utente;
    if (this == NULL) { // la lista è vuota
        printf("lista descrittori utenti vuota.\n");
        return;
    }
    while (this != NULL && this != utente) {
        old = this;
        this = this->prox;
    }
    if (this == NULL) {
        printf("descrittore utente non presente nella lista.\n");
        return;
    }
    // scollego this dalla lista (corretto anche se è la testa)
    if (old == NULL) {
        testa_des_utente = this->prox;
    }
    else {
        old->prox = this->prox;
    }
    if (this->registrato) {
        printf("scollegato l'utente %d.\n", this->utente);
        utentiConnessi--;
    }
    else {
        printf("scollegata connessione in attesa di HELLO (sock %d).\n", this->des);
    }
    close(this->des);
    free(this);
    STAMPA_UTENTI();
}

int check_id_card(uint32_t ID) {
    card_t*attuale;
    for (int i = 0; i < 3; i++) {
        if (i == 0) {
            attuale = testaToDo;
        }
        else if (i == 1) {
            attuale = testaDoing;
        }
        else if (i == 2) {
            attuale = testaDone;
        }
        while (attuale != NULL) {
            if (attuale->ID == ID) { // ID già preso
                return 1;
            }
            attuale = attuale->prox;
        }
    }
    return 0;
}

int SEND_USER_LIST() {
    // controllo se ci sono utenti connessi (registrati)
    if (!utentiConnessi) {
        printf("Non ci sono utenti connessi.\n");
        return -1;
    }

    // lista dei registrati non in Doing (idonei a ricevere una card)
    int idonei = 0;
    for (utente_des_t *u = testa_des_utente; u != NULL; u = u->prox) {
        if (u->registrato && !u->doing) {
            idonei++;
        }
    }
    if (idonei <= 0) {
        return -1;
    }

    // manda comando+dimensione matrice
    uint32_t mes1[2];
    mes1[0] = htonl(ID_SEND_USER_LIST);
    mes1[1] = htonl((uint32_t)idonei);

    // creo una matrice con le porte degli idonei
    uint32_t *utenti = malloc((size_t)idonei * sizeof(uint32_t));
    if (utenti == NULL) {
        perror("SEND_USER_LIST(): malloc");
        return -2;
    }
    int i = 0;
    for (utente_des_t *u = testa_des_utente; u != NULL; u = u->prox) {
        if (u->registrato && !u->doing) {
            utenti[i++] = u->utente;
        }
    }

    int rc = 0;
    for (utente_des_t *u = testa_des_utente; u != NULL; u = u->prox) {
        if (!u->registrato) {
            continue;
        }
        // mando comando e dimensione matrice
        ssize_t err = send(u->des, mes1, sizeof(mes1), 0);
        if (err < 0) {
            perror("SEND_USER_LIST: errore send:");
            rc = -2;
            continue;
        }
        if (manda_matrice(u, utenti, idonei) < 0) {
            perror("SEND_USER_LIST: errore send:");
            rc = -2;
        }
    }
    free(utenti);
    return rc;
}

void AVAILABLE_CARD() {
    if (attesa_ack)
        return;
    // Spec (matricola pari): solo con almeno due utenti registrati idonei
    int idonei = 0;
    for (utente_des_t *u = testa_des_utente; u != NULL; u = u->prox) {
        if (u->registrato && !u->doing) {
            idonei++;
        }
    }
    if (idonei < 2) {
        return;
    }
    // scelgo la prima card
    if (!testaToDo) {
        return;
    }
    attesa_ack = 1;
    card_t*card = estrazione_lista_card(&testaToDo);
    inserimento_lista_card(&testaPendingCards,card);

    // creo messaggio: ID_CARD + descrizione
    char*buf = malloc(sizeof(uint32_t) + strlen(card->descrizione) + 1);
    if (buf == NULL) {
        perror("AVAILABLE_CARD(): malloc");
        // rimetto la card in testa alla ToDo
        card->prox = testaToDo;
        testaToDo = card;
        card_t *prior = NULL;
        card_t *p = trova_card(testaPendingCards, card->ID, &prior);
        if (p == card) {
            if (prior == NULL) {
                testaPendingCards = card->prox;
            }
            else {
                prior->prox = card->prox;
            }
            card->prox = NULL;
        }
        attesa_ack = 0;
        return;
    }
    uint32_t*tmp_int = (uint32_t*)buf;
    *tmp_int = htonl(card->ID);
    char*tmp = buf + sizeof(uint32_t);
    strcpy((char*)tmp,(const char*)card->descrizione); // include '\0'
    size_t len = sizeof(uint32_t) + strlen(card->descrizione) + 1; // lunghezza di @ref buf in byte

    // invio ID_comando + lunghezza messaggio
    uint32_t mes1[2];
    mes1[0] = htonl(ID_AVAILABLE_CARD);
    mes1[1] = htonl((uint32_t)len);

    // Per ogni destinatario: prima la sua SEND_USER_LIST (lista degli altri
    // idonei + conteggio totale, come da spec), poi la card.
    for (utente_des_t *lavoro = testa_des_utente; lavoro != NULL; lavoro = lavoro->prox) {
        if (!lavoro->registrato || lavoro->doing) {
            continue;
        }
        // lista degli altri idonei (escluso il destinatario)
        int altri = idonei - 1;
        uint32_t *mat = NULL;
        if (altri > 0) {
            mat = malloc((size_t)altri * sizeof(uint32_t));
            if (mat == NULL) {
                perror("AVAILABLE_CARD(): malloc");
                continue;
            }
            int k = 0;
            for (utente_des_t *u = testa_des_utente; u != NULL; u = u->prox) {
                if (u->registrato && !u->doing && u != lavoro) {
                    mat[k++] = u->utente;
                }
            }
        }
        uint32_t lista[2] = {htonl(ID_SEND_USER_LIST), htonl((uint32_t)altri)};
        int ok = 1;
        if (send(lavoro->des, lista, sizeof(lista), 0) < 0) {
            perror("AVAILABLE_CARD: errore send lista:");
            ok = 0;
        }
        else if (altri > 0 && manda_matrice(lavoro, mat, altri) < 0) {
            perror("AVAILABLE_CARD: errore send matrice:");
            ok = 0;
        }
        free(mat);
        if (!ok) {
            continue;
        }
        if (send(lavoro->des, mes1, sizeof(mes1), 0) < 0) { // mando comando e dimensione prox messaggio
            perror("AVAILABLE_CARD: errore send comando:");
            continue;
        }
        size_t sent = 0, total = len;
        while (sent < total) {
            ssize_t sent_now = send(lavoro->des, buf + sent, total - sent, 0);
            if (sent_now < 0) {
                perror("AVAILABLE_CARD: errore send():");
                break;
            }
            sent += (size_t)sent_now;
        }
    }
    // finito di mandare i messaggi
    free(buf);
    return;
}

utente_des_t* trova_utente(utente_t utente, utente_des_t **prior) {
    utente_des_t *lavoro = testa_des_utente;
    utente_des_t *prev = NULL;

    while (lavoro != NULL && lavoro->utente != utente) {
        prev = lavoro;
        lavoro = lavoro->prox;
    }

    if (prior)
        *prior = prev;

    return lavoro;   // NULL se non è stato trovato
}

int manda_matrice(utente_des_t*utente, uint32_t*mat, int len) {
    if (len <= 0) {
        return 0; // niente da inviare (lista vuota)
    }
    if (mat == NULL) {
        return -1;
    }
    uint32_t *mat_msg = malloc((size_t)len * sizeof(uint32_t));
    if (!mat_msg) {
        perror("manda_matrice(): Errore Malloc:");
        return -1;
    }
    for (int i = 0; i < len; i++) {
        mat_msg[i] = htonl(mat[i]);
    }
    size_t bytes_sent = 0;
    size_t total_bytes = len*(sizeof(uint32_t));
    while (bytes_sent < total_bytes) {
        ssize_t sent = send(utente->des, (char*)mat_msg + bytes_sent, total_bytes - bytes_sent, 0);

        if (sent < 0) {
            perror("Error sending matrix");
            free(mat_msg);
            return -1; // Send failed
        }
        bytes_sent += sent;
    }
    free(mat_msg);
    return 0;
}

void inserimento_lista_card(card_t**testaLista, card_t*newCard) {
    newCard->prox = NULL;
    card_t*lavoro = *testaLista;
    if (!*testaLista) {
        *testaLista = newCard;
        return;
    }
    while (lavoro->prox) {
        lavoro = lavoro->prox;
    }
    lavoro->prox = newCard;
}

card_t* estrazione_lista_card(card_t**testaLista) {
    if (!*testaLista)
        return NULL;
    // estraggo la card in testa
    card_t*card_estratta = *testaLista;
    *testaLista = (*testaLista)->prox;
    card_estratta->prox = NULL; // scollego la card estratta dal resto della lista
    return card_estratta;
}

void MOVE_CARD(card_t*card, card_t**listaDest, card_t**listaSorg) {
    card_t*prior = NULL;
    card_t*lavoro = trova_card(*listaSorg, card->ID, &prior);
    if (!lavoro) {
        DBG(MOVE_CARD: CARD NON TROVATA IN QUESTA LISTA);
        return;
    }
    if (!prior) {
        *listaSorg = lavoro->prox;
    }
    else {
        prior->prox = lavoro->prox;
    }
    lavoro->prox = NULL;

    inserimento_lista_card(listaDest, lavoro);
}

void ACK_CARD(utente_des_t*utente) {
    uint32_t net_id = 0;
    ssize_t n = recv_fisso(utente->des, &net_id, sizeof(uint32_t));
    if (n <= 0) {
        DBG(ACK_CARD: utente disconnesso oppurre errore di rete.);
        gestisci_quit(utente);
        return;
    }
    uint32_t ID = ntohl(net_id); /// ID della card da spostare
    if (DEBUG) {
        printf("Ricevuto ACK per card %u da parte di utente %u.\n", ID, utente->utente);
    }
    card_t*prior = NULL;
    card_t*card_accettata = trova_card(testaPendingCards,ID,&prior);
    if (!card_accettata) {
        printf("ACK_CARD: ACK per card %u non in pending, ignorato.\n", ID);
        return;
    }
    card_accettata->responsabile = utente->utente;
    card_accettata->timestampUltimaModifica = time(NULL);
    card_accettata->pingInviato = 0;
    MOVE_CARD(card_accettata, &testaDoing, &testaPendingCards);
    if (!utente->doing) {
        utente->doing = 1;
        num_carte_assegnate++;
    }

    attesa_ack = 0;
    SHOW_LAVAGNA();
    if (testaToDo && utentiConnessi - num_carte_assegnate > 1) {
        // ci sono altre card in todo, le invio
        AVAILABLE_CARD();
    }
    return;
}

card_t* trova_card(card_t*lista, uint32_t ID, card_t**prior) {
    card_t *lavoro = lista;
    card_t *prev = NULL;

    while (lavoro != NULL && lavoro->ID != ID) {
        prev = lavoro;
        lavoro = lavoro->prox;
    }

    if (prior)
        *prior = prev;

    return lavoro;   // NULL se non è stato trovato
}

void card_done(utente_des_t*utente) {
    // resetta utente->doing
    uint32_t net_id = 0;
    ssize_t n = recv_fisso(utente->des, &net_id, sizeof(uint32_t));
    if (n <= 0) {
        DBG(card_done: utente disconnesso oppurre errore di rete.);
        gestisci_quit(utente);
        return;
    }
    uint32_t ID = ntohl(net_id); /// ID della card da spostare
    if (DEBUG) {
        printf("Ricevuto CARD_DONE per card %u da parte di utente %u.\n", ID, utente->utente);
    }
    card_t*prior = NULL;
    card_t*card_completata = trova_card(testaDoing,ID,&prior);
    if (!card_completata) {
        printf("card_done: card %u non in Doing, ignorato.\n", ID);
        return;
    }
    if (card_completata->responsabile != utente->utente) {
        printf("card_done: card %u di utente %u, non di %u. Ignorato.\n",
            ID, card_completata->responsabile, utente->utente);
        return;
    }
    card_completata->timestampUltimaModifica = time(NULL);
    card_completata->pingInviato = 0;
    MOVE_CARD(card_completata, &testaDone, &testaDoing);
    if (utente->doing) {
        utente->doing = 0;
        num_carte_assegnate--;
    }

    SHOW_LAVAGNA();
    if (testaToDo && utentiConnessi - num_carte_assegnate > 1) {
        // ci sono altre card in todo, le invio
        AVAILABLE_CARD();
    }
    return;
}

void libera_card(card_t*card) {
    if (card == NULL) {
        return;
    }
    free(card->descrizione);
    free(card);
}

void seed_carte_iniziali() {
    static const char *descrizioni[10] = {
        "Analisi requisiti",
        "Progettazione DB",
        "Implementazione socket TCP",
        "Implementazione asta UDP",
        "Gestione card Doing",
        "Gestione card Done",
        "Test con 4 utenti",
        "Documentazione",
        "Revisione codice",
        "Collaudo finale"
    };
    for (uint32_t i = 0; i < 10; i++) {
        if (check_id_card(i + 1)) {
            continue;
        }
        card_t *c = malloc(sizeof(card_t));
        if (c == NULL) {
            perror("seed_carte_iniziali(): malloc");
            return;
        }
        c->ID = i + 1;
        c->descrizione = malloc(strlen(descrizioni[i]) + 1);
        if (c->descrizione == NULL) {
            perror("seed_carte_iniziali(): malloc");
            free(c);
            return;
        }
        strcpy(c->descrizione, descrizioni[i]);
        c->responsabile = 0;
        c->timestampUltimaModifica = time(NULL);
        c->pingInviato = 0;
        inserimento_lista_card(&testaToDo, c);
    }
}

void gestisci_hello(utente_des_t*utente) {
    // Wire HELLO: [ID_HELLO][porta_utente]
    uint32_t net_porta = 0;
    ssize_t n = recv_fisso(utente->des, &net_porta, sizeof(uint32_t));
    if (n <= 0) {
        rimuovi_utente(utente);
        return;
    }
    uint32_t porta = ntohl(net_porta);
    if (porta < 5679 || porta > 65535) {
        printf("HELLO con porta non valida (%u), connessione chiusa.\n", porta);
        send(utente->des, "NO", 2, 0);
        rimuovi_utente(utente);
        return;
    }
    if (trova_utente((utente_t)porta, NULL) != NULL) {
        printf("HELLO con porta %u già registrata, connessione chiusa.\n", porta);
        send(utente->des, "NO", 2, 0);
        rimuovi_utente(utente);
        return;
    }
    utente->utente = (utente_t)porta;
    utente->registrato = 1;
    utentiConnessi++;

    if (DEBUG) {
        printf("accettazione utente %u\n", utente->utente);
    }
    send(utente->des, "OK", 2, 0);
    STAMPA_UTENTI();

    // Spec (matricola pari): con almeno due utenti registrati si offre la
    // prima card in ToDo. AVAILABLE_CARD verifica da sé la condizione.
    if (testaToDo) {
        AVAILABLE_CARD();
    }
}

void gestisci_quit(utente_des_t*utente) {
    if (!utente->registrato) {
        rimuovi_utente(utente);
        return;
    }
    utente_t porta = utente->utente;
    // Spec: le card in Doing dell'utente uscente tornano in ToDo per riassegnarle
    card_t *c = testaDoing;
    while (c != NULL) {
        card_t *next = c->prox;
        if (c->responsabile == porta) {
            card_t *prior = NULL;
            if (trova_card(testaDoing, c->ID, &prior) != NULL) {
                if (prior == NULL) {
                    testaDoing = c->prox;
                }
                else {
                    prior->prox = c->prox;
                }
                c->prox = NULL;
                c->responsabile = 0;
                c->timestampUltimaModifica = time(NULL);
                c->pingInviato = 0;
                inserimento_lista_card(&testaToDo, c);
                if (utente->doing) {
                    utente->doing = 0;
                    num_carte_assegnate--;
                }
                printf("card %u rimessa in ToDo (uscita utente %u).\n", c->ID, porta);
            }
        }
        c = next;
    }
    // Se restano meno di 2 idonei, le pending non sono più assegnabili: tornano in ToDo
    int idonei = 0;
    for (utente_des_t *u = testa_des_utente; u != NULL; u = u->prox) {
        if (u->registrato && !u->doing && u != utente) {
            idonei++;
        }
    }
    if (testaPendingCards != NULL && idonei < 2) {
        card_t *p = testaPendingCards;
        while (p != NULL) {
            card_t *next = p->prox;
            p->prox = NULL;
            inserimento_lista_card(&testaToDo, p);
            p = next;
        }
        testaPendingCards = NULL;
        attesa_ack = 0;
        printf("card pending rimesse in ToDo (utenti rimasti < 2).\n");
    }
    rimuovi_utente(utente);
    SHOW_LAVAGNA();
    if (testaToDo) {
        AVAILABLE_CARD();
    }
}

void controllo_ping() {
    // card in Doing da più di PING_TIMEOUT_SEC -> PING_USER
    // senza PONG entro PONG_TIMEOUT_SEC -> torna in ToDo
    time_t ora = time(NULL);
    card_t *c = testaDoing;
    while (c != NULL) {
        card_t *next = c->prox;
        if (c->pingInviato == 0) {
            if ((ora - c->timestampUltimaModifica) >= PING_TIMEOUT_SEC) {
                utente_des_t *u = trova_utente(c->responsabile, NULL);
                if (u == NULL) {
                    // responsabile sparito senza QUIT: rimetti in ToDo
                    card_t *prior = NULL;
                    if (trova_card(testaDoing, c->ID, &prior) != NULL) {
                        if (prior == NULL) {
                            testaDoing = c->prox;
                        }
                        else {
                            prior->prox = c->prox;
                        }
                        c->prox = NULL;
                        c->responsabile = 0;
                        c->timestampUltimaModifica = ora;
                        c->pingInviato = 0;
                        inserimento_lista_card(&testaToDo, c);
                        printf("card %u rimessa in ToDo (responsabile assente).\n", c->ID);
                        SHOW_LAVAGNA();
                    }
                }
                else {
                    uint32_t msg[2] = {htonl(ID_PING_USER), htonl(c->ID)};
                    if (send(u->des, msg, sizeof(msg), 0) < 0) {
                        perror("controllo_ping(): send PING");
                    }
                    else {
                        c->pingInviato = ora;
                        printf("PING_USER per card %u a utente %u.\n", c->ID, u->utente);
                    }
                }
            }
        }
        else {
            if ((ora - c->pingInviato) >= PONG_TIMEOUT_SEC) {
                // Nessun PONG: utente presunto disconnesso
                printf("PONG non ricevuto per card %u, rimessa in ToDo.\n", c->ID);
                utente_des_t *u = trova_utente(c->responsabile, NULL);
                card_t *prior = NULL;
                if (trova_card(testaDoing, c->ID, &prior) != NULL) {
                    if (prior == NULL) {
                        testaDoing = c->prox;
                    }
                    else {
                        prior->prox = c->prox;
                    }
                    c->prox = NULL;
                    c->responsabile = 0;
                    c->timestampUltimaModifica = ora;
                    c->pingInviato = 0;
                    inserimento_lista_card(&testaToDo, c);
                    if (u != NULL && u->doing) {
                        u->doing = 0;
                        num_carte_assegnate--;
                    }
                    SHOW_LAVAGNA();
                    AVAILABLE_CARD();
                }
            }
        }
        c = next;
    }
}
