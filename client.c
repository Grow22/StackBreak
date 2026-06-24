/*
 * client.c - Stack and Break Game Client
 *
 * Connects to server, receives game state, renders with ncurses,
 * and sends player input.
 *
 * System calls used: socket, connect, read, write, close,
 *   ioctl (terminal size), sigaction, pthread_create
 */

#include "common.h"
#include <ncurses.h>
#include <locale.h>

/* Globals */
static int       g_sock = -1;
static int       g_role = -1;  /* 0=attacker, 1=defender */
static GameState g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_running = 1;
static const GameState *g_render_state = NULL;

/* Colors */
#define COLOR_PIECE_I   1
#define COLOR_PIECE_O   2
#define COLOR_PIECE_T   3
#define COLOR_PIECE_S   4
#define COLOR_PIECE_Z   5
#define COLOR_PIECE_J   6
#define COLOR_PIECE_L   7
#define COLOR_DEFENDER      8
#define COLOR_DEFENDER_STUN 9
#define COLOR_BORDER    10
#define COLOR_GHOST     11
#define COLOR_BG        12
#define COLOR_BOMB_PREVIEW 13
#define COLOR_BOMB_FLASH   14
#define COLOR_DRILL_FX     15
#define COLOR_SHIELD_FX    16
#define COLOR_GUN_FX       17
#define COLOR_HIT_FX       18
#define COLOR_BOMB_BLOCK_PREVIEW 19
#define COLOR_COMBO        20

static void init_colors(void) {
    start_color();
    /* use_default_colors() removed to prevent WSL blank screen bugs */
    init_pair(COLOR_PIECE_I,   COLOR_WHITE,   COLOR_CYAN);
    init_pair(COLOR_PIECE_O,   COLOR_BLACK,   COLOR_WHITE);
    init_pair(COLOR_PIECE_T,   COLOR_WHITE,   COLOR_MAGENTA);
    init_pair(COLOR_PIECE_S,   COLOR_WHITE,   COLOR_GREEN);
    init_pair(COLOR_PIECE_Z,   COLOR_WHITE,   COLOR_RED);
    init_pair(COLOR_PIECE_J,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(COLOR_PIECE_L,   COLOR_BLACK,   COLOR_WHITE);
    init_pair(COLOR_DEFENDER,      COLOR_BLACK,   COLOR_GREEN);
    init_pair(COLOR_DEFENDER_STUN, COLOR_WHITE,   COLOR_RED);
    init_pair(COLOR_BORDER,    COLOR_WHITE,   COLOR_BLACK);
    init_pair(COLOR_GHOST,     COLOR_WHITE,   COLOR_BLACK); /* Fixed ghost color */
    init_pair(COLOR_BG,        COLOR_WHITE,   COLOR_BLACK);
    init_pair(COLOR_BOMB_PREVIEW, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_BOMB_FLASH,   COLOR_WHITE, COLOR_RED);
    init_pair(COLOR_DRILL_FX,     COLOR_BLACK, COLOR_WHITE);
    init_pair(COLOR_SHIELD_FX,    COLOR_BLACK, COLOR_CYAN);
    init_pair(COLOR_GUN_FX,       COLOR_BLACK, COLOR_GREEN);
    init_pair(COLOR_HIT_FX,       COLOR_WHITE, COLOR_MAGENTA);
    init_pair(COLOR_BOMB_BLOCK_PREVIEW, COLOR_BLACK, COLOR_YELLOW);
    init_pair(COLOR_COMBO,     COLOR_YELLOW,  COLOR_BLACK);
    init_pair(21,              COLOR_RED,     COLOR_BLACK);
    init_pair(22,              COLOR_GREEN,   COLOR_BLACK);
    init_pair(23,              COLOR_BLACK,   COLOR_CYAN);
    init_pair(24,              COLOR_BLACK,   COLOR_WHITE);
    init_pair(25,              COLOR_BLACK,   COLOR_MAGENTA);
    init_pair(26,              COLOR_BLACK,   COLOR_GREEN);
    init_pair(27,              COLOR_BLACK,   COLOR_RED);
    init_pair(28,              COLOR_BLACK,   COLOR_BLUE);
    init_pair(29,              COLOR_BLACK,   COLOR_WHITE);
    if (COLORS > 240) {
        init_pair(30,          COLOR_WHITE,   208);
        init_pair(31,          COLOR_WHITE,   240);
        init_pair(32,          COLOR_WHITE,   201);
        init_pair(33,          COLOR_MAGENTA, COLOR_BLACK);
    } else {
        init_pair(30,          COLOR_WHITE,   COLOR_RED);
        init_pair(31,          COLOR_WHITE,   COLOR_BLACK);
        init_pair(32,          COLOR_WHITE,   COLOR_MAGENTA);
        init_pair(33,          COLOR_MAGENTA, COLOR_BLACK);
    }
}

static void show_start_screen(const ScoreTable *rankings,
                              char player_name[MAX_NAME_LEN]) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int height = 22;
    int width = 64;
    int start_y = (rows - height) / 2;
    int start_x = (cols - width) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;

    WINDOW *win = newwin(height, width, start_y, start_x);
    keypad(win, TRUE);
    wtimeout(win, -1);
    box(win, 0, 0);

    wattron(win, A_BOLD);
    mvwprintw(win, 1, 21, "MULTI LEADERBOARD");
    wattroff(win, A_BOLD);
    mvwprintw(win, 2, 4, "Role: %s",
              g_role == 0 ? "ATTACKER" : "DEFENDER");
    mvwprintw(win, 3, 4, "High Score: %d",
              rankings->count > 0 ? rankings->entries[0].score : 0);

    for (int i = 0; i < MAX_RANKINGS; i++) {
        if (i < rankings->count) {
            char team[MAX_NAME_LEN * 2 + 4];
            if (rankings->entries[i].player2[0] != '\0')
                snprintf(team, sizeof(team), "%s / %s",
                         rankings->entries[i].player1,
                         rankings->entries[i].player2);
            else
                snprintf(team, sizeof(team), "%s",
                         rankings->entries[i].player1);
            mvwprintw(win, 5 + i, 4, "%2d. %-36.36s %8d",
                      i + 1, team, rankings->entries[i].score);
        } else {
            mvwprintw(win, 5 + i, 4, "%2d. %-36s %8s",
                      i + 1, "---", "---");
        }
    }

    mvwprintw(win, 17, 4, "Name: ");
    mvwprintw(win, 19, 4, "Enter your name and press Enter to join");
    wmove(win, 17, 10);
    wrefresh(win);

    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);
    int input_result;
    do {
        player_name[0] = '\0';
        wmove(win, 17, 10);
        whline(win, ' ', MAX_NAME_LEN - 1);
        wmove(win, 17, 10);
        wrefresh(win);
        input_result = wgetnstr(win, player_name, MAX_NAME_LEN - 1);
        if (input_result == ERR) napms(10);
    } while (g_running && (input_result == ERR || player_name[0] == '\0'));
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);

    if (g_running && player_name[0] == '\0')
        strncpy(player_name, "Guest", MAX_NAME_LEN);
    player_name[MAX_NAME_LEN - 1] = '\0';

    delwin(win);
    clear();
    refresh();
}

