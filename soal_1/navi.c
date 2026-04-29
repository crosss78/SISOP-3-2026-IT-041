#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#include "protocol.h"

int sock;

void handle_sigint(int sig) {
        printf("\n[System] Disconnecting from The Wired...[press ENTER]\n");
            fflush(stdout);

                close(sock);
                    exit(0);
}


void *send_msg(void *arg) {
    char msg[BUFFER_SIZE];

    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(msg, BUFFER_SIZE, stdin) == NULL)
            continue;

        // hapus newline biar rapi
        msg[strcspn(msg, "\n")] = 0;

        // kirim dengan newline manual
        char sendbuf[BUFFER_SIZE];
        snprintf(sendbuf, sizeof(sendbuf), "%.*s\n", BUFFER_SIZE - 2, msg);

        if (send(sock, sendbuf, strlen(sendbuf), 0) <= 0) {
            exit(0);
        }

        if (strcmp(msg, "/exit") == 0) {
            printf("[System] Disconnecting from The Wired...[press ENTER]\n");
            fflush(stdout);

            close(sock);
            exit(0);
        }
    }
}

void *recv_msg(void *arg) {
    char msg[BUFFER_SIZE];

    while (1) {
        int len = recv(sock, msg, BUFFER_SIZE, 0);

        if (len <= 0) {
            printf("\n[System] Disconnected from server. Tekan [enter] untuk keluar...");
            close(sock);
            exit(0);   
        }

        msg[len] = '\0';

        // pindah ke baris baru biar ga ganggu input
        printf("\n%s\n", msg);

        // tampilkan lagi prompt
        printf("> ");
        fflush(stdout);
    }
}

int main() {
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char input[BUFFER_SIZE];
    int len;
    int is_admin = 0;

    signal(SIGINT, handle_sigint);

    sock = socket(AF_INET, SOCK_STREAM, 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 0;
    }

    // ===== LOGIN PHASE =====
    while (1) {
        len = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (len == 0) {
            printf("\n[System] Disconnected from server\n");
            close(sock);
            return 0;
        }

        if (len < 0) {
            perror("recv");
            close(sock);
            return 0;
        }

        buffer[len] = '\0';
        printf("%s", buffer);
        fflush(stdout);

        // server minta input
        if (strstr(buffer, "Enter your name:") != NULL ||
            strstr(buffer, "Enter Password or (/exit):") != NULL) {

            if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
                close(sock);
                return 0;
            }

            send(sock, input, strlen(input), 0);
        }

        // ===== DETEKSI MODE =====
        if (strstr(buffer, "Authentication Successful") != NULL) {
            is_admin = 1;
            break;
        }

        if (strstr(buffer, "Connected to The Wired") != NULL) {
            is_admin = 0;
            break;
        }
    }

    // ===== MODE USER (CHAT) =====
    if (!is_admin) {
        pthread_t t1, t2;

        pthread_create(&t1, NULL, send_msg, NULL);
        pthread_create(&t2, NULL, recv_msg, NULL);

        pthread_join(t1, NULL);
        pthread_join(t2, NULL);
    }

    // ===== MODE ADMIN =====
    else {
        while (1) {
            int len = recv(sock, buffer, BUFFER_SIZE, 0);

            if (len <= 0) {
                printf("\n[System] Disconnected from server\n");
                break;
            }

            buffer[len] = '\0';
            printf("%s", buffer);
            fflush(stdout);

            if (strstr(buffer, "Command >>") != NULL) {
                if (fgets(input, BUFFER_SIZE, stdin) == NULL) {
                    break;
                }
                send(sock, input, strlen(input), 0);
            }
        }
    }

    close(sock);
    return 0;
}