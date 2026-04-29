#include "arena.h"
#include <pthread.h>

static int g_shmid = -1;
static int g_mqid = -1;
static int g_semid = -1;
static arena_state_t *g_state = NULL;
static volatile sig_atomic_t g_running = 1;

typedef struct {
    char human_name[MAX_USERNAME];
} bot_thread_arg_t;

static battle_state_t *find_battle_by_user(const char *username) {
    if (!g_state || !username) return NULL;
    for (int i = 0; i < MAX_BATTLES; i++) {
        if (g_state->battles[i].active &&
            (strcmp(g_state->battles[i].p1_name, username) == 0 ||
             strcmp(g_state->battles[i].p2_name, username) == 0)) {
            return &g_state->battles[i];
        }
    }
    return NULL;
}

static battle_state_t *find_free_battle_slot(void) {
    if (!g_state) return NULL;
    for (int i = 0; i < MAX_BATTLES; i++) {
        if (!g_state->battles[i].active) return &g_state->battles[i];
    }
    return NULL;
}

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

static void send_reply(pid_t pid, int cmd, int status, const char *payload,
                       const user_record_t *user, const char *opponent,
                       int value, int value2) {
    ipc_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = (long)pid;
    msg.cmd = cmd;
    msg.status = status;
    msg.pid = getpid();
    msg.value = value;
    msg.value2 = value2;
    if (payload) safe_copy(msg.payload, payload, sizeof(msg.payload));
    if (user) {
        safe_copy(msg.username, user->username, sizeof(msg.username));
        safe_copy(msg.weapon_name, user->weapon_name, sizeof(msg.weapon_name));
        msg.gold = user->gold;
        msg.level = user->level;
        msg.xp = user->xp;
        msg.weapon_bonus = user->weapon_bonus;
    }
    if (opponent) safe_copy(msg.opponent, opponent, sizeof(msg.opponent));
    if (msgsnd(g_mqid, &msg, sizeof(msg) - sizeof(long), 0) == -1) {
        perror("msgsnd");
    }
}

static int load_users(user_record_t *users, int max_users) {
    FILE *fp = fopen(USER_DB_FILE, "r");
    if (!fp) return 0;

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && count < max_users) {
        trim_newline(line);
        if (line[0] == '\0') continue;

        user_record_t u;
        memset(&u, 0, sizeof(u));
        char weapon[MAX_WEAPON] = "None";
        int parsed = sscanf(line, "%31[^|]|%63[^|]|%d|%d|%d|%31[^|]|%d|%d",
                            u.username, u.password, &u.gold, &u.level, &u.xp,
                            weapon, &u.weapon_bonus, &u.logged_in_pid);
        if (parsed >= 5) {
            if (parsed < 6) safe_copy(weapon, "None", sizeof(weapon));
            safe_copy(u.weapon_name, weapon, sizeof(u.weapon_name));
            if (parsed < 7) u.weapon_bonus = 0;
            if (parsed < 8) u.logged_in_pid = 0;
            users[count++] = u;
        }
    }
    fclose(fp);
    return count;
}

static int save_users(const user_record_t *users, int count) {
    FILE *fp = fopen("users.db.tmp", "w");
    if (!fp) return -1;
    for (int i = 0; i < count; ++i) {
        const user_record_t *u = &users[i];
        fprintf(fp, "%s|%s|%d|%d|%d|%s|%d|%d\n",
                u->username, u->password, u->gold, u->level, u->xp,
                u->weapon_name[0] ? u->weapon_name : "None",
                u->weapon_bonus, u->logged_in_pid);
    }
    fclose(fp);
    if (rename("users.db.tmp", USER_DB_FILE) != 0) {
        perror("rename");
        return -1;
    }
    return 0;
}

static int find_user_index(const user_record_t *users, int count, const char *username) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(users[i].username, username) == 0) return i;
    }
    return -1;
}

static void init_default_user(user_record_t *u, const char *username, const char *password) {
    memset(u, 0, sizeof(*u));
    safe_copy(u->username, username, sizeof(u->username));
    safe_copy(u->password, password, sizeof(u->password));
    u->gold = 150;
    u->level = 1;
    u->xp = 0;
    safe_copy(u->weapon_name, "None", sizeof(u->weapon_name));
    u->weapon_bonus = 0;
    u->logged_in_pid = 0;
}


