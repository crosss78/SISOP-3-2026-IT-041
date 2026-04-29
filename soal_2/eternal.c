#include "arena.h"
#include <pthread.h>
#include <sys/select.h>
#include <termios.h>

static int g_shmid = -1;
static int g_mqid = -1;
static int g_semid = -1;
static arena_state_t *g_state = NULL;
static user_record_t g_user;
static int g_logged_in = 0;
static struct termios g_orig_termios;
static char g_password[MAX_PASSWORD];

static void sem_lock(void) {
    struct sembuf op = {0, -1, 0};
    if (semop(g_semid, &op, 1) == -1) {
        perror("semop lock");
        exit(EXIT_FAILURE);
    }
}

static void sem_unlock(void) {
    struct sembuf op = {0, 1, 0};
    if (semop(g_semid, &op, 1) == -1) {
        perror("semop unlock");
        exit(EXIT_FAILURE);
    }
}

static int recv_reply_block(ipc_msg_t *out) {
    memset(out, 0, sizeof(*out));
    ssize_t r = msgrcv(g_mqid, out, sizeof(*out) - sizeof(long), (long)getpid(), 0);
    if (r == -1) return -1;
    return 0;
}

static int recv_reply_nowait(ipc_msg_t *out) {
    memset(out, 0, sizeof(*out));
    ssize_t r = msgrcv(g_mqid, out, sizeof(*out) - sizeof(long), (long)getpid(), IPC_NOWAIT);
    if (r == -1) return -1;
    return 0;
}

static void send_request(int cmd, const char *username, const char *password, int value) {
    ipc_msg_t req;
    memset(&req, 0, sizeof(req));
    req.mtype = 1;
    req.cmd = cmd;
    req.pid = getpid();
    if (username) safe_copy(req.username, username, sizeof(req.username));
    if (password) safe_copy(req.password, password, sizeof(req.password));
    req.value = value;
    if (msgsnd(g_mqid, &req, sizeof(req) - sizeof(long), 0) == -1) {
        perror("msgsnd");
        exit(EXIT_FAILURE);
    }
}

static void load_user_from_msg(const ipc_msg_t *msg) {
    if (!msg) return;
    safe_copy(g_user.username, msg->username, sizeof(g_user.username));
    safe_copy(g_user.weapon_name, msg->weapon_name, sizeof(g_user.weapon_name));
    g_user.gold = msg->gold;
    g_user.level = msg->level;
    g_user.xp = msg->xp;
    g_user.weapon_bonus = msg->weapon_bonus;
}

static void banner(void) {
    printf("__________         __    __  .__                   _____ \n");
    printf("\\______   \\_____ _/  |__/  |_|  |   ____     _____/ ____\\\n");
    printf(" |    |  _/\\__  \\\\   __\\   __\\  | _/ __ \\   /  _ \\   __\\ \n");
    printf(" |    |   \\ / __ \\|  |  |  | |  |_\\  ___/  (  <_> )  |   \n");
    printf(" |______  /(____  /__|  |__| |____/\\___  >  \\____/|__|   \n");
    printf("        \\/      \\/                     \\/                \n");
    printf("___________ __               .__                         \n");
    printf("\\_   _____//  |_  ___________|__| ____   ____            \n");
    printf(" |    __)_\\   __\\/ __ \\_  __ \\  |/  _ \\ /    \\           \n");
    printf(" |        \\|  | \\  ___/|  | \\/  (  <_> )   |  \\          \n");
    printf("/_______  /|__|  \\___  >__|  |__|\\____/|___|  /          \n");
    printf("        \\/           \\/                     \\/           \n");
}

static void print_user_banner(void) {
    printf("\n=== PROFILE ===\n");
    printf("User   : %s\n", g_user.username[0] ? g_user.username : "-");
    printf("Level  : %d\n", g_user.level);
    printf("Gold   : %d\n", g_user.gold);
    printf("XP     : %d\n", g_user.xp);
    printf("Weapon : %s (+%d)\n", g_user.weapon_name[0] ? g_user.weapon_name : "None", g_user.weapon_bonus);
}

