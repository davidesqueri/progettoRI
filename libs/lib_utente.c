#include "../include/include_utente.h"

int connesso = 0;
int mysock_utente;
int mysock_udp;
fd_set readfds; // set di descrittori da controllare per la lettura
int my_port;
int stdin_chiuso = 0;

/// puntatore alla testa della lista delle carte assegnate all'utente
card_t *testa_lista_card;

/// Puntatore alla lista degli utenti connessi, punta ad una matrice di uint32_t
utente_t*lista_utenti;
/// Numero di utenti nella lista
int numero_utenti = 0;

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

uint32_t rando() {
    int n = rand();
    return n;
}

void ACK_CARD(card_t*card) {
    // comunica alla lavagna la card che gli è stata assegnata

    // comunico ID_COMANDO + ID_card
    uint32_t msg[2] = {htonl(ID_ACK_CARD), htonl(card->ID)};
    int n = send(mysock_utente,&msg, sizeof(msg),0);
    if (n <= 0) {
        printf("ACK_CARD: errore send.\n");
        if (n < 0)
            perror("errore send:");
        exit(-1);
    }
}

void CARD_DONE(card_t*card) {
    // comunica alla lavagna la terminazione della card

    // comunico ID_COMANDO + ID_card
    uint32_t msg[2] = {htonl(ID_CARD_DONE), htonl(card->ID)};
    int n = send(mysock_utente,&msg, sizeof(msg),0);
    if (n <= 0) {
        printf("CARD_DONE: errore send.\n");
        if (n < 0)
            perror("errore send:");
        exit(-1);
    }
}

void QUIT () {
    // notifica uscita alla lavagna con ID_QUIT (best effort: la lavagna
    // rimette in ToDo le mie card in Doing per riassegnarle), poi chiudo
    if (connesso) {
        uint32_t msg = htonl(ID_QUIT);
        (void)send(mysock_utente, &msg, sizeof(msg), 0);
    }
    close(mysock_utente); // invia FIN alla lavagna
    exit(0);
}

void PONG_LAVAGNA(uint32_t id_card) {
    if (DEBUG) {
        printf("PONG_LAVAGNA\n");
    }
    // Wire PONG: [ID_PONG_LAVAGNA][ID_card]
    uint32_t msg[2] = {htonl(ID_PONG_LAVAGNA), htonl(id_card)};
    if (send(mysock_utente, msg, sizeof(msg), 0) < 0) {
        perror("PONG_LAVAGNA: errore send:");
        QUIT();
    }
    printf("PONG inviato per card %u.\n", id_card);
}