static void append_history(const char *username, const char *opponent, const char *result,
                           int xp, int gold, int level, const char *weapon, int is_bot) {
    (void)gold; (void)level; (void)weapon; (void)is_bot;
    FILE *fp = fopen(HISTORY_FILE, "a");
    if (!fp) return;
    char ts[64];
    time_t now = time(NULL);
    format_history_time(ts, sizeof(ts), now);
    fprintf(fp, "%s|%s|%s|%s|%d\n", ts, username ? username : "-", opponent ? opponent : "-", result ? result : "-", xp);
    fclose(fp);
}

static void format_history(const char *username, char *out, size_t outsz) {
    if (!out || outsz == 0) return;
    out[0] = '\0';
    out[0] = '\0';
    FILE *fp = fopen(HISTORY_FILE, "r");
    if (!fp) {
        snprintf(out, outsz, "No history found for %s.", username);
        return;
    }

    char line[512], items[MAX_HISTORY][256];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (!line[0]) continue;
        char ts[64], base[MAX_USERNAME], opp[MAX_USERNAME], result[16];
        int xp = 0;
        if (sscanf(line, "%63[^|]|%31[^|]|%31[^|]|%15[^|]|%d", ts, base, opp, result, &xp) != 5) continue;
        if (strcmp(base, username) != 0 || count >= MAX_HISTORY) continue;
        snprintf(items[count++], sizeof(items[0]), "%s|%s|%s|+%d", ts, opp, result, xp);
    }
    fclose(fp);

    if (!count) {
        snprintf(out, outsz, "No history found for %s.", username);
        return;
    }

    size_t used = snprintf(out, outsz, "History for %s:\n|TIMESTAMP|OPPONENT|RESULT|XP|\n", username);
    for (int i = 0; i < count && used < outsz; ++i)
        used += snprintf(out + used, outsz - used, "%s\n", items[i]);

}
static void ensure_level(user_record_t *u) {
    if (!u) return;
    u->level = level_from_xp(u->xp);
}

static void apply_rewards(user_record_t *winner, user_record_t *loser) {
    if (winner) {
        winner->xp += 50;
        winner->gold += 120;
        ensure_level(winner);
    }
    if (loser) {
        loser->xp += 15;
        loser->gold += 30;
        ensure_level(loser);
    }
}

static void init_bot_user(const user_record_t *human, user_record_t *bot) {
    memset(bot, 0, sizeof(*bot));
    safe_copy(bot->username, "BOT", sizeof(bot->username));
    safe_copy(bot->password, "-", sizeof(bot->password));
    bot->gold = 0;
    bot->level = human ? human->level : 1;
    bot->xp = 120 + (human ? human->level * 10 : 0);
    safe_copy(bot->weapon_name, "Bot Fang", sizeof(bot->weapon_name));
    bot->weapon_bonus = 5 + (human ? human->level : 0);
    bot->logged_in_pid = 0;
}

static void reset_battle(battle_state_t *b) {
    memset(b, 0, sizeof(*b));
}


static battle_state_t *start_human_battle_locked(const user_record_t *u1, pid_t p1_pid,
                                                 const user_record_t *u2, pid_t p2_pid) {
    battle_state_t *b = find_free_battle_slot();
    if (!b) return NULL;
    reset_battle(b);
    b->active = 1;
    b->mode = 2;
    safe_copy(b->p1_name, u1->username, sizeof(b->p1_name));
    safe_copy(b->p2_name, u2->username, sizeof(b->p2_name));
    b->p1_pid = p1_pid;
    b->p2_pid = p2_pid;
    b->p1_user = *u1;
    b->p2_user = *u2;
    b->p1_hp = calc_health(&b->p1_user);
    b->p2_hp = calc_health(&b->p2_user);
    b->log_count = 0;
    push_log(b->logs, &b->log_count, "Battle started: %s vs %s", u1->username, u2->username);
    safe_copy(b->result, "", sizeof(b->result));
    return b;
}


