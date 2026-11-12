#include "include/include_utente.h"

int main (int argc, char *argv[]) {
    if (argc != 2) {
        printf("ERR:\tincludere come argomento il numero di porta\n");
        return -1;
    }
    else if (atoi(argv[1]) < 5679) {
        printf("inserire un numero di porta maggiore di 5678\n");
        return -1;
    }
    // argv[0] : comando eseguito
    // argv[1] : numero di porta, (parte da 5679 incluso)

    // Ignora SIGPIPE: una send() verso un socket già chiuso non deve terminare
    // l'utente (default SIGPIPE = terminazione del processo).
    signal(SIGPIPE, SIG_IGN);

    srand(time(NULL)); // inizializzo il seme del generatore

    my_port = atoi(argv[1]);
    // connessione TCP bloccante alla lavagna
    int port = htons(atoi(argv[1]));
    if (DEBUG) {
        printf("porta da collegare: %d\n", atoi(argv[1]));
    }

    // addr mio
    struct sockaddr_in my_addr, lavagna;
    memset(&my_addr, 0, (size_t)sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = port;
    inet_pton(AF_INET, LOCALHOST, &my_addr.sin_addr);

    // addr lavagna
    //int lavagnaLen = sizeof(lavagna);
    memset(&lavagna, 0, (size_t)sizeof(lavagna));
    lavagna.sin_family = AF_INET;
    lavagna.sin_port = htons(PORTA_LAVAGNA);
    inet_pton(AF_INET, LOCALHOST, &lavagna.sin_addr);

    mysock_utente = socket(AF_INET, SOCK_STREAM, 0);
    if (mysock_utente < 0) {
        perror("errore su socket: ");
        return -1;
    }
    int opt = 1;
    setsockopt(mysock_utente, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (DEBUG) {
        printf("Aperto socket %d\n", mysock_utente);
    }

    int err_bind = bind(mysock_utente, (struct sockaddr*)&my_addr, (socklen_t)sizeof(my_addr));
    if (err_bind) {
        perror("errore su bind: ");
        return -1;
    }
    if (DEBUG) {
        printf("Socket collegato\n");
    }

    ///////// Apro un socket UDP e lo collego alla porta
    mysock_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (mysock_udp < 0) {
        perror("Errore creazione socket UDP:");
        exit(-1);
    }
    setsockopt(mysock_udp, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // bind
    struct sockaddr_in my_udp_addr;
    memset(&my_udp_addr, 0, sizeof(my_udp_addr));
    my_udp_addr.sin_family = AF_INET;
    //my_udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_udp_addr.sin_port = port; // port è già in network endianess
    err_bind = bind(mysock_udp, (struct sockaddr*)&my_udp_addr, (socklen_t)sizeof(my_udp_addr));
    if (err_bind) {
        perror("errore bind UDP.");
        exit(-1);
    }
    //////// aperto e collegato il socket UDP

    char buf[MAX_LEN_COMANDO + 2]; // buffer per il comando da terminale, dimensionato per il comando più lungo possibile + \n + \0

    printf("Inserire un comando:\n");
    while(1) {
        int des = rileva_input();
        switch (des) {
            case 0: {
                // timeout select (1s): niente da fare in questo giro
                break;
            }
            case 3: {
                // messaggi sia da lavagna che da terminale
                DBG(MESSAGGIO SIA DA LAVAGNA CHE DA TERMINALE);
            }
            case 1: {
                DBG(gestisco messaggio da lavagna);
                gestisci_messaggio_lavagna();
                printf("Inserire un comando:\n");
                break;
            }
            case 2: {
                // messaggio dal terminale
                DBG(Gestione comando da terminale.);
                printf("Inserire un comando:\n");
                if (fgets(buf, sizeof(buf), stdin) == NULL) { // questo legge al più (numero di caratteri disponibili in buf - 1), ed in fondo aggiunge \0
                    // EOF/errore su stdin (es. input ridiretto terminato):
                    // non validare il vecchio contenuto di buf e smetti di
                    // monitorare stdin, altrimenti si va in busy-loop
                    stdin_chiuso = 1;
                    clearerr(stdin);
                    break;
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
                        printf("comando non riconosciuto. Riprovare...\n");
                        break;
                    }
                    case ID_HELLO: {
                        HELLO(&mysock_utente, (struct sockaddr_in *)&lavagna);
                        break;
                    }
                    case ID_QUIT: {
                        QUIT();
                        break;
                    }
                    case ID_CREATE_CARD: {
                        CREATE_CARD();
                        break;
                    }
                    case ID_CARD_DONE: {
                        //CARD_DONE();
                        printf("Comando non disponibile.\n");
                        break;
                    }
                    default: {
                        printf("Comando non disponibile.\n");
                    }
                }
                printf("Inserire un comando:\n");
            }
        }
    }
    close(mysock_utente);
    DBG(chiusura.);
    return 0;
}