static void show_lobby_waiting_screen(const char *player_name) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int height = 7;
    const int width = 54;
    int start_y = (rows - height) / 2;
    int start_x = (cols - width) / 2;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;

    clear();
    WINDOW *win = newwin(height, width, start_y, start_x);
    box(win, 0, 0);
    wattron(win, A_BOLD);
    mvwprintw(win, 1, 18, "MULTIPLAYER LOBBY");
    wattroff(win, A_BOLD);
    mvwprintw(win, 3, 4, "Name: %-15.15s  Role: %s", player_name,
              g_role == 0 ? "ATTACKER" : "DEFENDER");
    mvwprintw(win, 5, 8, "Ready. Waiting for the other player...");
    wrefresh(win);
    delwin(win);
}

static int piece_color(int type) {
    if (type >= 1 && type <= 7) return type;
    return COLOR_BG;
}

static int item_color(int type) {
    if (type >= 1 && type <= 7) return type + 22;
    return COLOR_BG;
}

static double ticks_to_sec(int t) {
    return t / 30.0;
}

/* Signal Handler */
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* Network Receiver Thread */
static void *net_receiver(void *arg) {
    (void)arg;
    while (g_running) {
        int msg_type;
        if (recv_all(g_sock, &msg_type, sizeof(int)) < 0)
            break;

        if (msg_type == MSG_STATE) {
            GameState tmp;
            if (recv_all(g_sock, &tmp, sizeof(GameState)) < 0)
                break;
            pthread_mutex_lock(&g_lock);
            memcpy(&g_state, &tmp, sizeof(GameState));
            pthread_mutex_unlock(&g_lock);
        }
    }
    g_running = 0;
    return NULL;
}

/* Send Input */
static void send_key(int key) {
    MsgInput msg;
    msg.type = MSG_INPUT;
    msg.key  = key;
    send_all(g_sock, &msg, sizeof(MsgInput));
}

/* Ghost Piece (drop preview) */
static int ghost_row(void) {
    const GameState *st = g_render_state;
    int gr = st->piece_r;
    while (1) {
        int ok = 1;
        if (st->piece_type < 1 || st->piece_type > 7) break;
        const Shape *s = &SHAPES[st->piece_type][st->piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = (gr + 1) + s->cells[i][0];
            int c = st->piece_c + s->cells[i][1];
            if (r >= BOARD_H || c < 0 || c >= BOARD_W) { ok = 0; break; }
            if (r >= 0 && st->board[r][c] != 0) { ok = 0; break; }
        }
        if (!ok) break;
        gr++;
    }
    return gr;
}

/* Rendering */
#define CELL_W 2  /* each cell is 2 chars wide */

static void draw_cell(int y, int x, int type, int is_ghost, int item_type) {
    if (is_ghost) {
        attron(COLOR_PAIR(COLOR_GHOST) | A_DIM);
        mvprintw(y, x, "[]");
        attroff(COLOR_PAIR(COLOR_GHOST) | A_DIM);
    } else {
        int color_type = type % 10;
        int stored_item = type / 10;
        if (item_type == 0) item_type = stored_item;

        if (color_type >= 1 && color_type <= 7) {
            if (item_type >= 1 && item_type <= 4) {
                attron(COLOR_PAIR(item_color(color_type)));
                if (item_type == 1) mvprintw(y, x, " B");
                else if (item_type == 2) mvprintw(y, x, " D");
                else if (item_type == 3) mvprintw(y, x, " S");
                else if (item_type == 4) mvprintw(y, x, " G");
                attroff(COLOR_PAIR(item_color(color_type)));
            } else {
                attron(COLOR_PAIR(piece_color(color_type)));
                mvprintw(y, x, "  ");
                attroff(COLOR_PAIR(piece_color(color_type)));
            }
        } else {
            mvprintw(y, x, "  ");
        }
    }
}

static void draw_bomb_prev(int y, int x, int type) {
    if (type == 0) {
        attron(COLOR_PAIR(COLOR_BOMB_PREVIEW));
        mvprintw(y, x, "..");
        attroff(COLOR_PAIR(COLOR_BOMB_PREVIEW));
    } else {
        int item_type = type / 10;

        attron(COLOR_PAIR(30) | A_BOLD);
        if (item_type == 1) mvprintw(y, x, " B");
        else if (item_type == 2) mvprintw(y, x, " D");
        else if (item_type == 3) mvprintw(y, x, " S");
        else if (item_type == 4) mvprintw(y, x, " G");
        else mvprintw(y, x, "  ");
        attroff(COLOR_PAIR(30) | A_BOLD);
    }
}