static battle_state_t *start_bot_battle_locked(const user_record_t *u, pid_t p1_pid) {
    battle_state_t *b = find_free_battle_slot();
    if (!b) return NULL;
    reset_battle(b);
    b->active = 1;
    b->mode = 1;
    safe_copy(b->p1_name, u->username, sizeof(b->p1_name));
    safe_copy(b->p2_name, "BOT", sizeof(b->p2_name));
    b->p1_pid = p1_pid;
    b->p2_pid = 0;
    b->p1_user = *u;
    init_bot_user(u, &b->p2_user);
    b->p1_hp = calc_health(&b->p1_user);
    b->p2_hp = 140 + u->level * 15;
    b->p1_last_attack = 0;
    b->p2_last_attack = 0;
    b->log_count = 0;
    push_log(b->logs, &b->log_count, "Battle started: %s vs BOT", u->username);
    safe_copy(b->result, "", sizeof(b->result));
    return b;
}

static void save_battle_users_and_history(battle_state_t *b, int attacker_side) {
    if (!b) return;

    user_record_t users[MAX_USERS];
    int count = load_users(users, MAX_USERS);

    if (b->mode == 2) {
        user_record_t p1 = b->p1_user;
        user_record_t p2 = b->p2_user;
        if (attacker_side == 1) {
            apply_rewards(&p1, &p2);
        } else {
            apply_rewards(&p2, &p1);
        }

        int i1 = find_user_index(users, count, b->p1_name);
        int i2 = find_user_index(users, count, b->p2_name);
        if (i1 >= 0) users[i1] = p1;
        if (i2 >= 0) users[i2] = p2;
        save_users(users, count);

        b->p1_user = p1;
        b->p2_user = p2;
        int xp_p1 = (attacker_side == 1) ? 50 : 15;
        int xp_p2 = (attacker_side == 2) ? 50 : 15;

        append_history(p1.username, p2.username,
                    attacker_side == 1 ? "WIN" : "LOSE",
                    xp_p1, 0, 0, NULL, 0);

        append_history(p2.username, p1.username,
                    attacker_side == 2 ? "WIN" : "LOSE",
                    xp_p2, 0, 0, NULL, 0);

    } else {
        user_record_t human;
        int human_side = 0;
        if (strcmp(b->p1_name, "BOT") != 0) {
            human = b->p1_user;
            human_side = 1;
        } else {
            human = b->p2_user;
            human_side = 2;
        }

        if (attacker_side == human_side) {
            apply_rewards(&human, NULL);
        } else {
            apply_rewards(NULL, &human);
        }

        int i = find_user_index(users, count, human.username);
        if (i >= 0) users[i] = human;
        save_users(users, count);

        if (strcmp(b->p1_name, "BOT") != 0) {
            b->p1_user = human;
        } else {
            b->p2_user = human;
        }

        int xp_gain = (attacker_side == human_side) ? 50 : 15;

        append_history(human.username, "BOT",
                    attacker_side == human_side ? "WIN" : "LOSE",
                    xp_gain, 0, 0, NULL, 1);

    }
}

static int attack_locked(battle_state_t *b, const char *attacker_name, int attack_type,
                         char *info, size_t infosz, int *ended, int *attacker_side) {
    time_t now = time(NULL);
    *ended = 0;
    *attacker_side = 0;

    if (info && infosz) info[0] = '\0';

    if (!b || !b->active) {
        snprintf(info, infosz, "No active battle.");
        return -1;
    }

    int side = 0;
    if (strcmp(attacker_name, b->p1_name) == 0) side = 1;
    else if (strcmp(attacker_name, b->p2_name) == 0) side = 2;
    else if (b->mode == 1 && strcmp(attacker_name, "BOT") == 0)
        side = (strcmp(b->p1_name, "BOT") == 0) ? 1 : 2;

    if (side == 0) {
        snprintf(info, infosz, "You are not part of this battle.");
        return -1;
    }

    user_record_t *attacker = (side == 1) ? &b->p1_user : &b->p2_user;
    int *def_hp = (side == 1) ? &b->p2_hp : &b->p1_hp;
    time_t *last = (side == 1) ? &b->p1_last_attack : &b->p2_last_attack;

    if (now - *last < 1) {
        snprintf(info, infosz, "Cooldown aktif. Tunggu 1 detik.");
        return 1;
    }

    if (attack_type == 2 && attacker->weapon_bonus <= 0) {
        snprintf(info, infosz, "Ultimate gagal: belum punya senjata.");
        return 1;
    }

    int dmg = calc_damage(attacker);
    if (attack_type == 2) dmg *= 3;

    *def_hp -= dmg;
    if (*def_hp < 0) *def_hp = 0;
    *last = now;

    if (attack_type == 2) {
        push_log(b->logs, &b->log_count, "%s used Ultimate for %d damage.", attacker->username, dmg);
    } else {
        push_log(b->logs, &b->log_count, "%s attacked for %d damage.", attacker->username, dmg);
    }

    snprintf(info, infosz, "%s dealt %d damage.", attacker->username, dmg);
    *attacker_side = side;

    if (*def_hp <= 0) {
        *ended = 1;
        b->active = 0;
        snprintf(b->result, sizeof(b->result), "%s won against %s.",
                 attacker->username, side == 1 ? b->p2_name : b->p1_name);
        push_log(b->logs, &b->log_count, "%s", b->result);
    }

    return 0;
}