void CREATE_CARD() {
    // comunico alla lavagna una nuova carta
    // servono ID, colonna (0=ToDo, 1=Doing, 2=Done) e testo attività

    if (!connesso) {
        printf("comando non disponibile senza una connessione con la lavagna.\n");
        return;
    }

    // --- ID carta: intero 0..2^31-1, input validato riga per riga ---
    long id_carta = -1;
    while (id_carta < 0) {
        printf("Inserire ID carta (numero >= 0):\n");
        char buf_id[32] = {0};
        if (fgets(buf_id, sizeof(buf_id), stdin) == NULL) {
            return;
        }
        if (strchr(buf_id, '\n') == NULL) {
            // riga più lunga del buffer: scarta il resto
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { }
            printf("input troppo lungo, ritentare...\n");
            continue;
        }
        char *end = NULL;
        errno = 0;
        long v = strtol(buf_id, &end, 10);
        if (errno != 0 || end == buf_id || (*end != '\n' && *end != '\0') || v < 0 || v > 2147483647L) {
            printf("non è un numero valido, ritentare...\n");
            continue;
        }
        id_carta = v;
    }
    if (DEBUG) {
        printf("numero inserito: %ld\n", id_carta);
    }

    // --- colonna di destinazione ---
    long colonna = 0;
    while (1) {
        printf("Inserire colonna (0=ToDo, 1=Doing, 2=Done) [default 0]:\n");
        char buf_col[16] = {0};
        if (fgets(buf_col, sizeof(buf_col), stdin) == NULL) {
            return;
        }
        if (strchr(buf_col, '\n') == NULL) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { }
            printf("input troppo lungo, ritentare...\n");
            continue;
        }
        if (buf_col[0] == '\n') {
            colonna = 0;
            break;
        }
        char *end = NULL;
        errno = 0;
        long v = strtol(buf_col, &end, 10);
        if (errno != 0 || end == buf_col || (*end != '\n' && *end != '\0') || v < 0 || v > 2) {
            printf("colonna non valida (0, 1 o 2), ritentare...\n");
            continue;
        }
        colonna = v;
        break;
    }

    ////////////////////////

    printf("Inserire descrizione attività:\n");
    char buf_des[DES_ATT_LEN];

    if (fgets(buf_des, DES_ATT_LEN, stdin) == NULL) return;
    size_t des_len = strlen(buf_des);
    if (des_len > 0 && buf_des[des_len - 1] == '\n') { // rimuovo il newline
        buf_des[des_len - 1] = '\0';
    }

    // Wire: [ID_CMD][des_len(strlen+1)][ID_card][colonna][descrizione\0]
    size_t total_len = 4*sizeof(uint32_t) + strlen(buf_des) + 1;
    char* buf = (char*)malloc(total_len);
    uint32_t string_len = htonl((uint32_t)(strlen(buf_des) + 1));

    if (buf == NULL) { // controllo errore nella malloc
        perror("malloc error");
        return;
    }

    size_t offset = 0;

    uint32_t net_command_id = htonl(ID_CREATE_CARD); // porto in endianess di rete
    uint32_t net_id_carta = htonl((uint32_t)id_carta);
    uint32_t net_colonna = htonl((uint32_t)colonna);

    memcpy(buf + offset, &net_command_id, sizeof(uint32_t)); // ID del comando
    offset += sizeof(uint32_t);

    memcpy(buf + offset, &string_len, sizeof(uint32_t)); // lunghezza descrizione +1
    offset += sizeof(uint32_t);

    memcpy(buf + offset, &net_id_carta, sizeof(uint32_t)); // ID della carta
    offset += sizeof(uint32_t);

    memcpy(buf + offset, &net_colonna, sizeof(uint32_t)); // colonna di destinazione
    offset += sizeof(uint32_t);

    memcpy(buf + offset, buf_des, strlen(buf_des) + 1 /*per via del '\0'*/); // descrizione attività

    if (send(mysock_utente, buf, total_len, 0) < 0) { // invio il messaggio completo alla lavagna
        perror("send error");
    } else {
        printf("Richiesta CREATE_CARD inviata. Totale %zu bytes.\n", total_len);
        char buf_risposta[2] = {0};
        int received = recv(mysock_utente, buf_risposta, 1, 0);
        controlla_connessione(received);
        // CONTROLLA BUF_RISPOSTA, 1=ID non disponibile, 0=ID disponibile
        buf_risposta[1] = '\0';
        if (strcmp("0", buf_risposta) == 0) { // se ricevo 0 da lavagna, ID è disponibile
            printf("card accettata.\n");
            if (colonna == 1) {
                // la card è in Doing e assegnata a me: comincio a lavorarci.
                // Costruisco una card locale: a CARD_DONE serve solo l'ID.
                card_t card = {0};
                card.ID = (uint32_t)id_carta;
                int tempo_lavoro = 1 + rando() % 100;
                printf("Durata Lavoro Card: %i secondi\n", tempo_lavoro);
                printf("Lavorando sulla carta...\n");

                for (int i = tempo_lavoro; i > 0; i--) {
                    // controlla ping lavagna
                    // 1. controllo se ci siano messaggi dalla lavagna
                    char peek[16];
                    ssize_t bytes = recv(mysock_utente, (void*)peek, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (bytes > 0) {
                        // gestisco il messaggio della lavagna
                        gestisci_messaggio_lavagna();
                    }
                    else if (bytes == 0) {
                        printf("\nLa lavagna ha chiuso la connessione, disconnessione...\n");
                        QUIT();
                    }
                    // torno a gestire la card
                    printf("\rTempo rimanente: %3d secondi",i);
                    fflush(stdout);
                    sleep(1);
                }

                // Lavoro finito: comunico il completamento alla lavagna
                CARD_DONE(&card);
                printf("\rLavoro terminato!             \n");
            }
        }
        else { // lavagna invia 1: ID non disponibile
            printf("ID card non disponibile, ritentare.\n");
        }
    }

    free(buf);
}

