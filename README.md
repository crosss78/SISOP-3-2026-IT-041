# SISOP-3-2026-IT-041

| Nama                   | NRP        |
| ---------------------- | ---------- |
| Muhamad Sabilil Haq    | 5027251041 |

<details>
<summary>Daftar Isi</summary>

- [Soal 1](#soal-1)
- [Soal 2](#soal-2)

</details>


# Soal 1: Present Day, Present Time

## Penjelasan Umum

Pada soal ini, diminta untuk membangun sebuah sistem komunikasi berbasis jaringan yang disebut sebagai **The Wired**, yang terdiri dari dua komponen utama, yaitu **server** dan **client**. Sistem ini memungkinkan beberapa user untuk saling terhubung dan berkomunikasi secara real-time dalam satu jaringan yang sama.
Implementasi sistem ini dibagi ke dalam tiga file utama, yaitu:
-   `protocol.h` → sebagai **shared configuration**
-   `wired.c` → sebagai **server**
-   `navi.c` → sebagai **client**

**Arsitektur Sistem**

Sistem ini menggunakan arsitektur **client-server**, di mana:

-   **Protocol (`protocol.h`)**
    -   Berisi konfigurasi yang digunakan bersama oleh server dan client
-   **Server (`wired.c`)**
    -   Bertugas menerima koneksi dari client
    -   Mengelola daftar user yang sedang aktif
    -   Mendistribusikan pesan (broadcast) ke seluruh client
    -   Menyediakan fitur khusus untuk admin (The Knights)
    -   Menyimpan seluruh aktivitas ke dalam file `history.log`
-   **Client (`navi.c`)**
    -   Digunakan oleh user untuk terhubung ke server
    -   Mengirim dan menerima pesan secara asynchronous menggunakan thread
    -   Mendukung dua mode:
        -   **User biasa** → untuk chat
        -   **Admin (The Knights)** → untuk akses console khusus
  ---

## File ``protocol.h``

File `protocol.h` berfungsi sebagai **shared configuration** yang digunakan oleh server (`wired.c`) dan client (`navi.c`). Tujuan utama dari file ini adalah untuk memastikan bahwa kedua komponen menggunakan konfigurasi yang sama, sehingga komunikasi dapat berjalan dengan lancar.
Isi dari filenya adalah sebagai berikut:
```
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
```
Di dalamnya meliputi:

 1. Konstanta yang Digunakan

    `PORT 8080`: Menentukan port yang digunakan oleh server untuk menerima koneksi dari client.

    `BUFFER_SIZE 1024`: Menentukan ukuran buffer untuk pengiriman dan penerimaan data antar client dan server.

    `MAX_CLIENTS 100`: Menentukan jumlah maksimum client yang dapat terhubung secara bersamaan ke server.

    `NAME_LEN 50`: Menentukan panjang maksimum username yang dapat digunakan oleh client.

 2. Struktur `Client`

    Struct `Client` digunakan untuk merepresentasikan setiap user yang terhubung ke server. Struktur ini memiliki beberapa atribut:

    - `int socket`: Menyimpan file descriptor dari socket client yang digunakan untuk komunikasi.
    - `char name[NAME_LEN]`: Menyimpan nama user yang digunakan sebagai identitas unik dalam sistem.
    - `int is_admin`: Menandakan apakah client tersebut memiliki hak akses sebagai admin (`The Knights`) atau tidak.
---

## File `wired.c`

File `wired.c` merupakan  **server utama** dalam sistem _The Wired_. Server ini bertanggung jawab untuk menangani koneksi dari banyak client secara bersamaan, mengelola komunikasi antar client, serta menyediakan fitur khusus untuk admin. File ini terbagi menjadi beberapa bagian, yaitu sebagai berikut.

 **1. Liblary liblary yang digunakan**

Meliputi liblary-liblary yang dibutukhan dapat memakai fungsi yg sudah tersedia, contohnya `pthread.h` agar bisa melakukan `multithreading`
```
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

#include "protocol.h"
```
 **2. Inisialisasi Variabel Global**

 Pendeklarasian variabel global ini ditujukan agar data bisa diakses dan dibagikan antar thread/fungsi.
 ```
Client clients[MAX_CLIENTS]; #menyimpan seluruh client yang terhubung
int client_count = 0; #Menghitung jumlah client aktif
pthread_mutex_t lock; #mutex untuk menghindari race condition
time_t server_start; #waktu server mulai (untuk uptime)
int server_fd; #socket utama server
```
 **3. Logging System**

Fungsi ini digunakan untuk mencatat seluruh aktivitas ke dalam file `history.log` dengan format: `[YYYY-MM-DD HH:MM:SS] [System/Admin/User] [Status/Command/Chat]`
```
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
```
 **4. Graceful Shutdown (SIGINT)**

Fungsi ini akan dijalankan ketika server menerima sinyal `interrupt`, seperti `Ctrl + C`.
```
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
```
Jika fungsi ini terpanggil, secara umum alurnya seperti ini:
menampilkan notifikasi shutdown → mencatat ke log → mengunci mutex → menutup seluruh koneksi client → membuka mutex → menutup socket server → menghentikan program.

 **5. Utility Function**
 - **Cek Username**

   Digunakan untuk mengecek username agar tiap username berbeda (unique)
   ```
   int is_name_taken(char *name) {
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].name, name) == 0)
            return 1;
    }
    return 0;
    }
   ```
 - **Broadcast Message**

   Fungsi ini aka mengirim pesan ke semua client, kecuali client pengirim. Menggunakan mutex agar aman saat multi-thread.
   ```
   void broadcast(char *msg, int sender) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket != sender) {
            send(clients[i].socket, msg, strlen(msg), 0);
        }
    }
    pthread_mutex_unlock(&lock);
    }
   ```
 - **Remove Client**

   Fungsi ini digunakan untuk menghapus client dari list ketika `disconnect` dan menjaga konsistensi `client_count`
   ```
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
   ```
 **5. Admin System (The Knights)**
 - **Menu Admin**
   ```
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
   ```
   Menampilkan menu admin:
   ```
   1. Check Active Entities (Users)
   2. Check Server Uptime
   3. Execute Emergency Shutdown
   4. Disconnect
   ```
 - **Handle Admin**

   Fungsi ini menangani seluruh command admin:

     `1 → Cek user aktif`: menghitung jumlah client non-admin dan mengirim hasil ke admin

     `2 → Cek uptime`: menghitung waktu sejak server start

     `3 → Shutdown`: menulis log dan menghentikan server

     `4 → Disconnect`: keluar dari admin mode
   ```
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
   ```
 **7. Handle Client**

 Ini adalah bagian paling penting dalam server
 - **Login Phase**
   - Client diminta memasukkan nama:
     ```
     Enter your name:
     ```
   - Jika nama adalah **"The Knights"**:
     - Masuk ke proses autentikasi password
     - Jika salah → ulangi
     - Jika `/exit` → keluar
     - Jika benar → masuk admin mode
   - Jika user biasa:
     - Dicek apakah username sudah digunakan
     - Jika ya → diminta ulang
     - Jika tidak → lanjut
 - **Register Client**
   ```
   clients[client_count]
   ```
   - Menyimpan socket
   - Menyimpan nama
   - Menandai apakah admin
   - Menambah jumlah client
   - Mengirim log `<user>` connected
   - Menampilkan `<user> Connected to The Wired`
 - **Mode Admin**

    jika user adalah admin
 - **Mode Chat**

   untuk user biasa
   - Menerima pesan dari client
   - Jika `/exit` → keluar
   - Jika pesan biasa:
     - Format: ``[username]: pesan``
     - Dicatat ke log
     - Dikirim ke semua client lain (broadcast)
 - **Disconnect**

   Saat client keluar:
   - Log disconnect
   - Hapus dari list client
   - Tutup socket
```
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
```
 **8. Main Server**

 - **Setup Server**
   - Inisialisasi socket
   - Set `SO_REUSEADDR`
   - Bind ke port (`8080`)
   - Listen koneksi
 - **Start Server**
   ```
   log_event("System", "SERVER ONLINE");
   ```
 - **Accept Client**
   ```
   accept()
   ```
   - Setiap client:
     - Dialokasikan memory
     - Dibuat thread baru (`pthread`)
     - Dijalankan `handle_client`

```
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
```
---
## File `wired.c`

File `wired.c` merupakan  **server utama** dalam sistem _The Wired_. Server ini bertanggung jawab untuk menangani koneksi dari banyak client secara bersamaan, mengelola komunikasi antar client, serta menyediakan fitur khusus untuk admin. File ini terbagi menjadi beberapa bagian, yaitu sebagai berikut.

 **1. Liblary liblary yang digunakan**

Meliputi liblary-liblary yang dibutukhan dapat memakai fungsi yg sudah tersedia, contohnya `pthread.h` agar bisa melakukan `multithreading`.
```
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#include "protocol.h"

int sock;
```
 **2. Inisialisasi Variabel Global**

 Variabel ini digunakan untuk menyimpan **socket descriptor** yang menghubungkan client dengan server.
 ```
 int sock;
 ```
 **3. Handle Interrupt (SIGINT)**

Fungsi ini akan dijalankan ketika server menerima sinyal `interrupt`, seperti `Ctrl + C`.
```
void handle_sigint(int sig) {
        printf("\n[System] Disconnecting from The Wired...[press ENTER]\n");
            fflush(stdout);

                close(sock);
                    exit(0);
}
```
Jika fungsi ini terpanggil, secara umum alurnya seperti ini: Menampilkan pesan disconnect → Menutup koneksi socket → Menghentikan program

 **4. Thread Pengirim Pesan**

Fungsi ini berjalan pada thread terpisah dan bertugas untuk:
 - Menampilkan prompt `>`
 - Mengambil input dari user (`fgets`)
 - Menghapus newline agar format rapi
 - Mengirim pesan ke server
Sedangkan, Jika user mengetik `/exit`: Menampilkan notifikasi disconnect → Menutup socket → Menghentikan program
```
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
```
 **4. Thread Pengirim Pesan**

Fungsi ini juga berjalan pada thread terpisah dan bertugas untuk:
 - Menerima pesan dari server
 - Menampilkan pesan ke terminal
 - Menampilkan kembali prompt `>`
Namun, JJika koneksi terputus: Menampilkan notifikasi → Menutup socket → Menghentikan program
```
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
```
 **8. Main Server**

 - **Login Phase**
   Pada bagian ini, client akan melakukan komunikasi awal dengan server:
   ```
   while (1) {
    recv(...)
   }
   ```
   - Alurnya: Menerima prompt dari server (misalnya: `Enter your name:`) → Mengirim input user ke server → Menunggu respon dari server
 - **Deteksi Mode**
   ```
   if (strstr(buffer, "Authentication Successful"))
   ```
   Client akan menentukan mode berdasarkan respon server:
    - Jika **Authentication Successful** → masuk mode admin
    - Jika **Connected to The Wired** → masuk mode user biasa
 - **Mode User (Chat)**
   ```
   pthread_create(...)
   ```
   Jika user biasa:
   - Membuat 2 thread:
     - Thread kirim (`send_msg`)
     - Thread terima (`recv_msg`)
   - Kedua thread berjalan bersamaan (asynchronous)
   Hal ini memungkinkan `User` tetap bisa mengetik Sambil menerima pesan dari user lain
 - **Mode Admin**
   ```
   while (1) {
    recv(...)
    }
   ```
   Jika admin:
   - Tidak menggunakan thread
   - Menggunakan loop biasa (blocking)
   Alurnya: Menerima menu dari server → Menampilkan ke terminal → Mengirim input command ke server
```
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
```
---
## Dokumentasi

![alt text](assets/soal_1/1.png)

![alt text](assets/soal_1/2.png)

![alt text](assets/soal_1/3.png)

![alt text](assets/soal_1/4.png)

![alt text](assets/soal_1/5.png)

---