static void reply_battle_end(const battle_state_t *b) {
    if (!b) return;
    if (b->mode == 2) {
        send_reply(b->p1_pid, CMD_ATTACK, RESP_BATTLE_END, b->result, &b->p1_user, b->p2_name, 0, 0);
        send_reply(b->p2_pid, CMD_ATTACK, RESP_BATTLE_END, b->result, &b->p2_user, b->p1_name, 0, 0);
    } else {
        pid_t human_pid = (strcmp(b->p1_name, "BOT") != 0) ? b->p1_pid : b->p2_pid;
        const user_record_t *human = (strcmp(b->p1_name, "BOT") != 0) ? &b->p1_user : &b->p2_user;
        send_reply(human_pid, CMD_ATTACK, RESP_BATTLE_END, b->result, human, "BOT", 0, 0);
    }
}

static void send_battle_start(pid_t pid, const battle_state_t *b, int side) {
    const user_record_t *u = side == 1 ? &b->p1_user : &b->p2_user;
    const char *opp = side == 1 ? b->p2_name : b->p1_name;
    send_reply(pid, CMD_BATTLE_REQ, RESP_BATTLE_START, "Battle mulai!", u, opp, 0, 0);
}

static void *bot_thread(void *arg) {
    bot_thread_arg_t *ctx = (bot_thread_arg_t *)arg;
    char human_name[MAX_USERNAME];
    safe_copy(human_name, ctx->human_name, sizeof(human_name));
    free(ctx);

    while (g_running) {
        sleep(1);

        sem_lock();

        battle_state_t *b = find_battle_by_user(human_name);
        if (!b || !b->active || b->mode != 1) {
            sem_unlock();
            break;
        }

        char info[256];
        int ended = 0, side = 0;

        int rc = attack_locked(b, "BOT", 1, info, sizeof(info), &ended, &side);

        if (rc == 0 && ended) {
            save_battle_users_and_history(b, side);
            battle_state_t snapshot = *b;
            sem_unlock();
            reply_battle_end(&snapshot);
            break;
        }

        sem_unlock();
    }

    return NULL;
}

static void clear_waiting_state(void) {
    g_state->waiting_active = 0;
    g_state->waiting_user[0] = '\0';
    g_state->waiting_pid = 0;
    g_state->waiting_since = 0;
    memset(&g_state->waiting_user_record, 0, sizeof(g_state->waiting_user_record));
}


static void *watcher_thread(void *arg) {
    (void)arg;
    while (g_running) {
        sleep(1);
        pid_t human_pid = 0;
        user_record_t human;
        int start_bot = 0;

        sem_lock();
        if (g_state->waiting_active) {
            if (find_battle_by_user(g_state->waiting_user) != NULL) clear_waiting_state();

            if (g_state->waiting_active && difftime(time(NULL), g_state->waiting_since) >= 35.0) {
                human = g_state->waiting_user_record;
                human_pid = g_state->waiting_pid;
                if (start_bot_battle_locked(&human, human_pid)) {
                    clear_waiting_state();
                    start_bot = 1;
                }
            }
        }
        sem_unlock();

        if (start_bot) {
            send_reply(human_pid, CMD_BATTLE_REQ, RESP_BATTLE_START,
                       "No opponent found in 35 seconds. Monster battle begins.", &human, "BOT", 0, 0);
            bot_thread_arg_t *ctx = malloc(sizeof(*ctx));
            if (ctx) {
                safe_copy(ctx->human_name, human.username, sizeof(ctx->human_name));
                pthread_t tid;
                if (pthread_create(&tid, NULL, bot_thread, ctx) == 0) pthread_detach(tid);
                else free(ctx);
            }
        }
    }
    return NULL;
}