void CHOOSE_USER(card_t*card) {
    // messaggio che si scambiano gli utenti, associando un costo all'esecuzione della card attualmente discussa
    // gli utenti convergono nell'assegnare la card all'utente che ha il costo minore

    // gli utenti comunicano UDP i propri costi, ogni utente deve sapere il costo di tutti gli altri utenti
    // l'utente vincitore chiama ACK_CARD, gli altri ritornano

    // Pacchetto UDP: [ID_CARD, MIO_PORT, MIO_COSTO] (l'ID card scarta i
    // pacchetti stantii delle aste precedenti)
    // numero_utenti = numero di peer nella lista (io escluso): asta ok con >=1
    if (numero_utenti < 1) {
        printf("Asta impossibile: nessun altro utente (%d).\n", numero_utenti);
        return;
    }

    // 1. genero un costo casuale
    uint32_t costo = rando();
    printf("Inizio asta per la carta %u. Il mio costo è: %u\n", card->ID, costo);

    // 2. mando il mio costo a tutti gli utenti
    for (int i = 0; i < numero_utenti; i++) {
        uint32_t id_dest = lista_utenti[i];
        if (id_dest == (uint32_t)my_port)
            continue; // Non mando il costo a me stesso
        struct sockaddr_in dest_addr;
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons((uint16_t)id_dest);
        inet_pton(AF_INET, "127.0.0.1", &dest_addr.sin_addr);

        // Pacchetto: [ID_CARD, MIO_PORT, MIO_COSTO]
        uint32_t pkt[3] = { htonl(card->ID), htonl((uint32_t)my_port), htonl(costo) };
        (void)sendto(mysock_udp, pkt, sizeof(pkt), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    }
    if (DEBUG) {
        printf("Inviato il mio costo a tutti gli altri utenti.\n");
    }

    // 3. leggo tutti i costi che mi sono stati inviati dagli altri utenti.
    // Attesa con timeout (select, 10s): evita il blocco infinito se un utente
    // esce a metà asta; i pacchetti di altre card vengono scartati.
    // La lista può includermi (SEND_USER_LIST broadcast) o no (AVAILABLE_CARD
    // per-recipient): i voti attesi sono le voci altrui.
    int presente = 0;
    for (int i = 0; i < numero_utenti; i++) {
        if (lista_utenti[i] == (uint32_t)my_port) {
            presente = 1;
            break;
        }
    }
    uint32_t min_costo = costo;
    uint32_t vincitore = (uint32_t)my_port;
    int voti_ricevuti = 0;
    int attesi = numero_utenti - presente;

    // Devo ricevere da tutti gli altri
    while (voti_ricevuti < attesi) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mysock_udp, &rfds);
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        int pronti = select(mysock_udp + 1, &rfds, NULL, NULL, &tv);
        if (pronti < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("CHOOSE_USER: select UDP:");
            break;
        }
        if (pronti == 0) {
            printf("CHOOSE_USER: timeout, decido con %d/%d voti ricevuti.\n",
                voti_ricevuti, attesi);
            break;
        }
        uint32_t pkt_recv[3];
        struct sockaddr_in mitt_addr;
        socklen_t mitt_len = sizeof(mitt_addr);

        ssize_t n = recvfrom(mysock_udp, pkt_recv, sizeof(pkt_recv), 0, (struct sockaddr*)&mitt_addr, &mitt_len);
        if (n < (ssize_t)sizeof(pkt_recv)) {
            continue; // pacchetto corto/spurio
        }
        uint32_t id_carta_rx = ntohl(pkt_recv[0]);
        if (id_carta_rx != card->ID) {
            continue; // pacchetto stantio di un'asta precedente
        }
        uint32_t id_mittente = ntohl(pkt_recv[1]);
        uint32_t costo_ricevuto = ntohl(pkt_recv[2]);
        printf("Ricevuto costo %u dall'utente %u\n", costo_ricevuto, id_mittente);

        // determinare il minore (a parità di costo vince porta minore)
        if (costo_ricevuto < min_costo ||
            (costo_ricevuto == min_costo && id_mittente < vincitore)) {
            min_costo = costo_ricevuto;
            vincitore = id_mittente;
        }
        voti_ricevuti++;
    }
    DBG(Ricevuti tutti i costi.);

    // 4. se ho il costo minore mando ACK_CARD(card)
    if (vincitore == (uint32_t)my_port) {
        printf(">>> HO VINTO IO! Assegno a me la carta %u...\n", card->ID);
        // Invio ACK_CARD alla lavagna (tcp)
        ACK_CARD(card);

        int tempo_lavoro = 1 + rando()%100;
        printf("Durata Lavoro Card: %i secondi\n", tempo_lavoro);
        printf("Lavorando sulla carta...\n");

        for (int i = tempo_lavoro; i > 0; i--) {
            // controlla ping lavagna
            // 1. controllo se ci siano messaggi dalla lavagna
            char peek[16];
            ssize_t bytes = recv(mysock_utente, (void*)peek, 1, MSG_PEEK | MSG_DONTWAIT);
            if (bytes > 0) {
                // gestisco il messaggio della lavagna
                gestisci_messaggio_lavagna();
            }
            else if (bytes == 0) {
                printf("\nLa lavagna ha chiuso la connessione, disconnessione...\n");
                QUIT();
            }
            // torno a gestire la card
            printf("\rTempo rimanente: %3d secondi",i);
            fflush(stdout);
            sleep(1);
        }

        // 6. Ho finito
        CARD_DONE(card);
        printf("\rLavoro terminato!             \n");
    }
    else {
        printf("Ha vinto l'utente %u con costo %u. Mi rimetto in attesa.\n", vincitore, min_costo);
    }
}

