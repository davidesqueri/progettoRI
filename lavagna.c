#include "include/include_lavagna.h"

int main() {

    // Ignora SIGPIPE: una send() verso un socket già chiuso non deve terminare
    // la lavagna (default SIGPIPE = terminazione del processo).
    signal(SIGPIPE, SIG_IGN);

    // La spec richiede almeno 10 card tutte in "To Do" all'avvio
    seed_carte_iniziali();

    // 1. mostra a video lo stato della lavagna
    SHOW_LAVAGNA();

    // ad ogni spostamento o creazione di una card, mostra la lavagna

    // se una card è in Doing da più di PING_TIMEOUT_SEC, ping utente, se non
    // riceve risposta dopo PONG_TIMEOUT_SEC rimette la card in ToDo
    // (controllo_ping() nel loop, vedi sotto)

    // se ci sono almeno due utenti viene mandato a tutti AVAILABLE_CARD.
    // Questa operazione verrà ripetuta ogni volta che un utente ha terminato una
    // card e ci sono almeno due utenti presenti e card disponibili.

    //////////////////////////////////////////////////////////////////////////////////////////////////////
    //////////////////////////////////inizializzazione socket di ascolto nuove
    ///connessioni
    if (DEBUG) {printf("porta da collegare: %d\n", PORTA_LAVAGNA);}

    struct sockaddr_in my_addr;
    memset(&my_addr, 0, (size_t)sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(PORTA_LAVAGNA);
    inet_pton(AF_INET, LOCALHOST, &my_addr.sin_addr);

    mysock_lavagna = socket(AF_INET, SOCK_STREAM, 0);
    if (mysock_lavagna < 0) {
        perror("errore su socket: ");
        return -1;
    }
    if (DEBUG) {printf("Aperto socket %d\n", mysock_lavagna);}

    int opt = 1;
    setsockopt(mysock_lavagna, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int err_bind = bind(mysock_lavagna, (struct sockaddr *)&my_addr,
                        (socklen_t)sizeof(my_addr));
    if (err_bind) {
        perror("errore su bind: ");
        return -1;
    }
    if (DEBUG) {printf("Socket collegato\n");}

    int err_listen = listen(mysock_lavagna, BACKLOG);
    if (err_listen) {
        perror("errore su listen: ");
        return -1;
    }
    if (DEBUG) {printf("socket in ascolto per nuove connessioni...\n");}
    ////////////////////////////////////////////////////////////////////
    //////////////////////////////////////////////////////////////////////////////////////////////////////

    int des_pronti = 0;

    printf("Inserire un comando:\n");
    while (1) {
        prepara_set();
        DBG(Controllo socket con select()...);
        // Timeout di 1s: serve per eseguire controllo_ping() anche senza traffico
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        des_pronti = select(max_des + 1, &readfds, NULL, NULL, &tv);

        if (DEBUG) {
            printf("numero di descrittori pronti: %d\n", des_pronti);
        }

        if (des_pronti == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("errore select");
            exit(-1);
        }

        // 1. controllo input da terminale (senza goto: non deve affamare i client)
        if (!stdin_chiusa && FD_ISSET(STDIN_FILENO, &readfds)) {
            if (DEBUG) {
                printf("gestisco input da terminale\n");
            }
            input_stdin();
        }

        // 2. Controllo nuove connessioni
        if (FD_ISSET(mysock_lavagna, &readfds)) {
            if (DEBUG) {
                printf("gestisco nuove connessioni\n");
            }
            accetta_utente();
        }

        // 3. controlla connessioni esistenti (tutte quelle pronte, non solo la prima)
        gestisci_messaggio_utente();

        // 4. controllo periodico PING_USER/PONG_LAVAGNA sulle card in Doing
        controllo_ping();
    }
}

/*
    la lavagna è identificata da:
        - ID
        - colonne
        - card
    le card sono almeno 10
    ogni card è identificata da:
        - ID
        - colonna
        - testo attività
        - utente che la sta facendo oppure utente che l'ha fatta
        - timestamp dell'ultima modifica
    gli utenti sono identificati dal loro numero di porta (incrementale a
    partire dalla 5679) per prima cosa l'utente contatta la lavagna e si registra
*/