static void handle_register(const ipc_msg_t *req) {
    user_record_t users[MAX_USERS];
    sem_lock();
    int count = load_users(users, MAX_USERS);
    if (find_user_index(users, count, req->username) >= 0) {
        sem_unlock();
        send_reply(req->pid, CMD_REGISTER, RESP_ERR, "Username already exists.", NULL, NULL, 0, 0);
        return;
    }
    if (count >= MAX_USERS) {
        sem_unlock();
        send_reply(req->pid, CMD_REGISTER, RESP_ERR, "User database is full.", NULL, NULL, 0, 0);
        return;
    }
    init_default_user(&users[count], req->username, req->password);
    if (save_users(users, count + 1) != 0) {
        sem_unlock();
        send_reply(req->pid, CMD_REGISTER, RESP_ERR, "Failed to save user.", NULL, NULL, 0, 0);
        return;
    }
    user_record_t created = users[count];
    sem_unlock();
    send_reply(req->pid, CMD_REGISTER, RESP_OK, "Account created.", &created, NULL, 0, 0);
}

static void handle_login(const ipc_msg_t *req) {
    user_record_t users[MAX_USERS];
    sem_lock();
    int count = load_users(users, MAX_USERS);
    int idx = find_user_index(users, count, req->username);
    if (idx < 0) {
        sem_unlock();
        send_reply(req->pid, CMD_LOGIN, RESP_ERR, "Username not found.", NULL, NULL, 0, 0);
        return;
    }

    user_record_t *u = &users[idx];
    if (strcmp(u->password, req->password) != 0) {
        sem_unlock();
        send_reply(req->pid, CMD_LOGIN, RESP_ERR, "Wrong password.", NULL, NULL, 0, 0);
        return;
    }

    if (u->logged_in_pid != 0 && u->logged_in_pid != req->pid && pid_alive(u->logged_in_pid)) {
        sem_unlock();
        send_reply(req->pid, CMD_LOGIN, RESP_ERR, "Account is already active in another session.", NULL, NULL, 0, 0);
        return;
    }

    u->logged_in_pid = req->pid;
    if (save_users(users, count) != 0) {
        sem_unlock();
        send_reply(req->pid, CMD_LOGIN, RESP_ERR, "Failed to update login state.", NULL, NULL, 0, 0);
        return;
    }
    user_record_t logged = *u;
    sem_unlock();
    send_reply(req->pid, CMD_LOGIN, RESP_OK, "Login successful.", &logged, NULL, 0, 0);
}

static void handle_logout(const ipc_msg_t *req) {
    user_record_t users[MAX_USERS];
    sem_lock();
    int count = load_users(users, MAX_USERS);
    int idx = find_user_index(users, count, req->username);
    if (idx >= 0 && (users[idx].logged_in_pid == req->pid || !pid_alive(users[idx].logged_in_pid))) {
        users[idx].logged_in_pid = 0;
        save_users(users, count);
    }
    if (g_state->waiting_active && strcmp(g_state->waiting_user, req->username) == 0 && g_state->waiting_pid == req->pid) {
        clear_waiting_state();
    }
    sem_unlock();
    send_reply(req->pid, CMD_LOGOUT, RESP_OK, "Logout successful.", NULL, NULL, 0, 0);
}

static void handle_history(const ipc_msg_t *req) {
    char buf[MAX_TEXT];
    sem_lock();
    format_history(req->username, buf, sizeof(buf));
    sem_unlock();
    send_reply(req->pid, CMD_HISTORY, RESP_INFO, buf, NULL, NULL, 0, 0);
}

