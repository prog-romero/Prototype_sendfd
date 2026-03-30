#include <wolfssl/options.h> // TOUJOURS EN PREMIER
#include <wolfssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h"

/**
 * Reçoit le descripteur de fichier (FD) et les données de session TLS
 * via un socket Unix Domain (UDS) en utilisant SCM_RIGHTS.
 * Retourne 0 en succès, -1 en échec.
 */
int receive_handoff(int uds_conn, int* client_fd, migration_msg_t* msg) {
    struct msghdr msgh;
    struct iovec iov[1];
    struct cmsghdr *cmsg;
    char control[CMSG_SPACE(sizeof(int))];

    memset(&msgh,   0, sizeof(msgh));
    memset(control, 0, sizeof(control));

    iov[0].iov_base = msg;
    iov[0].iov_len  = sizeof(migration_msg_t);

    msgh.msg_name       = NULL;
    msgh.msg_namelen    = 0;
    msgh.msg_iov        = iov;
    msgh.msg_iovlen     = 1;
    msgh.msg_control    = control;
    msgh.msg_controllen = sizeof(control);

    ssize_t n = recvmsg(uds_conn, &msgh, 0);
    if (n <= 0) {
        perror("[Worker] recvmsg");
        return -1;
    }

    if (msgh.msg_flags & MSG_CTRUNC) {
        fprintf(stderr, "[Worker] Message de contrôle tronqué !\n");
        return -1;
    }

    cmsg = CMSG_FIRSTHDR(&msgh);
    if (cmsg
        && cmsg->cmsg_level == SOL_SOCKET
        && cmsg->cmsg_type  == SCM_RIGHTS
        && cmsg->cmsg_len   == CMSG_LEN(sizeof(int)))
    {
        memcpy(client_fd, CMSG_DATA(cmsg), sizeof(int));
        return 0;
    }

    fprintf(stderr, "[Worker] Aucun FD reçu dans les données de contrôle.\n");
    return -1;
}

int main() {
    wolfSSL_Init();

    WOLFSSL_CTX* ctx = wolfSSL_CTX_new(wolfTLSv1_2_server_method());
    if (!ctx) {
        fprintf(stderr, "[Worker] Erreur création du CTX\n");
        return EXIT_FAILURE;
    }

    if (wolfSSL_CTX_use_certificate_file(ctx, "server-cert.pem", SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[Worker] Erreur chargement certificat\n");
        wolfSSL_CTX_free(ctx);
        return EXIT_FAILURE;
    }
    if (wolfSSL_CTX_use_PrivateKey_file(ctx, "server-key.pem", SSL_FILETYPE_PEM) != SSL_SUCCESS) {
        fprintf(stderr, "[Worker] Erreur chargement clé privée\n");
        wolfSSL_CTX_free(ctx);
        return EXIT_FAILURE;
    }

    /* Serveur Unix Domain Socket */
    int uds_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds_fd < 0) {
        perror("[Worker] socket UDS");
        return EXIT_FAILURE;
    }

    struct sockaddr_un uds_addr;
    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;
    strncpy(uds_addr.sun_path, UDS_PATH, sizeof(uds_addr.sun_path) - 1);

    unlink(UDS_PATH);
    if (bind(uds_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) < 0) {
        perror("[Worker] bind UDS");
        return EXIT_FAILURE;
    }
    if (listen(uds_fd, 5) < 0) {
        perror("[Worker] listen UDS");
        return EXIT_FAILURE;
    }

    printf("[Worker] Prêt et en attente de sessions migrées sur %s...\n", UDS_PATH);

    while (1) {
        int uds_conn = accept(uds_fd, NULL, NULL);
        if (uds_conn < 0) {
            perror("[Worker] accept UDS");
            continue;
        }

        int client_fd = -1;
        migration_msg_t msg;
        memset(&msg, 0, sizeof(msg));

        if (receive_handoff(uds_conn, &client_fd, &msg) != 0) {
            fprintf(stderr, "[Worker] Échec réception handoff\n");
            close(uds_conn);
            continue;
        }

        printf("\n[Worker] ✅ Connexion reçue ! FD=%d, Fonction='%s', Blob=%u octets\n",
               client_fd, msg.function_name, msg.blob_sz);

        /* Affichage de la requête HTTP pré-lue par le gateway */
        if (msg.request_len > 0) {
            printf("[Worker] 📨 Requête HTTP (%d octets) :\n%.200s\n",
                   msg.request_len, msg.http_request);
        } else {
            printf("[Worker] ⚠️  Aucune requête HTTP dans le message.\n");
        }

        /* Création de l'objet SSL et association au FD reçu */
        WOLFSSL* ssl = wolfSSL_new(ctx);
        if (!ssl) {
            fprintf(stderr, "[Worker] Erreur création objet SSL\n");
            close(client_fd);
            close(uds_conn);
            continue;
        }
        wolfSSL_set_fd(ssl, client_fd);

        /* Import de la session TLS migrée */
        int ret = wolfSSL_tls_import(ssl, msg.tls_blob, msg.blob_sz);

        /* CRITIQUE : wolfSSL_tls_import désérialise l'objet SSL complet du gateway,
         * y compris son FD interne (FD=4 côté gateway). Dans ce processus, FD=4
         * est la connexion UDS — PAS le socket client. On corrige le FD ici. */
        wolfSSL_set_fd(ssl, client_fd);

        if (ret <= 0) {
            int err = wolfSSL_get_error(ssl, ret);
            char errStr[80];
            wolfSSL_ERR_error_string(err, errStr);
            fprintf(stderr, "[Worker] ❌ Échec Import TLS (ret=%d, err=%d) : %s\n",
                    ret, err, errStr);
            wolfSSL_free(ssl);
            close(client_fd);
            close(uds_conn);
            continue;
        }

        printf("[Worker] ✅ Import TLS réussi (%d octets consommés). FD corrigé à %d.\n", ret, client_fd);
        fflush(stdout);

        /* Construction et envoi de la réponse HTTP sur la session migrée.
         * On utilise directement wolfSSL_write : la requête a déjà été lue
         * par le gateway et nous est transmise dans msg.http_request. */
        const char* body    = "Hello from the Worker!";
        int         bodyLen = (int)strlen(body);
        char        response[512];
        int         respLen = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            bodyLen, body);

        printf("[Worker] 🔄 Envoi de la réponse sur FD=%d...\n", client_fd);
        fflush(stdout);
        int write_ret = wolfSSL_write(ssl, response, respLen);
        fflush(stdout);
        if (write_ret > 0) {
            printf("[Worker] ✅ Réponse HTTPS envoyée au client (%d octets).\n", write_ret);
            fflush(stdout);
        } else {
            int err = wolfSSL_get_error(ssl, write_ret);
            char errStr[80];
            wolfSSL_ERR_error_string(err, errStr);
            fprintf(stderr, "[Worker] ❌ Échec wolfSSL_write : %s (code=%d)\n", errStr, err);
        }

        /* Fermeture propre de la session TLS : envoi du TLS close_notify */
        wolfSSL_shutdown(ssl);

        wolfSSL_free(ssl);
        close(client_fd);
        close(uds_conn);
    }

    wolfSSL_CTX_free(ctx);
    wolfSSL_Cleanup();
    return 0;
}