static int has_ready_bomb(void) {
    const GameState *st = g_render_state;
    return st->defender.inv_count > 0 && st->defender.inventory[0] == 1;
}

static int is_bomb_preview(int r, int c) {
    const GameState *st = g_render_state;
    if (st->game_over || !has_ready_bomb())
        return 0;
    if (r == st->defender.y && c == st->defender.x)
        return 0;

    int left = st->defender.x - 2;
    int top = st->defender.y - 2;

    return r >= top && r < top + 4 && c >= left && c < left + 4;
}

static void draw_fx_cell(int y, int x, int color, const char *text) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    if (y < 0 || y >= max_y || x < 0 || x + 1 >= max_x)
        return;

    attron(COLOR_PAIR(color) | A_BOLD);
    mvprintw(y, x, "%s", text);
    attroff(COLOR_PAIR(color) | A_BOLD);
}

static void draw_effects(int sy, int sx) {
    const GameState *st = g_render_state;
    int count = st->num_effects;
    if (count < 0) count = 0;
    if (count > MAX_EFFECTS) count = MAX_EFFECTS;

    for (int i = 0; i < count; i++) {
        const VisualEffect *fx = &st->effects[i];

        if (fx->type == EFFECT_BOMB) {
            int left = fx->x - 2;
            int top = fx->y - 2;
            const char *text = (fx->timer > 6) ? "**" :
                               (fx->timer > 3) ? "!!" : "..";
            for (int r = top; r < top + 4; r++) {
                for (int c = left; c < left + 4; c++) {
                    if (r < 0 || r >= BOARD_H || c < 0 || c >= BOARD_W)
                        continue;
                    draw_fx_cell(sy + 1 + r,
                                 sx + 1 + c * CELL_W,
                                 COLOR_BOMB_FLASH, text);
                }
            }
        } else if (fx->type == EFFECT_DRILL) {
            if (fx->y >= 0 && fx->y < BOARD_H && fx->x >= 0 && fx->x < BOARD_W) {
                draw_fx_cell(sy + 1 + fx->y,
                             sx + 1 + fx->x * CELL_W,
                             COLOR_DRILL_FX, "!!");
            }
        } else if (fx->type == EFFECT_SHIELD) {
            if (fx->y >= 0 && fx->y < BOARD_H && fx->x >= 0 && fx->x < BOARD_W)
                draw_fx_cell(sy + 1 + fx->y,
                             sx + 1 + fx->x * CELL_W,
                             COLOR_SHIELD_FX, "<>");
        } else if (fx->type == EFFECT_GUN_FIRE) {
            if (fx->y >= 0 && fx->y < BOARD_H && fx->x >= 0 && fx->x < BOARD_W)
                draw_fx_cell(sy + 1 + fx->y,
                             sx + 1 + fx->x * CELL_W,
                             COLOR_GUN_FX, "||");
        } else if (fx->type == EFFECT_GUN_HIT) {
            int y = (fx->y < 0) ? sy : sy + 1 + fx->y;
            int x = sx + 1 + fx->x * CELL_W;
            if (fx->x >= 0 && fx->x < BOARD_W)
                draw_fx_cell(y, x, COLOR_HIT_FX, "!!");
        }
    }
}

static void draw_particles(int sy, int sx) {
    const GameState *st = g_render_state;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int count = st->num_particles;
    if (count < 0) count = 0;
    if (count > MAX_PARTICLES) count = MAX_PARTICLES;

    for (int i = 0; i < count; i++) {
        const VisualParticle *p = &st->particles[i];
        int x = sx + 1 + (int)(p->x * CELL_W);
        int y = sy + 1 + (int)(p->y);
        if (y < 0 || y >= max_y || x < 0 || x + 1 >= max_x)
            continue;
        int attr = COLOR_PAIR(p->color);
        float ratio = p->max_life > 0 ? (float)p->life / p->max_life : 0.0f;
        if (p->bold && ratio > 0.3f)
            attr |= A_BOLD;
        if (ratio < 0.3f)
            attr |= A_DIM;
        attron(attr);
        mvprintw(y, x, "%s", p->ch);
        attroff(attr);
    }
}

#define HEART_ROWS 3
#define HEART_COLS 3
static const int HEART_PATTERN[HEART_ROWS][HEART_COLS] = {
    {1, 0, 1},
    {1, 1, 1},
    {0, 1, 0},
};

static void draw_big_heart(int y, int x, int alive) {
    int color = alive ? 32 : 33;
    for (int r = 0; r < HEART_ROWS; r++) {
        for (int c = 0; c < HEART_COLS; c++) {
            if (!HEART_PATTERN[r][c])
                continue;
            attron(COLOR_PAIR(color));
            mvprintw(y + r, x + c * CELL_W, "  ");
            attroff(COLOR_PAIR(color));
        }
    }
}

static void draw_hp_hearts(int y, int x, int hp, int max_hp) {
    int spacing = 4;
    if (x < 0) x = 0;
    for (int i = 0; i < max_hp; i++)
        draw_big_heart(y + i * spacing, x, i < hp);
}

#define INVADER_W 6
#define INVADER_H 4

static const char INVADER_F1[INVADER_H][INVADER_W + 1] = {
    ".#..#.",
    ".####.",
    "#.##.#",
    ".#..#.",
};

static const char INVADER_F2[INVADER_H][INVADER_W + 1] = {
    ".#..#.",
    ".####.",
    "#.##.#",
    "#....#",
};