static void handle_buy(const ipc_msg_t *req) {
    user_record_t users[MAX_USERS];
    sem_lock();
    int count = load_users(users, MAX_USERS);
    int idx = find_user_index(users, count, req->username);
    if (idx < 0) {
        sem_unlock();
        send_reply(req->pid, CMD_BUY_WEAPON, RESP_ERR, "Account not found.", NULL, NULL, 0, 0);
        return;
    }

    if (users[idx].logged_in_pid != req->pid && pid_alive(users[idx].logged_in_pid)) {
        sem_unlock();
        send_reply(req->pid, CMD_BUY_WEAPON, RESP_ERR, "Please login first.", NULL, NULL, 0, 0);
        return;
    }

    int choice = req->value;
    if (choice < 1 || choice > WEAPON_SHOP_COUNT) {
        sem_unlock();
        send_reply(req->pid, CMD_BUY_WEAPON, RESP_ERR, "Invalid weapon choice.", &users[idx], NULL, 0, 0);
        return;
    }

    const weapon_item_t *item = &WEAPON_SHOP[choice - 1];
    if (item->bonus <= users[idx].weapon_bonus) {
        sem_unlock();
        send_reply(req->pid, CMD_BUY_WEAPON, RESP_ERR, "You already have an equal or better weapon.", &users[idx], NULL, 0, 0);
        return;
    }
    if (users[idx].gold < item->cost) {
        sem_unlock();
        send_reply(req->pid, CMD_BUY_WEAPON, RESP_ERR, "Not enough gold.", &users[idx], NULL, 0, 0);
        return;
    }

    users[idx].gold -= item->cost;
    users[idx].weapon_bonus = item->bonus;
    safe_copy(users[idx].weapon_name, item->name, sizeof(users[idx].weapon_name));
    if (save_users(users, count) != 0) {
        sem_unlock();
        send_reply(req->pid, CMD_BUY_WEAPON, RESP_ERR, "Failed to save purchase.", &users[idx], NULL, 0, 0);
        return;
    }
    user_record_t updated = users[idx];
    sem_unlock();
    send_reply(req->pid, CMD_BUY_WEAPON, RESP_OK, "Weapon purchased.", &updated, NULL, 0, 0);
}


static void handle_battle_request(const ipc_msg_t *req) {
    user_record_t users[MAX_USERS];
    sem_lock();
    int count = load_users(users, MAX_USERS), idx = find_user_index(users, count, req->username);
    if (idx < 0) { sem_unlock(); send_reply(req->pid, CMD_BATTLE_REQ, RESP_ERR, "Account not found.", NULL, NULL, 0, 0); return; }

    user_record_t me = users[idx];
    if (find_battle_by_user(me.username)) { sem_unlock(); send_reply(req->pid, CMD_BATTLE_REQ, RESP_ERR, "You are already in battle.", &me, NULL, 0, 0); return; }
    if (g_state->waiting_active && find_battle_by_user(g_state->waiting_user)) clear_waiting_state();

    if (g_state->waiting_active) {
        if (strcmp(g_state->waiting_user, me.username) == 0) { sem_unlock(); send_reply(req->pid, CMD_BATTLE_REQ, RESP_WAITING, "You are already waiting.", &me, NULL, 0, 0); return; }
        user_record_t opp = g_state->waiting_user_record; pid_t opp_pid = g_state->waiting_pid;
        battle_state_t *b = start_human_battle_locked(&opp, opp_pid, &me, req->pid);
        if (!b) { sem_unlock(); send_reply(req->pid, CMD_BATTLE_REQ, RESP_ERR, "Battle slots are full.", &me, NULL, 0, 0); return; }
        clear_waiting_state();
        battle_state_t snapshot = *b;
        sem_unlock();
        send_battle_start(opp_pid, &snapshot, 1);
        send_battle_start(req->pid, &snapshot, 2);
        return;
    }

    g_state->waiting_active = 1;
    g_state->waiting_pid = req->pid;
    g_state->waiting_since = time(NULL);
    safe_copy(g_state->waiting_user, me.username, sizeof(g_state->waiting_user));
    g_state->waiting_user_record = me;
    sem_unlock();
    send_reply(req->pid, CMD_BATTLE_REQ, RESP_WAITING, "Menunggu lawan...", &me, NULL, 0, 0);
}


