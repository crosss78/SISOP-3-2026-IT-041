#ifndef PROTOCOL_H
#define PROTOCOL_H

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100
#define NAME_LEN 50

typedef struct {
    int socket;
    char name[NAME_LEN];
    int is_admin;
} Client;

#endif