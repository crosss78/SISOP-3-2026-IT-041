#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "protocol.h"

Client clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t lock;
time_t server_start;
int server_fd;

// ===== LOGGING =====
void log_event(const char *role, const char *msg) {
    FILE *f = fopen("history.log", "a");

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s]\n",
        t->tm_year+1900, t->tm_mon+1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec,
        role, msg);

    fclose(f);
}

void handle_sigint(int sig) {
    printf("\n[System] Server shutting down...\n");

    log_event("System", "SERVER SHUTDOWN");

    // close semua client
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        close(clients[i].socket);
    }
    pthread_mutex_unlock(&lock);

    close(server_fd);
    exit(0);
}

// ===== UTIL =====
int is_name_taken(char *name) {
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].name, name) == 0)
            return 1;
    }
    return 0;
}

void broadcast(char *msg, int sender) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket != sender) {
            send(clients[i].socket, msg, strlen(msg), 0);
        }
    }
    pthread_mutex_unlock(&lock);
}

void remove_client(int sock) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket == sock) {
            clients[i] = clients[client_count - 1];
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

// ===== ADMIN MENU =====
void admin_menu(int sock) {
    char menu[] =
        "=== THE KNIGHTS CONSOLE ===\n"
        "1. Check Active Entities (Users)\n"
        "2. Check Server Uptime\n"
        "3. Execute Emergency Shutdown\n"
        "4. Disconnect\n"
        "Command >> ";

    send(sock, menu, strlen(menu), 0);
}

void handle_admin(int sock) {
    char buffer[BUFFER_SIZE];

    while (1) {
        admin_menu(sock);
        int len = recv(sock, buffer, BUFFER_SIZE, 0);
        if (len <= 0) break;

        buffer[len] = '\0';
        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, "1") == 0) {
            int count = 0;

            pthread_mutex_lock(&lock);
            for (int i = 0; i < client_count; i++) {
                if (!clients[i].is_admin) {
                    count++;
                }
            }
            pthread_mutex_unlock(&lock);

            char msg[100];
            snprintf(msg, sizeof(msg), "Active Users: %d\n", count);

            send(sock, msg, strlen(msg), 0);
            log_event("Admin", "RPC_GET_USERS");
        } else if (strcmp(buffer, "2") == 0) {
            time_t now = time(NULL);
            int uptime = (int)(now - server_start);

            char msg[100];
            snprintf(msg, sizeof(msg), "Server Uptime: %d seconds\n", uptime);

            send(sock, msg, strlen(msg), 0);
            log_event("Admin", "RPC_GET_UPTIME");

        } else if (strcmp(buffer, "3") == 0) {
            log_event("Admin", "RPC_SHUTDOWN");
            log_event("System", "EMERGENCY SHUTDOWN INITIATED");

            printf("[System] Server shutting down...\n");
            exit(0);

        } else if (strcmp(buffer, "4") == 0) {
            log_event("System", "The Knights disconnected");

            close(sock);
            pthread_exit(NULL);
        }
    }
}

// ===== CLIENT HANDLER =====
void *handle_client(void *arg) {
    int sock = *(int *)arg;
    free(arg);

    char name[NAME_LEN];
    char buffer[BUFFER_SIZE];

    // ===== LOGIN =====
    while (1) {
        send(sock, "Enter your name: ", strlen("Enter your name: "), 0);

        int len = recv(sock, name, NAME_LEN, 0);
        if (len <= 0) {
            close(sock);
            pthread_exit(NULL);
        }

        name[len] = '\0';
        name[strcspn(name, "\n")] = 0;

        // ===== ADMIN FLOW =====
        if (strcmp(name, "The Knights") == 0) {
            char password[BUFFER_SIZE];

            while (1) {
                send(sock, "Enter Password or (/exit): ", strlen("Enter Password or (/exit): "), 0);

                int plen = recv(sock, password, BUFFER_SIZE, 0);
                if (plen <= 0) {
                    close(sock);
                    pthread_exit(NULL);
                }

                password[plen] = '\0';
                password[strcspn(password, "\n")] = 0;

                if (strcmp(password, "/exit") == 0) {
                    close(sock);
                    pthread_exit(NULL);
                }

                if (strcmp(password, "admin") == 0) {
                    send(sock, "[System] Authentication Successful. Granted Admin privileges.\n", 66, 0);
                    break;
                } else {
                    send(sock, "[System] Wrong password!\n", 25, 0);
                }
            }
            break;
        }

        // ===== USER NORMAL =====
        pthread_mutex_lock(&lock);
        int taken = is_name_taken(name);
        pthread_mutex_unlock(&lock);

        if (taken) {
            char msg[BUFFER_SIZE];
            snprintf(msg, sizeof(msg), "[System] username %s telah digunakan, masukkan username lain\n", name);
            send(sock, msg, strlen(msg), 0);
        } else {
            break;
        }
    }

    // ===== REGISTER CLIENT =====
    pthread_mutex_lock(&lock);
    clients[client_count].socket = sock;
    strcpy(clients[client_count].name, name);
    clients[client_count].is_admin = (strcmp(name, "The Knights") == 0);
    client_count++;
    pthread_mutex_unlock(&lock);

    char logmsg[BUFFER_SIZE];
    snprintf(logmsg, sizeof(logmsg), "User '%s' connected", name);
    log_event("System", logmsg);

    if (!clients[client_count - 1].is_admin) {
        char welcome[BUFFER_SIZE];
        snprintf(welcome, sizeof(welcome), "%s Connected to The Wired\n", name);
        send(sock, welcome, strlen(welcome), 0);
    }

    // ===== ADMIN MODE =====
    if (clients[client_count - 1].is_admin) {
        handle_admin(sock);
    }

    // ===== CHAT =====
    while (1) {
        int len = recv(sock, buffer, BUFFER_SIZE, 0);
        if (len <= 0) break;

        buffer[len] = '\0';
        buffer[strcspn(buffer, "\n")] = 0;

        if (strncmp(buffer, "/exit", 5) == 0)
            break;

        char msg[BUFFER_SIZE];
        snprintf(msg, sizeof(msg), "[%s]: %.900s", name, buffer);

        log_event("User", msg);
        broadcast(msg, sock);
    }

    snprintf(logmsg, sizeof(logmsg), "User '%s' disconnected", name);
    log_event("System", logmsg);

    remove_client(sock);
    close(sock);
    pthread_exit(NULL);
}

int main() {
    struct sockaddr_in server_addr;

    signal(SIGINT, handle_sigint);

    server_start = time(NULL);
    pthread_mutex_init(&lock, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_fd, 10);

    log_event("System", "SERVER ONLINE");
    printf("[System] Server listening...\n");

    while (1) {
        int *client_sock = malloc(sizeof(int));
        *client_sock = accept(server_fd, NULL, NULL);

        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, client_sock);
        pthread_detach(tid);
    }

    return 0;
}