static void handle_attack(const ipc_msg_t *req) {
    char info[256];
    int ended = 0, attacker_side = 0;
    sem_lock();
    battle_state_t *b = find_battle_by_user(req->username);
    if (!b) { sem_unlock(); send_reply(req->pid, CMD_ATTACK, RESP_ERR, "You are not part of this battle.", NULL, NULL, 0, 0); return; }

    int rc = attack_locked(b, req->username, req->value, info, sizeof(info), &ended, &attacker_side);
    if (rc != 0) { sem_unlock(); send_reply(req->pid, CMD_ATTACK, RESP_ERR, info, NULL, NULL, 0, 0); return; }
    if (!ended) { sem_unlock(); send_reply(req->pid, CMD_ATTACK, RESP_BATTLE_UPDATE, info, NULL, NULL, 0, 0); return; }

    save_battle_users_and_history(b, attacker_side);
    battle_state_t snapshot = *b;
    sem_unlock();
    reply_battle_end(&snapshot);
}

static void handle_ping(const ipc_msg_t *req) {
    send_reply(req->pid, CMD_PING, RESP_OK, "PONG", NULL, NULL, 0, 0);
}

static void cleanup_ipc(void) {
    if (g_state && g_state != (void *)-1) shmdt(g_state);
    if (g_shmid != -1) shmctl(g_shmid, IPC_RMID, NULL);
    if (g_mqid != -1) msgctl(g_mqid, IPC_RMID, NULL);
    if (g_semid != -1) semctl(g_semid, 0, IPC_RMID);
}

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
    cleanup_ipc();
    _exit(0);
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_shmid = shmget(SHM_KEY, sizeof(arena_state_t), IPC_CREAT | 0666);
    if (g_shmid == -1) {
        perror("shmget");
        return 1;
    }
    g_state = (arena_state_t *)shmat(g_shmid, NULL, 0);
    if (g_state == (void *)-1) {
        perror("shmat");
        return 1;
    }

    g_mqid = msgget(MQ_KEY, IPC_CREAT | 0666);
    if (g_mqid == -1) {
        perror("msgget");
        return 1;
    }

    g_semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (g_semid == -1) {
        perror("semget");
        return 1;
    }
    union semun su;
    su.val = 1;
    if (semctl(g_semid, 0, SETVAL, su) == -1) {
        perror("semctl");
        return 1;
    }

    sem_lock();
    if (g_state->initialized != 1) {
        memset(g_state, 0, sizeof(*g_state));
        g_state->initialized = 1;
    }
    g_state->waiting_active = 0;
    g_state->waiting_user[0] = '\0';
    g_state->waiting_pid = 0;
    g_state->waiting_since = 0;
    memset(&g_state->waiting_user_record, 0, sizeof(g_state->waiting_user_record));
    for (int i = 0; i < MAX_BATTLES; i++) {
        g_state->battles[i].active = 0;
    }
    sem_unlock();

    printf("Orion is ready (PID: %d)\n", getpid());
    fflush(stdout);

    pthread_t watcher;
    if (pthread_create(&watcher, NULL, watcher_thread, NULL) == 0) {
        pthread_detach(watcher);
    }

    while (g_running) {
        ipc_msg_t req;
        memset(&req, 0, sizeof(req));
        ssize_t r = msgrcv(g_mqid, &req, sizeof(req) - sizeof(long), 1, 0);
        if (r == -1) {
            if (errno == EINTR) continue;
            perror("msgrcv");
            break;
        }

        switch (req.cmd) {
            case CMD_REGISTER:   handle_register(&req); break;
            case CMD_LOGIN:      handle_login(&req); break;
            case CMD_LOGOUT:     handle_logout(&req); break;
            case CMD_BATTLE_REQ: handle_battle_request(&req); break;
            case CMD_ATTACK:     handle_attack(&req); break;
            case CMD_BUY_WEAPON: handle_buy(&req); break;
            case CMD_HISTORY:    handle_history(&req); break;
            case CMD_PING:       handle_ping(&req); break;
            case CMD_EXIT:       handle_logout(&req); break;
            default:
                send_reply(req.pid, req.cmd, RESP_ERR, "Unknown command.", NULL, NULL, 0, 0);
                break;
        }
    }

    cleanup_ipc();
    return 0;
}