static void draw_invader(int top_y, int left_x, int frame, int stunned, int hit) {
    const GameState *st = g_render_state;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    const char (*sprite)[INVADER_W + 1] =
        (frame % 2 == 0) ? INVADER_F1 : INVADER_F2;

    for (int r = 0; r < INVADER_H; r++) {
        for (int c = 0; c < INVADER_W; c++) {
            if (sprite[r][c] != '#')
                continue;
            if (stunned && (st->attacker_stun_timer / 3) % 2 == 0)
                continue;

            int cp = COLOR_PIECE_S;
            if (st->harddrop_cooldown_timer > 0) {
                int elapsed = HARD_DROP_COOLDOWN_TICKS - st->harddrop_cooldown_timer;
                if (elapsed < 0) elapsed = 0;
                if (elapsed > HARD_DROP_COOLDOWN_TICKS)
                    elapsed = HARD_DROP_COOLDOWN_TICKS;
                int restored_rows = (elapsed * INVADER_H) / HARD_DROP_COOLDOWN_TICKS;
                if (r < INVADER_H - restored_rows)
                    cp = 31;
            }
            if (hit)
                cp = COLOR_PIECE_Z;

            int y = top_y + r;
            int x = left_x + c * CELL_W;
            if (y < 0 || y >= max_y || x < 0 || x + 1 >= max_x)
                continue;

            attron(COLOR_PAIR(cp));
            mvprintw(y, x, "  ");
            attroff(COLOR_PAIR(cp));
        }
    }
}

static void draw_shield_bubble(int sy, int sx, const Defender *defender) {
    int timer = 0;
    if (defender->shield_timer > 0)
        timer = defender->shield_timer;
    else if (defender->stun_timer == 0 && defender->stun_invuln_timer > 0)
        timer = defender->stun_invuln_timer;
    if (timer <= 0 || defender->stun_timer > 0)
        return;

    int cy = sy + 1 + defender->y;
    int cx = sx + 1 + defender->x * CELL_W;
    int attr = COLOR_PAIR(COLOR_SHIELD_FX) | A_BOLD;
    if (timer % 4 < 2)
        attr |= A_REVERSE;

    attron(attr);
    if (cy - 1 > sy)
        mvprintw(cy - 1, cx - 1, "/--\\");
    mvprintw(cy, cx - 1, "|");
    mvprintw(cy, cx + 2, "|");
    if (cy + 1 < sy + BOARD_H + 1)
        mvprintw(cy + 1, cx - 1, "\\--/");
    attroff(attr);
}

static void draw_bomb_overlay(const GameState *st, int max_y, int max_x) {
    if (st->bomb_flash_timer <= 0)
        return;

    static const char bomb_bmp[5][7][6] = {
        {"1111.","1...1","1...1","1111.","1...1","1...1","1111."},
        {".111.","1...1","1...1","1...1","1...1","1...1",".111."},
        {"1...1","11.11","1.1.1","1...1","1...1","1...1","1...1"},
        {"1111.","1...1","1...1","1111.","1...1","1...1","1111."},
        {"..1..","..1..","..1..","..1..","..1..",".....","..1.."},
    };

    int letter_w = 5, letter_h = 7, letters = 5, gap = 1;
    int total_w = letters * letter_w + (letters - 1) * gap;
    int pw = (max_x * 85 / 100) / total_w;
    int ph = (max_y * 75 / 100) / letter_h;
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    int text_w = total_w * pw;
    int text_h = letter_h * ph;
    int ox = (max_x - text_w) / 2;
    int oy = (max_y - text_h) / 2;

    int attr;
    if (st->bomb_flash_timer >= 18)
        attr = COLOR_PAIR(COLOR_PIECE_L) | A_BOLD;
    else if (st->bomb_flash_timer > 5)
        attr = COLOR_PAIR(COLOR_BOMB_FLASH) |
            (((st->bomb_flash_timer / 3) % 2 == 0) ? A_BOLD : 0);
    else
        attr = COLOR_PAIR(COLOR_BOMB_FLASH) | A_DIM;

    char fill[64];
    int fill_len = pw;
    if (fill_len > 63) fill_len = 63;
    memset(fill, ' ', fill_len);
    fill[fill_len] = '\0';

    attron(attr);
    for (int li = 0; li < letters; li++) {
        int letter_x = ox + li * (letter_w + gap) * pw;
        for (int br = 0; br < letter_h; br++) {
            for (int bc = 0; bc < letter_w; bc++) {
                if (bomb_bmp[li][br][bc] != '1')
                    continue;
                int px = letter_x + bc * pw;
                for (int dy = 0; dy < ph; dy++) {
                    int py = oy + br * ph + dy;
                    if (py >= 0 && py < max_y && px >= 0 && px + fill_len <= max_x)
                        mvprintw(py, px, "%s", fill);
                }
            }
        }
    }
    attroff(attr);
}

static int g_pac_anim = 0;
static int g_invader_anim = 0;
static int g_score_animated = 0;
static int g_gameover_score = 0;

#define GAME_OVER_BOX_W 37
#define GAME_OVER_BOX_H 9

static void draw_game_over_text(int top, int left, int row,
                                const char *text, int attr) {
    int inner_w = GAME_OVER_BOX_W - 2;
    int len = (int)strlen(text);
    if (len > inner_w) len = inner_w;
    int x = left + 1 + (inner_w - len) / 2;
    attron(attr);
    mvprintw(top + row, x, "%.*s", inner_w, text);
    attroff(attr);
}