static void read_line(const char *prompt, char *buf, size_t sz) {
    if (prompt) printf("%s", prompt);
    if (!fgets(buf, (int)sz, stdin)) {
        buf[0] = '\0';
        return;
    }
    trim_newline(buf);
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return;
    struct termios raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

static int kbhit(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int read_key(void) {
    char c = '\0';
    if (read(STDIN_FILENO, &c, 1) == 1) return (unsigned char)c;
    return -1;
}

static int get_my_battle_snapshot(battle_state_t *out) {
    if (!out) return -1;

    sem_lock();
    for (int i = 0; i < MAX_BATTLES; ++i) {
        const battle_state_t *b = &g_state->battles[i];
        if (b->active &&
            (strcmp(g_user.username, b->p1_name) == 0 ||
             strcmp(g_user.username, b->p2_name) == 0)) {
            *out = *b;
            sem_unlock();
            return 0;
        }
    }
    sem_unlock();
    return -1;
}

static void draw_battle_screen(const battle_state_t *b) {
    if (!b) return;

    int my_side = 0;
    if (strcmp(g_user.username, b->p1_name) == 0) my_side = 1;
    else if (strcmp(g_user.username, b->p2_name) == 0) my_side = 2;
    if (my_side == 0) return;

    const user_record_t *me = (my_side == 1) ? &b->p1_user : &b->p2_user;
    const user_record_t *enemy = (my_side == 1) ? &b->p2_user : &b->p1_user;
    int my_hp = (my_side == 1) ? b->p1_hp : b->p2_hp;
    int enemy_hp = (my_side == 1) ? b->p2_hp : b->p1_hp;

    printf("\033[2J\033[H");
    printf("=== BATTLE OF ETERION ===\n");
    printf("You   : %s | Lv %d | HP %d | Dmg %d | Weapon %s (+%d)\n",
           me->username, me->level, my_hp, calc_damage(me),
           me->weapon_name[0] ? me->weapon_name : "None", me->weapon_bonus);
    printf("Enemy : %s | Lv %d | HP %d | Dmg %d | Weapon %s (+%d)\n",
           enemy->username, enemy->level, enemy_hp, calc_damage(enemy),
           enemy->weapon_name[0] ? enemy->weapon_name : "None", enemy->weapon_bonus);
    printf("\nControls: [a] attack  [u] ultimate\n");
    printf("\nLogs:\n");
    for (int i = 0; i < MAX_LOGS; ++i) {
        if (b->logs[i][0]) printf("%d. %s\n", i + 1, b->logs[i]);
    }
    fflush(stdout);
}

static void wait_for_battle_start(void) {
    ipc_msg_t resp;
    while (1) {
        if (recv_reply_block(&resp) == -1) return;
        if (resp.status == RESP_BATTLE_START) {
            if (resp.payload[0]) printf("%s\n", resp.payload);
            break;
        }
        if (resp.status == RESP_ERR) {
            printf("%s\n", resp.payload);
            break;
        }
        if (resp.status == RESP_WAITING) {
            printf("%s\n", resp.payload);
        }
    }
}

static void battle_loop(void) {
    enable_raw_mode();

    while (1) {
        ipc_msg_t resp;
        while (recv_reply_nowait(&resp) == 0) {
            if (resp.status == RESP_BATTLE_UPDATE) {
                if (resp.payload[0]) printf("\n%s\n", resp.payload);
            }

            if (resp.status == RESP_BATTLE_END) {
                if (resp.username[0]) load_user_from_msg(&resp);
                printf("\n%s\n", resp.payload);
                goto battle_end;
            }
        }

        battle_state_t b;
        if (get_my_battle_snapshot(&b) == 0) {
            draw_battle_screen(&b);
        }

        if (kbhit()) {
            int c = read_key();
            if (c == 'a' || c == 'u') {
                send_request(CMD_ATTACK, g_user.username, NULL, (c == 'u') ? 2 : 1);

                if (recv_reply_block(&resp) == 0) {
                    if (resp.status == RESP_ERR || resp.status == RESP_BATTLE_UPDATE) {
                        if (resp.payload[0]) printf("\n%s\n", resp.payload);
                    }

                    if (resp.status == RESP_BATTLE_END) {
                        if (resp.username[0]) load_user_from_msg(&resp);
                        printf("\n%s\n", resp.payload);
                        goto battle_end;
                    }
                }
            }
        }

        struct timespec ts = {0, 120 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

battle_end:
    disable_raw_mode();

    printf("\nBattle ended. Press ENTER to continue...");
    fflush(stdout);
    char tmp[8];
    fgets(tmp, sizeof(tmp), stdin);
}

static void register_flow(void) {
    char user[MAX_USERNAME], pass[MAX_PASSWORD];
    read_line("Username: ", user, sizeof(user));
    read_line("Password: ", pass, sizeof(pass));
    send_request(CMD_REGISTER, user, pass, 0);

    ipc_msg_t resp;
    if (recv_reply_block(&resp) == 0) {
        printf("%s\n", resp.payload);
        if (resp.status == RESP_OK) {
            load_user_from_msg(&resp);
        }
    }
}

static int login_flow(void) {
    char user[MAX_USERNAME], pass[MAX_PASSWORD];
    read_line("Username: ", user, sizeof(user));
    read_line("Password: ", pass, sizeof(pass));
    send_request(CMD_LOGIN, user, pass, 0);

    ipc_msg_t resp;
    if (recv_reply_block(&resp) == 0) {
        printf("%s\n", resp.payload);
        if (resp.status == RESP_OK) {
            g_logged_in = 1;
            load_user_from_msg(&resp);
            safe_copy(g_password, pass, sizeof(g_password));
            return 1;
        }
    }
    return 0;
}

static void logout_flow(void) {
    if (!g_logged_in) return;
    send_request(CMD_LOGOUT, g_user.username, NULL, 0);
    ipc_msg_t resp;
    if (recv_reply_block(&resp) == 0) {
        printf("%s\n", resp.payload);
    }
    memset(&g_user, 0, sizeof(g_user));
    g_logged_in = 0;
}

static void history_flow(void) {
    send_request(CMD_HISTORY, g_user.username, NULL, 0);
    ipc_msg_t resp;
    if (recv_reply_block(&resp) == 0) {
        printf("\n%s\n", resp.payload);
    }
}

static void armory_flow(void) {
    while (1) {
        printf("\n=== ARMORY ===\n");
        printf("Gold: %d\n", g_user.gold);
        printf("Current Weapon: %s (+%d)\n", g_user.weapon_name[0] ? g_user.weapon_name : "None", g_user.weapon_bonus);
        for (int i = 0; i < WEAPON_SHOP_COUNT; ++i) {
            printf("%d. %-12s | Cost %d | +%d dmg\n", i + 1, WEAPON_SHOP[i].name, WEAPON_SHOP[i].cost, WEAPON_SHOP[i].bonus);
        }
        printf("0. Back\n");
        printf("Choice: ");

        char buf[32];
        if (!fgets(buf, sizeof(buf), stdin)) return;
        int choice = atoi(buf);
        if (choice == 0) return;

        send_request(CMD_BUY_WEAPON, g_user.username, NULL, choice);
        ipc_msg_t resp;
        if (recv_reply_block(&resp) == 0) {
            printf("%s\n", resp.payload);
            if (resp.status == RESP_OK) {
                load_user_from_msg(&resp);
            }
        }
    }
}

static void battle_request_flow(void) {
    send_request(CMD_BATTLE_REQ, g_user.username, NULL, 0);
    ipc_msg_t resp;
    if (recv_reply_block(&resp) != 0) return;

    if (resp.status == RESP_ERR) {
        printf("%s\n", resp.payload);
        return;
    }

    if (resp.status == RESP_WAITING) {
        printf("%s\n", resp.payload);
        wait_for_battle_start();
    } else if (resp.status != RESP_BATTLE_START) {
        printf("%s\n", resp.payload);
        return;
    }

    battle_loop();

    send_request(CMD_LOGIN, g_user.username, g_password, 0);

    if (recv_reply_block(&resp) == 0 && resp.status == RESP_OK) {
        load_user_from_msg(&resp);
    }
}

static int connect_ipc(void) {
    g_shmid = shmget(SHM_KEY, sizeof(arena_state_t), 0666);
    if (g_shmid == -1) {
        printf("Orion are you there?\n");
        return -1;
    }

    g_state = (arena_state_t *)shmat(g_shmid, NULL, 0);
    if (g_state == (void *)-1) {
        printf("Orion are you there?\n");
        return -1;
    }

    g_mqid = msgget(MQ_KEY, 0666);
    if (g_mqid == -1) {
        printf("Orion are you there?\n");
        return -1;
    }

    g_semid = semget(SEM_KEY, 1, 0666);
    if (g_semid == -1) {
        printf("Orion are you there?\n");
        return -1;
    }

    return 0;
}

static void cleanup(void) {
    if (g_state && g_state != (void *)-1) shmdt(g_state);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (connect_ipc() == -1) {
        return 0;
    }
    atexit(cleanup);

    printf("Battle Eterion Client ready.\n");

    while (1) {
        if (!g_logged_in) {
            banner();
            printf("\n1. Register\n2. Login\n3. Exit\nChoice: ");
            char choice_buf[16];
            if (!fgets(choice_buf, sizeof(choice_buf), stdin)) break;
            int choice = atoi(choice_buf);
            if (choice == 1) {
                register_flow();
            } else if (choice == 2) {
                login_flow();
            } else if (choice == 3) {
                break;
            } else {
                printf("Invalid choice.\n");
            }
            continue;
        }

        banner();
        print_user_banner();
        printf("\n1. Battle\n2. Armory\n3. History\n4. Logout\nChoice: ");
        char choice_buf[16];
        if (!fgets(choice_buf, sizeof(choice_buf), stdin)) break;
        int choice = atoi(choice_buf);
        if (choice == 1) {
            battle_request_flow();
        } else if (choice == 2) {
            armory_flow();
        } else if (choice == 3) {
            history_flow();
        } else if (choice == 4) {
            logout_flow();
        } else {
            printf("Invalid choice.\n");
        }
    }

    logout_flow();
    return 0;
}