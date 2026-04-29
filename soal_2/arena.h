#ifndef ARENA_H
#define ARENA_H

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define USER_DB_FILE "users.db"
#define HISTORY_FILE "history.db"

#define SHM_KEY 0x1234
#define MQ_KEY  0x5678
#define SEM_KEY 0x9012

#define MAX_USERNAME 32
#define MAX_PASSWORD 64
#define MAX_WEAPON   32
#define MAX_TEXT     1024
#define MAX_LOGS        5
#define MAX_USERS     256
#define MAX_HISTORY   256
#define MAX_BATTLES 16

/* Commands */
enum {
    CMD_REGISTER = 1,
    CMD_LOGIN,
    CMD_LOGOUT,
    CMD_BATTLE_REQ,
    CMD_ATTACK,
    CMD_BUY_WEAPON,
    CMD_HISTORY,
    CMD_EXIT,
    CMD_PING
};

/* Responses */
enum {
    RESP_OK = 1,
    RESP_ERR,
    RESP_INFO,
    RESP_WAITING,
    RESP_BATTLE_START,
    RESP_BATTLE_UPDATE,
    RESP_BATTLE_END
};

typedef struct {
    char name[MAX_WEAPON];
    int cost;
    int bonus;
} weapon_item_t;

static const weapon_item_t WEAPON_SHOP[] = {
    {"Wood Sword", 100, 5},
    {"Iron Sword", 300, 15},
    {"Steel Axe", 600, 30},
    {"Demon Blade", 1500, 60},
    {"God Slayer", 5000, 150},
};

#define WEAPON_SHOP_COUNT ((int)(sizeof(WEAPON_SHOP) / sizeof(WEAPON_SHOP[0])))

typedef struct {
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    int gold;
    int level;
    int xp;
    char weapon_name[MAX_WEAPON];
    int weapon_bonus;
    pid_t logged_in_pid;
} user_record_t;

typedef struct {
    int active;
    int mode; /* 1 = vs bot, 2 = human vs human */
    char p1_name[MAX_USERNAME];
    char p2_name[MAX_USERNAME];
    pid_t p1_pid;
    pid_t p2_pid;

    user_record_t p1_user;
    user_record_t p2_user;

    int p1_hp;
    int p2_hp;
    time_t p1_last_attack;
    time_t p2_last_attack;

    char logs[MAX_LOGS][MAX_TEXT];
    int log_count;
    char result[MAX_TEXT];
} battle_state_t;

typedef struct {
    int initialized;

    int waiting_active;
    char waiting_user[MAX_USERNAME];
    pid_t waiting_pid;
    time_t waiting_since;
    user_record_t waiting_user_record;

    int battle_active;
    battle_state_t battles[MAX_BATTLES];
} arena_state_t;

typedef struct {
    long mtype; /* destination pid */
    int cmd;
    int status;
    pid_t pid;
    int value;
    int value2;
    int gold;
    int level;
    int xp;
    int weapon_bonus;
    char username[MAX_USERNAME];
    char password[MAX_PASSWORD];
    char opponent[MAX_USERNAME];
    char weapon_name[MAX_WEAPON];
    char payload[MAX_TEXT];
} ipc_msg_t;

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

static inline void trim_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static inline void safe_copy(char *dst, const char *src, size_t dstsz) {
    if (!dst || dstsz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dstsz, "%s", src);
}

static inline int pid_alive(pid_t pid) {
    if (pid <= 0) return 0;
    if (kill(pid, 0) == 0) return 1;
    return errno == EPERM;
}

static inline int level_from_xp(int xp) {
    if (xp < 0) xp = 0;
    return 1 + (xp / 100);
}

static inline int calc_damage(const user_record_t *u) {
    if (!u) return 10;
    return 10 + (u->xp / 50) + u->weapon_bonus;
}

static inline int calc_health(const user_record_t *u) {
    if (!u) return 100;
    return 100 + (u->xp / 10);
}

static inline void push_log(char logs[MAX_LOGS][MAX_TEXT], int *log_count, const char *fmt, ...) {
    if (!logs || !log_count || !fmt) return;
    for (int i = MAX_LOGS - 1; i > 0; --i) {
        snprintf(logs[i], MAX_TEXT, "%s", logs[i - 1]);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logs[0], MAX_TEXT, fmt, ap);
    va_end(ap);
    if (*log_count < MAX_LOGS) (*log_count)++;
}

static inline void format_history_time(char *out, size_t outsz, time_t t) {
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, outsz, "[%d/%m/%Y-%H:%M]", &tmv);
}

#endif /* ARENA_H */