static void draw_game_over_panel(int rows, int cols, const char *winner,
                                 int score, int highscore,
                                 int new_highscore, int show_score) {
    int top = (rows - GAME_OVER_BOX_H) / 2;
    int left = (cols - GAME_OVER_BOX_W) / 2;

    attron(COLOR_PAIR(COLOR_BG));
    for (int row = 0; row < GAME_OVER_BOX_H; row++)
        mvhline(top + row, left, ' ', GAME_OVER_BOX_W);
    attroff(COLOR_PAIR(COLOR_BG));

    int border_attr = COLOR_PAIR(21) | A_BOLD;
    attron(border_attr);
    mvaddch(top, left, '+');
    mvhline(top, left + 1, '-', GAME_OVER_BOX_W - 2);
    mvaddch(top, left + GAME_OVER_BOX_W - 1, '+');
    mvvline(top + 1, left, '|', GAME_OVER_BOX_H - 2);
    mvvline(top + 1, left + GAME_OVER_BOX_W - 1, '|', GAME_OVER_BOX_H - 2);
    mvaddch(top + GAME_OVER_BOX_H - 1, left, '+');
    mvhline(top + GAME_OVER_BOX_H - 1, left + 1, '-', GAME_OVER_BOX_W - 2);
    mvaddch(top + GAME_OVER_BOX_H - 1,
            left + GAME_OVER_BOX_W - 1, '+');
    attroff(border_attr);

    draw_game_over_text(top, left, 1, "GAME OVER", border_attr);
    draw_game_over_text(top, left, 3, winner,
                        COLOR_PAIR(COLOR_BORDER) | A_BOLD);
    if (show_score) {
        char score_text[32];
        snprintf(score_text, sizeof(score_text), "SCORE  %d", score);
        draw_game_over_text(top, left, 4, score_text,
                            COLOR_PAIR(COLOR_BORDER) | A_BOLD);
    }

    char record_text[32];
    if (new_highscore)
        snprintf(record_text, sizeof(record_text), "NEW HIGH SCORE!");
    else
        snprintf(record_text, sizeof(record_text), "HIGH SCORE  %d", highscore);
    draw_game_over_text(top, left, 5, record_text,
                        COLOR_PAIR(new_highscore ? COLOR_COMBO : COLOR_BORDER) |
                        A_BOLD);
    draw_game_over_text(top, left, 7, "[R] Restart     [Q] Quit",
                        COLOR_PAIR(COLOR_BORDER));
}