void HELLO(int*mysock_utente, struct sockaddr_in*lavagna) {
    if (connesso) {
        printf("connessione con la lavagna già effettuata.\n");
        return;
    }

    int err_connect = connect(*mysock_utente, (const struct sockaddr*)lavagna, (socklen_t)sizeof(*lavagna));
    if (err_connect) {
        perror("Errore connect: ");
        exit(-1);
    }

    // Wire HELLO: [ID_HELLO][mia_porta]; la lavagna risponde "OK" o "NO"
    uint32_t msg[2] = {htonl(ID_HELLO), htonl((uint32_t)my_port)};
    if (send(*mysock_utente, msg, sizeof(msg), 0) < 0) {
        perror("Errore send HELLO: ");
        exit(-1);
    }

    // ricezione ACK da lavagna
    char buf_temp[3] = {0};
    ssize_t n = recv_fisso(*mysock_utente, buf_temp, 2); // recv() NON aggiunge '\0'
    if (n <= 0) {
        printf("Connessione chiusa dalla lavagna.\n");
        exit(-1);
    }
    if (strcmp(buf_temp,"OK") == 0) {
        printf("Connesso alla lavagna\n");
        connesso = 1;
    }
    else {
        printf("Registrazione rifiutata dalla lavagna.\n");
        printf("messaggio ricevuto: %s\n", buf_temp);
        exit(-1);
    }
}

void controlla_connessione(int received) {
    if (received<=0) {
        printf("Lavagna disconnessa, terminazione...\n");
        exit(0);
    }
}

int rileva_input () {
    int n_des_pronti, max_des = 0;
    FD_ZERO(&readfds);

    // aggiungo il descrittore del socket connesso alla lavagna
    if (connesso) {
        FD_SET(mysock_utente, &readfds);
        if (mysock_utente > max_des) {
            max_des = mysock_utente;
        }
    }

    // aggiungo il descrittore di stdin (salvo EOF già rilevato: un fd a EOF
    // risulta sempre pronto e causerebbe busy-loop)
    if (!stdin_chiuso) {
        FD_SET(STDIN_FILENO, &readfds);
        if (STDIN_FILENO > max_des) { // Di solito STDIN_FILENO è 0, ma è buona norma
            max_des = STDIN_FILENO;
        }
    }

    // Timeout di 1s: senza, uno stdin a EOF (input ridiretto) causerebbe
    // busy-loop; con il timeout il ciclo resta reattivo verso la lavagna
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    n_des_pronti = select(max_des + 1, &readfds, NULL, NULL, &tv);

    // Controllo se ci sono errori
    if (n_des_pronti == -1) {
        if (errno == EINTR) {
            return 0;
        }
        perror("errore select():");
        QUIT();
    }

    // timeout o nessun descrittore pronto: niente da fare in questo giro
    if (!n_des_pronti)
        return 0;

    // controllo se entrambi i descrittori siano pronti
    if (FD_ISSET(mysock_utente, &readfds) && FD_ISSET(STDIN_FILENO, &readfds))
        return 3;

    /// Controllo Socket
    if (FD_ISSET(mysock_utente, &readfds)) {
        return 1;
    }

    /// Controllo STDIN
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
        return 2;
    }

    return 0;
}

void gestisci_messaggio_lavagna() {
    // leggi dal socket
    // determina che comando è
    // agisci di conseguenza

    // 1. leggo 1 uint32_t (lettura completa: il TCP può frammentare)
    uint32_t net_cmd = 0;
    ssize_t n = recv_fisso(mysock_utente, &net_cmd, sizeof(uint32_t));
    if (n <= 0) {
        printf("Terminazione della lavagna. Chiusura.\n");
        QUIT();
    }
    uint32_t ID_comando = ntohl(net_cmd);
    switch (ID_comando) {
        case ID_SEND_USER_LIST: {
            printf("Ricezione lista utenti.\n");
            receive_user_list();
            break;
        }
        case ID_AVAILABLE_CARD: {
            printf("Ricezione nuova card.\n");
            receive_available_card();
            break;
        }
        case ID_PING_USER: {
            printf("\nRicezione PING dalla lavagna.\n");
            uint32_t net_id = 0;
            if (recv_fisso(mysock_utente, &net_id, sizeof(uint32_t)) <= 0) {
                printf("Terminazione della lavagna. Chiusura.\n");
                QUIT();
            }
            PONG_LAVAGNA(ntohl(net_id));
            break;
        }
        default: {
            printf("Ricevuto messaggio non riconosciuto, terminazione..\n");
            QUIT();
        }
    }
}