static void render(void) {
    pthread_mutex_lock(&g_lock);
    GameState st = g_state;
    pthread_mutex_unlock(&g_lock);
    g_render_state = &st;

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int board_h = BOARD_H + 2;
    int board_w = BOARD_W * CELL_W + 2;
    int start_y = (max_y - board_h) / 2;
    int start_x = (max_x - board_w) / 2 - 8;
    if (start_y < 6) start_y = 6;
    if (start_x < 0) start_x = 0;

    if (st.shake_timer > 0 && st.shake_intensity > 0) {
        int span = st.shake_intensity * 2 + 1;
        start_y += (rand() % span) - st.shake_intensity;
        start_x += (rand() % span) - st.shake_intensity;
    }

    erase();

    if (!st.game_over) {
        g_score_animated = 0;
        g_gameover_score = 0;
    }

    {
        int title_y = start_y / 2 + 3;
        int title_x = (st.attacker_hp <= 0) ? start_x - 8 : start_x - 10;
        int heart_x = start_x - 9;
        int heart_y = start_y / 2 + 5;
        if (title_x < 0) title_x = 0;
        if (heart_x < 0) heart_x = 0;
        attron(COLOR_PAIR(33) | A_BOLD);
        if (st.attacker_hp <= 0) mvprintw(title_y, title_x, "<DEAD>");
        else mvprintw(title_y, title_x, "<Atk. HP>");
        attroff(COLOR_PAIR(33) | A_BOLD);
        draw_hp_hearts(heart_y, heart_x, st.attacker_hp, 5);
    }

    attron(COLOR_PAIR(COLOR_BORDER));
    for (int r = 0; r < board_h; r++) {
        mvprintw(start_y + r, start_x, "|");
        mvprintw(start_y + r, start_x + board_w - 1, "|");
    }
    for (int c = 0; c < board_w; c++) {
        mvprintw(start_y, start_x + c, "-");
        mvprintw(start_y + board_h - 1, start_x + c, "-");
    }
    mvprintw(start_y, start_x, "+");
    mvprintw(start_y, start_x + board_w - 1, "+");
    mvprintw(start_y + board_h - 1, start_x, "+");
    mvprintw(start_y + board_h - 1, start_x + board_w - 1, "+");
    attroff(COLOR_PAIR(COLOR_BORDER));

    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            int sy = start_y + 1 + r;
            int sx = start_x + 1 + c * CELL_W;
            if (r == st.defender.drill_target_y && c == st.defender.drill_target_x &&
                st.defender.drill_crack_timer > 0) {
                int phase = ((st.defender.drill_crack_timer - 1) * 4) / 8 + 1;
                if (phase < 1) phase = 1;
                if (phase > 4) phase = 4;
                attron(COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
                mvprintw(sy, sx, "%d ", phase);
                attroff(COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
            } else if (is_bomb_preview(r, c)) {
                draw_cell(sy, sx, st.board[r][c], 0, 0);
                draw_bomb_prev(sy, sx, st.board[r][c]);
            } else {
                draw_cell(sy, sx, st.board[r][c], 0, 0);
            }
        }
    }

    if (st.lock_flash_timer > 0) {
        int attr = COLOR_PAIR(COLOR_PIECE_L) | A_BOLD;
        if (st.lock_flash_timer <= 2) attr |= A_DIM;
        attron(attr);
        for (int i = 0; i < 4; i++) {
            int r = st.lock_cells[i][0], c = st.lock_cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W)
                mvprintw(start_y + 1 + r, start_x + 1 + c * CELL_W, "[]");
        }
        attroff(attr);
    }

    if (st.piece_type >= 1 && st.piece_type <= 7 && !st.game_over) {
        int gr = ghost_row();
        const Shape *s = &SHAPES[st.piece_type][st.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = gr + s->cells[i][0], c = st.piece_c + s->cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W &&
                st.board[r][c] == 0)
                draw_cell(start_y + 1 + r,
                          start_x + 1 + c * CELL_W, 0, 1, 0);
        }
    }

    if (st.piece_type >= 1 && st.piece_type <= 7 && !st.game_over) {
        const Shape *s = &SHAPES[st.piece_type][st.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = st.piece_r + s->cells[i][0];
            int c = st.piece_c + s->cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W) {
                int item = (i == st.piece_item_idx) ? st.piece_item_type : 0;
                draw_cell(start_y + 1 + r,
                          start_x + 1 + c * CELL_W, st.piece_type, 0, item);
            }
        }
    }

    {
        int cr = st.defender.y, cc = st.defender.x;
        if (cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W) {
            int cy = start_y + 1 + cr;
            int cx = start_x + 1 + cc * CELL_W;
            g_pac_anim++;
            int mouth = (g_pac_anim / 6) % 2;

            if (st.defender.stun_timer > 0) {
                attron(COLOR_PAIR(COLOR_DEFENDER_STUN) | A_BOLD);
                mvprintw(cy, cx, (st.defender.stun_timer % 4 < 2) ? "XX" : "xx");
                attroff(COLOR_PAIR(COLOR_DEFENDER_STUN) | A_BOLD);
            } else if (st.defender.carrying) {
                attron(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
                if (st.defender.facing == 1) mvprintw(cy, cx, "o]");
                else if (st.defender.facing == -1) mvprintw(cy, cx, "[o");
                else mvprintw(cy, cx, "oo");
                attroff(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
            } else if (st.defender.drill_timer > 0) {
                attron(COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
                if (st.defender.facing == 1) mvprintw(cy, cx, ">>");
                else if (st.defender.facing == -1) mvprintw(cy, cx, "<<");
                else mvprintw(cy, cx, "vv");
                attroff(COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
            } else {
                attron(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
                if (mouth) {
                    if (st.defender.facing == 1) mvprintw(cy, cx, "o>");
                    else if (st.defender.facing == -1) mvprintw(cy, cx, "<o");
                    else mvprintw(cy, cx, "oo");
                } else {
                    mvprintw(cy, cx, "oo");
                }
                attroff(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
            }
            draw_shield_bubble(start_y, start_x, &st.defender);
        }
    }

    attron(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
    for (int i = 0; i < st.num_bullets; i++) {
        int r = st.bullets[i][1], c = st.bullets[i][0];
        if (r >= -4 && r < BOARD_H && c >= 0 && c < BOARD_W)
            mvprintw(start_y + 1 + r, start_x + 1 + c * CELL_W, "^^");
    }
    attroff(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);

    if (st.attacker_hp > 0) {
        g_invader_anim++;
        draw_invader(start_y - INVADER_H - 1,
                     start_x + 1 + (st.piece_c - 2) * CELL_W,
                     g_invader_anim / 15,
                     st.attacker_stun_timer > 0,
                     st.boss_hit_timer > 0);
    }

    draw_effects(start_y, start_x);
    draw_particles(start_y, start_x);

    int panel_x = start_x + board_w + 4;
    int panel_y = start_y / 3 - 1;
    if (panel_y < 0) panel_y = 0;

    attron(A_BOLD);
    mvprintw(panel_y++, panel_x, "STACK AND BREAK");
    attroff(A_BOLD);
    mvprintw(panel_y++, panel_x, "Level: %d", st.level);
    mvprintw(panel_y++, panel_x, "Lines: %d", st.lines);
    mvprintw(panel_y++, panel_x, "High : %d", st.highscore);
    panel_y++;

    if (st.combo_timer > 0 && st.combo > 1) {
        attron(COLOR_PAIR(COLOR_COMBO) | A_BOLD);
        mvprintw(panel_y++, panel_x, "x%d COMBO!", st.combo);
        attroff(COLOR_PAIR(COLOR_COMBO) | A_BOLD);
    } else {
        panel_y++;
    }

    {
        const char *item_names[] = {"", "Bomb", "Drill", "Shield", "Gun"};
        if (st.next_item_idx >= 0 && st.next_item_type >= 1 && st.next_item_type <= 4)
            mvprintw(panel_y++, panel_x, "Next: [%s]", item_names[st.next_item_type]);
        else
            mvprintw(panel_y++, panel_x, "Next:");
    }
    if (st.next_type >= 1 && st.next_type <= 7) {
        const Shape *s = &SHAPES[st.next_type][0];
        for (int i = 0; i < 4; i++) {
            int item = (i == st.next_item_idx) ? st.next_item_type : 0;
            draw_cell(panel_y + 1 + s->cells[i][0],
                      panel_x + 2 + s->cells[i][1] * CELL_W,
                      st.next_type, 0, item);
        }
    }
    panel_y += 4;

    mvprintw(panel_y++, panel_x, "--- Attacker ---");
    if (st.attacker_stun_timer > 0) {
        attron(COLOR_PAIR(COLOR_DEFENDER_STUN) | A_BOLD);
        mvprintw(panel_y++, panel_x, "STUNNED! %.1fs",
                 ticks_to_sec(st.attacker_stun_timer));
        attroff(COLOR_PAIR(COLOR_DEFENDER_STUN) | A_BOLD);
    } else {
        mvprintw(panel_y++, panel_x, "Status: OK");
    }
    if (st.harddrop_cooldown_timer > 0) {
        attron(COLOR_PAIR(COLOR_PIECE_Z) | A_BOLD);
        mvprintw(panel_y++, panel_x, "Harddrop CD: %.1fs",
                 ticks_to_sec(st.harddrop_cooldown_timer));
        attroff(COLOR_PAIR(COLOR_PIECE_Z) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_PIECE_S) | A_BOLD);
        mvprintw(panel_y++, panel_x, "Harddrop: READY");
        attroff(COLOR_PAIR(COLOR_PIECE_S) | A_BOLD);
    }
    mvprintw(panel_y++, panel_x, "Score: %d", st.atkscore);
    panel_y++;

    mvprintw(panel_y++, panel_x, "--- Defender ---");
    if (st.defender.stun_timer > 0)
        mvprintw(panel_y++, panel_x, "STUNNED! %.1fs",
                 ticks_to_sec(st.defender.stun_timer));
    else if (st.defender.stun_invuln_timer > 0)
        mvprintw(panel_y++, panel_x, "INVULN! %.1fs",
                 ticks_to_sec(st.defender.stun_invuln_timer));
    else
        mvprintw(panel_y++, panel_x, "Status: OK");

    {
        const char *items[] = {"", "💣 Bomb", "⛏️  Drill", "🛡  Shield", "🔫 Gun"};
        const char *empty = "  Empty";
        int slot_w = 11;
        for (int slot = 0; slot < 3; slot++) {
            int inv = (slot < st.defender.inv_count) ? st.defender.inventory[slot] : 0;
            const char *label = (inv > 0 && inv <= 4) ? items[inv] : empty;
            mvprintw(panel_y, panel_x + slot * slot_w, "[");
            mvprintw(panel_y, panel_x + slot * slot_w + 1, "%s", label);
            mvprintw(panel_y, panel_x + (slot + 1) * slot_w - 1, "]");
        }
        panel_y++;
    }

    if (st.defender.shield_timer > 0) {
        attron(COLOR_PAIR(COLOR_SHIELD_FX) | A_BOLD);
        mvprintw(panel_y++, panel_x, "[ SHIELD %.1fs ]",
                 ticks_to_sec(st.defender.shield_timer));
        attroff(COLOR_PAIR(COLOR_SHIELD_FX) | A_BOLD);
    }
    if (st.defender.drill_timer > 0) {
        attron(COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
        mvprintw(panel_y++, panel_x, "[ DRILL  %.1fs ]",
                 ticks_to_sec(st.defender.drill_timer));
        attroff(COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
    }
    mvprintw(panel_y++, panel_x, "Carry: %s", st.defender.carrying ? "Block" : "None");
    mvprintw(panel_y++, panel_x, "Score: %d", st.defscore);
    panel_y++;

    if (g_role == 0) {
        attron(COLOR_PAIR(COLOR_PIECE_I) | A_BOLD);
        mvprintw(panel_y++, panel_x, "You: ATTACKER");
        attroff(COLOR_PAIR(COLOR_PIECE_I) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
        mvprintw(panel_y++, panel_x, "You: DEFENDER");
        attroff(COLOR_PAIR(COLOR_DEFENDER) | A_BOLD);
    }
    panel_y++;

    attron(A_BOLD);
    mvprintw(panel_y++, panel_x, "--- Controls ---");
    attroff(A_BOLD);
    if (g_role == 0) {
        mvprintw(panel_y++, panel_x, "A/D : Move L/R");
        mvprintw(panel_y++, panel_x, "W   : Rotate");
        mvprintw(panel_y++, panel_x, "S   : Soft Drop");
        mvprintw(panel_y++, panel_x, "Space: Hard Drop");
    } else {
        mvprintw(panel_y++, panel_x, "Arrows: Move");
        mvprintw(panel_y++, panel_x, "Z   : Jump");
        mvprintw(panel_y++, panel_x, "X   : Pick/Place");
        mvprintw(panel_y++, panel_x, "C   : Use Item");
    }
    mvprintw(panel_y++, panel_x, "P=Pause R=Restart Q=Quit");
    panel_y++;

    if (st.popup_timer > 0) {
        int pop_y = start_y + BOARD_H / 2 - (24 - st.popup_timer) / 3;
        int pop_x = start_x + board_w / 2 - 3;
        if (st.popup_score > 0) {
            attron(COLOR_PAIR(COLOR_PIECE_O) | A_BOLD);
            mvprintw(pop_y, pop_x, "+%d", st.popup_score);
            attroff(COLOR_PAIR(COLOR_PIECE_O) | A_BOLD);
        } else {
            attron(COLOR_PAIR(COLOR_PIECE_Z) | A_BOLD);
            mvprintw(pop_y, pop_x, "-%d", -st.popup_score);
            attroff(COLOR_PAIR(COLOR_PIECE_Z) | A_BOLD);
        }
    }

    if (!st.game_started) {
        int cy = max_y / 2;
        int cx = max_x / 2 - 12;
        attron(A_BOLD | A_BLINK);
        mvprintw(cy, cx, "Waiting for other player...");
        attroff(A_BOLD | A_BLINK);
    }

    if (st.paused) {
        int cy = start_y * 2;
        int cx = start_x;
        attron(A_BOLD | COLOR_PAIR(31));
        mvprintw(cy,     cx, "        PAUSED       ");
        mvprintw(cy + 1, cx, " Press 'P' to Resume ");
        mvprintw(cy + 2, cx, " Retry='R', Quit='Q' ");
        attroff(A_BOLD | COLOR_PAIR(31));
    }

    if (st.game_over) {
        const char *winner = st.attacker_hp <= 0
            ? "DEFENDER WINS!" : "ATTACKER WINS!";
        if (!g_score_animated) {
            int final_score = calculate_final_score(st.attacker_hp,
                                                    st.defscore,
                                                    st.atkscore);
            int display_score = st.attacker_hp <= 0
                ? st.defscore : st.atkscore;
            if (display_score <= 0) display_score = 10;

            draw_game_over_panel(max_y, max_x, winner, display_score,
                                 st.highscore, st.new_highscore, 1);
            refresh();
            usleep(1000000);
            while (display_score < final_score) {
                display_score = display_score > final_score / 2
                    ? final_score : display_score * 2;
                draw_game_over_panel(max_y, max_x, winner, display_score,
                                     st.highscore, st.new_highscore, 1);
                refresh();
                if (display_score < final_score) usleep(500000);
            }

            draw_game_over_panel(max_y, max_x, winner, display_score,
                                 st.highscore, st.new_highscore, 1);
            refresh();
            usleep(500000);

            for (int blink = 0; blink < 3; blink++) {
                draw_game_over_panel(max_y, max_x, winner, display_score,
                                     st.highscore, st.new_highscore, 0);
                refresh();
                usleep(100000);
                draw_game_over_panel(max_y, max_x, winner, display_score,
                                     st.highscore, st.new_highscore, 1);
                refresh();
                usleep(250000);
            }

            g_gameover_score = display_score;
            g_score_animated = 1;
        }
        draw_game_over_panel(max_y, max_x, winner,
                             g_score_animated ? g_gameover_score : st.score,
                             st.highscore, st.new_highscore, 1);
    }

    draw_bomb_overlay(&st, max_y, max_x);
    refresh();
}

static void handle_input(void) {
    int ch = getch();
    if (ch == ERR) return;

    int key = KEY_NONE;

    if (ch == 'q' || ch == 'Q') {
        key = K_QUIT;
        g_running = 0;
    } else if (ch == 'r' || ch == 'R') {
        key = K_RESTART;
    } else if (ch == 'p' || ch == 'P') {
        key = K_PAUSE;
    } else if (g_role == 0) {
        /* Attacker player: WASD + Space */
        switch (ch) {
        case 'a': case 'A': key = K_LEFT;      break;
        case 'd': case 'D': key = K_RIGHT;     break;
        case 'w': case 'W': key = K_ROTATE;    break;
        case 's': case 'S': key = K_SOFT_DROP;  break;
        case ' ':           key = K_HARD_DROP;  break;
        }
    } else {
        /* Defender player: Arrow keys + Z/X */
        switch (ch) {
        case KEY_LEFT:  key = K_DEFENDER_LEFT;   break;
        case KEY_RIGHT: key = K_DEFENDER_RIGHT;  break;
        case KEY_UP:    key = K_DEFENDER_JUMP;   break;
        case KEY_DOWN:  key = K_DEFENDER_DOWN;   break;
        case 'z': case 'Z': key = K_DEFENDER_JUMP;   break;
        case 'x': case 'X': key = K_DEFENDER_PICKUP; break;
        case 'c': case 'C': key = K_DEFENDER_ITEM;   break;
        }
    }

    if (key != KEY_NONE)
        send_key(key);
}

/* Main */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server_ip> [port]\n", argv[0]);
        return 1;
    }

    const char *host = argv[1];
    int port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;

    /* signal setup */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    /* connect to server */
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(g_sock);
        return 1;
    }

    printf("Connecting to %s:%d...\n", host, port);
    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(g_sock);
        return 1;
    }

    /* receive role and leaderboard */
    MsgWelcome welcome;
    if (recv_all(g_sock, &welcome, sizeof(welcome)) < 0 ||
        welcome.type != MSG_WELCOME) {
        fprintf(stderr, "Failed to receive welcome data\n");
        close(g_sock);
        return 1;
    }
    g_role = welcome.role;
    printf("Connected! Your role: %s\n",
           g_role == 0 ? "ATTACKER Player (WASD + Space)" :
                         "DEFENDER Player (Arrows + Z/X)");

    printf("\033[8;35;80t");
    fflush(stdout);
    usleep(50000);

    /* init ncurses */
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);  /* non-blocking getch */
    init_colors();

    /* check terminal size */
    int rows, cols;
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
        if (rows < 35 || cols < 80) {
		ws.ws_row = 35;
		ws.ws_col = 80;
		if (ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws) == 0) {
			usleep(100000);
			endwin();
			refresh();

			rows = 35;
			cols = 80;
		} else {
	            endwin();
	            fprintf(stderr,
	                "Terminal too small! Need at least 35x80, got %dx%d\n",
        	        cols, rows);
	            close(g_sock);
	            return 1;
		}
        }
    }

    char player_name[MAX_NAME_LEN] = {0};
    show_start_screen(&welcome.rankings, player_name);

    if (!g_running) {
        endwin();
        close(g_sock);
        return 0;
    }

    MsgPlayerName name_msg;
    memset(&name_msg, 0, sizeof(name_msg));
    name_msg.type = MSG_PLAYER_NAME;
    snprintf(name_msg.name, sizeof(name_msg.name), "%s", player_name);
    if (send_all(g_sock, &name_msg, sizeof(name_msg)) < 0) {
        endwin();
        close(g_sock);
        fprintf(stderr, "Failed to send player name\n");
        return 1;
    }

    show_lobby_waiting_screen(player_name);

    int start_msg;
    if (recv_all(g_sock, &start_msg, sizeof(start_msg)) < 0 ||
        start_msg != MSG_START) {
        endwin();
        close(g_sock);
        fprintf(stderr, "Failed to receive game start signal\n");
        return 1;
    }
    clear();
    refresh();

    /* start network receiver thread */
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, net_receiver, NULL);

    memset(&g_state, 0, sizeof(g_state));

    /* Main Loop */
    while (g_running) {
        handle_input();
        render();
        usleep(TICK_US);
    }

    /* cleanup */
    endwin();
    close(g_sock);
    pthread_join(net_thread, NULL);

    printf("Game ended. Thanks for playing!\n");
    return 0;
}