ssize_t recv_all(int sockfd, void *buf, size_t len) {
    size_t bytes_received = 0;
    char *ptr = (char *)buf;

    while (bytes_received < len) {
        ssize_t r = recv(sockfd, ptr + bytes_received, len - bytes_received, 0);
        if (r == 0) {
            // il server ha chiuso la connessione
            printf("La lavagna ha chiuso la connessione.\n");
            QUIT();
            return 0;
        } else if (r < 0) {
            /// errore di rete
            perror("Errore in recv_all");
            QUIT();
            return -1;
        }
        bytes_received += r;
    }
    return bytes_received; // Ritornerà sempre esattamente 'len'
}

void receive_available_card() {
    // ID_COMANDO è già stato letto
    // devo leggere lunghezza messaggio
    uint32_t net_len = 0; // lunghezza del messaggio da leggere in byte
    recv_all(mysock_utente, &net_len, sizeof(uint32_t));
    uint32_t len = ntohl(net_len);
    // devo leggere ID_CARD + descrizione
    if (len < sizeof(uint32_t) || len > sizeof(uint32_t) + DES_ATT_LEN + 1) {
        printf("receive_available_card(): lunghezza non valida (%u).\n", len);
        return;
    }

    /// leggo ID card
    uint32_t net_id = 0;
    recv_all(mysock_utente, &net_id, sizeof(uint32_t));
    uint32_t ID_card = ntohl(net_id);
    len -= sizeof(uint32_t);

    /// leggo la descrizione
    char*descrizione = malloc(len);
    if (descrizione == NULL) {
        perror("receive_available_card(): malloc");
        return;
    }
    recv_all(mysock_utente, descrizione, len);
    if (memchr(descrizione, '\0', len) == NULL) {
        printf("receive_available_card(): descrizione non terminata.\n");
        free(descrizione);
        return;
    }

    /// creo la card
    card_t*card = malloc(sizeof(card_t));
    if (card == NULL) {
        perror("receive_available_card(): malloc");
        free(descrizione);
        return;
    }
    card->ID = ID_card;
    card->descrizione = descrizione;
    card->responsabile = 0;
    card->timestampUltimaModifica = time(NULL);
    card->pingInviato = 0;
    card->prox = NULL;

    sleep(1); // per dare il tempo alla maggior parte degli utenti di ricevere la card
    /// comincio il protocollo di assegnamento UDP tra utenti
    CHOOSE_USER(card);

    free(card->descrizione);
    free(card);
}

void receive_user_list() {
    // 1. leggo la dimensione della matrice in arrivo
    uint32_t net_num = 0; // il NUMERO di utenti nella lista
    ssize_t n = recv_fisso(mysock_utente, &net_num, sizeof(uint32_t));
    if (n <= 0) {
        printf("receive_user_list(): errore recv 1.\n");
        if (n < 0) {
            perror("errore recv():");
        }
        return;
    }
    uint32_t num_utenti_rx = ntohl(net_num);
    // validazione: la lista esclude al più me stesso, limite superiore anti-spam
    if (num_utenti_rx == 0 || num_utenti_rx > 1024) {
        printf("receive_user_list(): conteggio non valido (%u).\n", num_utenti_rx);
        return;
    }
    size_t len = sizeof(uint32_t) * num_utenti_rx; // il numero di byte che compongono la matrice

    // 2. leggo la matrice
    uint32_t *tmp = malloc(len);
    if (tmp == NULL) {
        perror("receive_user_list(): malloc");
        return;
    }
    n = recv_all(mysock_utente, tmp, len);
    if (n <= 0) {
        printf("receive_user_list(): errore recv 2.\n");
        if (n < 0) {
            perror("errore recv():");
        }
        free(tmp);
        return;
    }
    // 3. salvo la matrice
    free(lista_utenti);
    lista_utenti = malloc(len);
    if (lista_utenti == NULL) {
        perror("receive_user_list(): malloc");
        free(tmp);
        return;
    }
    for (uint32_t i = 0; i < num_utenti_rx; i++) {
        lista_utenti[i] = ntohl(tmp[i]);
    }
    numero_utenti = (int)num_utenti_rx;
    free(tmp);
}
