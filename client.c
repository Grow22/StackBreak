/*
 * client.c - Tetris Co-op Game Client
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

/* ──────────── Globals ──────────── */
static int       g_sock = -1;
static int       g_role = -1;  /* 0=tetris, 1=character */
static GameState g_state;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_running = 1;

/* ──────────── Colors ──────────── */
#define COLOR_PIECE_I   1
#define COLOR_PIECE_O   2
#define COLOR_PIECE_T   3
#define COLOR_PIECE_S   4
#define COLOR_PIECE_Z   5
#define COLOR_PIECE_J   6
#define COLOR_PIECE_L   7
#define COLOR_CHAR      8
#define COLOR_CHAR_STUN 9
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

static void init_colors(void) {
    start_color();
    /* use_default_colors() removed to prevent WSL blank screen bugs */
    init_pair(COLOR_PIECE_I,   COLOR_WHITE,   COLOR_CYAN);
    init_pair(COLOR_PIECE_O,   COLOR_WHITE,   COLOR_YELLOW);
    init_pair(COLOR_PIECE_T,   COLOR_WHITE,   COLOR_MAGENTA);
    init_pair(COLOR_PIECE_S,   COLOR_WHITE,   COLOR_GREEN);
    init_pair(COLOR_PIECE_Z,   COLOR_WHITE,   COLOR_RED);
    init_pair(COLOR_PIECE_J,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(COLOR_PIECE_L,   COLOR_BLACK,   COLOR_WHITE);
    init_pair(COLOR_CHAR,      COLOR_BLACK,   COLOR_GREEN);
    init_pair(COLOR_CHAR_STUN, COLOR_WHITE,   COLOR_RED);
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
}

static int piece_color(int type) {
    if (type >= 1 && type <= 7) return type;
    return COLOR_BG;
}

static double ticks_to_seconds(int ticks) {
    return ticks / 30.0;
}

/* ──────────── Signal Handler ──────────── */
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ──────────── Network Receiver Thread ──────────── */
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

/* ──────────── Send Input ──────────── */
static void send_key(int key) {
    MsgInput msg;
    msg.type = MSG_INPUT;
    msg.key  = key;
    send_all(g_sock, &msg, sizeof(MsgInput));
}

/* ──────────── Ghost Piece (drop preview) ──────────── */
static int ghost_row(const GameState *st) {
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

/* ──────────── Rendering ──────────── */
#define CELL_W 2  /* each cell is 2 chars wide */

static void draw_cell(WINDOW *win, int y, int x, int type, int is_ghost, int item_type) {
    if (is_ghost) {
        wattron(win, COLOR_PAIR(COLOR_GHOST) | A_DIM);
        mvwprintw(win, y, x, "[]");
        wattroff(win, COLOR_PAIR(COLOR_GHOST) | A_DIM);
    } else {
        int color_type = type % 10;
        int stored_item = type / 10;
        if (item_type == 0) item_type = stored_item;
        
        if (color_type >= 1 && color_type <= 7) {
            wattron(win, COLOR_PAIR(piece_color(color_type)));
            if (item_type == 1) mvwprintw(win, y, x, "B ");      /* Bomb */
            else if (item_type == 2) mvwprintw(win, y, x, "D "); /* Drill */
            else if (item_type == 3) mvwprintw(win, y, x, "S "); /* Shield */
            else if (item_type == 4) mvwprintw(win, y, x, "G "); /* Gun */
            else mvwprintw(win, y, x, "  ");
            wattroff(win, COLOR_PAIR(piece_color(color_type)));
        } else {
            mvwprintw(win, y, x, "  ");
        }
    }
}

static void draw_bomb_preview_cell(WINDOW *win, int y, int x, int type) {
    if (type == 0) {
        wattron(win, COLOR_PAIR(COLOR_BOMB_PREVIEW));
        mvwprintw(win, y, x, "..");
        wattroff(win, COLOR_PAIR(COLOR_BOMB_PREVIEW));
    } else {
        int item_type = type / 10;

        wattron(win, COLOR_PAIR(COLOR_BOMB_BLOCK_PREVIEW) | A_BOLD);
        if (item_type == 1) mvwprintw(win, y, x, "B ");
        else if (item_type == 2) mvwprintw(win, y, x, "D ");
        else if (item_type == 3) mvwprintw(win, y, x, "S ");
        else if (item_type == 4) mvwprintw(win, y, x, "G ");
        else mvwprintw(win, y, x, "  ");
        wattroff(win, COLOR_PAIR(COLOR_BOMB_BLOCK_PREVIEW) | A_BOLD);
    }
}

static int has_ready_bomb(const GameState *st) {
    return st->ch.inv_count > 0 && st->ch.inventory[0] == 1;
}

static int has_inventory_item(const GameState *st, int item_type) {
    for (int i = 0; i < st->ch.inv_count; i++) {
        if (st->ch.inventory[i] == item_type)
            return 1;
    }
    return 0;
}

static int is_bomb_preview_cell(const GameState *st, int r, int c) {
    if (st->game_over || !has_ready_bomb(st))
        return 0;
    if (r == st->ch.y && c == st->ch.x)
        return 0;

    int left = st->ch.x - 2;
    int top = st->ch.y - 2;

    return r >= top && r < top + 4 && c >= left && c < left + 4;
}

static void draw_effect_cell(WINDOW *win, int y, int x, int color, const char *text) {
    int max_y, max_x;
    getmaxyx(win, max_y, max_x);
    if (y < 0 || y >= max_y || x < 0 || x + 1 >= max_x)
        return;

    wattron(win, COLOR_PAIR(color) | A_BOLD);
    mvwprintw(win, y, x, "%s", text);
    wattroff(win, COLOR_PAIR(color) | A_BOLD);
}

static void draw_effects(const GameState *st, int start_y, int start_x) {
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
                    draw_effect_cell(stdscr, start_y + 1 + r,
                                     start_x + 1 + c * CELL_W,
                                     COLOR_BOMB_FLASH, text);
                }
            }
        } else if (fx->type == EFFECT_DRILL) {
            if (fx->y >= 0 && fx->y < BOARD_H && fx->x >= 0 && fx->x < BOARD_W) {
                draw_effect_cell(stdscr, start_y + 1 + fx->y,
                                 start_x + 1 + fx->x * CELL_W,
                                 COLOR_DRILL_FX, "!!");
            }
        } else if (fx->type == EFFECT_SHIELD) {
            if (fx->y >= 0 && fx->y < BOARD_H && fx->x >= 0 && fx->x < BOARD_W)
                draw_effect_cell(stdscr, start_y + 1 + fx->y,
                                 start_x + 1 + fx->x * CELL_W,
                                 COLOR_SHIELD_FX, "<>");
        } else if (fx->type == EFFECT_GUN_FIRE) {
            if (fx->y >= 0 && fx->y < BOARD_H && fx->x >= 0 && fx->x < BOARD_W)
                draw_effect_cell(stdscr, start_y + 1 + fx->y,
                                 start_x + 1 + fx->x * CELL_W,
                                 COLOR_GUN_FX, "||");
        } else if (fx->type == EFFECT_GUN_HIT) {
            int y = (fx->y < 0) ? start_y : start_y + 1 + fx->y;
            int x = start_x + 1 + fx->x * CELL_W;
            if (fx->x >= 0 && fx->x < BOARD_W)
                draw_effect_cell(stdscr, y, x, COLOR_HIT_FX, "!!");
        }
    }
}

static void render(void) {
    pthread_mutex_lock(&g_lock);
    GameState st = g_state;
    pthread_mutex_unlock(&g_lock);

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    /* board window position */
    int board_h = BOARD_H + 2;
    int board_w = BOARD_W * CELL_W + 2;
    int start_y = (max_y - board_h) / 2;
    if (start_y < 4) start_y = 4; /* Reserve top 4 lines for boss */
    int start_x = (max_x - board_w) / 2 - 8;
    if (start_x < 0) start_x = 0;

    erase();

    /* ── Draw border ── */
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

    /* ── Draw board cells ── */
    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            int sy = start_y + 1 + r;
            int sx = start_x + 1 + c * CELL_W;
            if (r == st.ch.drill_target_y && c == st.ch.drill_target_x && st.ch.drill_crack_timer > 0) {
                /* Drill Cracking Animation */
                int phase = ((st.ch.drill_crack_timer - 1) * 4) / 15 + 1;
                if (phase < 1) phase = 1;
                if (phase > 4) phase = 4;
                wattron(stdscr, COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
                mvwprintw(stdscr, sy, sx, "%d ", phase);
                wattroff(stdscr, COLOR_PAIR(COLOR_DRILL_FX) | A_BOLD);
            } else if (is_bomb_preview_cell(&st, r, c)) {
                draw_cell(stdscr, sy, sx, st.board[r][c], 0, 0);
                draw_bomb_preview_cell(stdscr, sy, sx, st.board[r][c]);
            } else {
                draw_cell(stdscr, sy, sx, st.board[r][c], 0, 0);
            }
        }
    }

    /* ── Draw ghost piece ── */
    if (st.piece_type >= 1 && st.piece_type <= 7 && !st.game_over) {
        int gr = ghost_row(&st);
        const Shape *s = &SHAPES[st.piece_type][st.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = gr + s->cells[i][0];
            int c = st.piece_c + s->cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W &&
                st.board[r][c] == 0) {
                int sy = start_y + 1 + r;
                int sx = start_x + 1 + c * CELL_W;
                draw_cell(stdscr, sy, sx, 0, 1, 0);
            }
        }
    }

    /* ── Draw falling piece ── */
    if (st.piece_type >= 1 && st.piece_type <= 7 && !st.game_over) {
        const Shape *s = &SHAPES[st.piece_type][st.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = st.piece_r + s->cells[i][0];
            int c = st.piece_c + s->cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W) {
                int sy = start_y + 1 + r;
                int sx = start_x + 1 + c * CELL_W;
                int item = (i == st.piece_item_idx) ? st.piece_item_type : 0;
                draw_cell(stdscr, sy, sx, st.piece_type, 0, item);
            }
        }
    }

    /* ── Draw character ── */
    {
        int cr = st.ch.y, cc = st.ch.x;
        if (cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W) {
            int sy = start_y + 1 + cr;
            int sx = start_x + 1 + cc * CELL_W;
            int cp = (st.ch.stun_timer > 0) ? COLOR_CHAR_STUN : COLOR_CHAR;
            if (st.ch.stun_timer == 0 && st.ch.shield_timer > 0)
                cp = COLOR_SHIELD_FX;
            else if (st.ch.stun_timer == 0 && st.ch.drill_timer > 0)
                cp = COLOR_DRILL_FX;
            attron(COLOR_PAIR(cp) | A_BOLD);
            if (st.ch.stun_timer > 0)
                mvprintw(sy, sx, "XX");
            else if (st.ch.shield_timer > 0)
                mvprintw(sy, sx, "[]");
            else if (st.ch.carrying) {
                if (st.ch.facing == -1) mvprintw(sy, sx, "+@");
                else if (st.ch.facing == 1) mvprintw(sy, sx, "@+");
                else mvprintw(sy, sx, "v@");
            } else if (st.ch.drill_timer > 0 || has_inventory_item(&st, 2)) {
                if (st.ch.facing == -1) mvprintw(sy, sx, "<D");
                else if (st.ch.facing == 1) mvprintw(sy, sx, "D>");
                else mvprintw(sy, sx, "vD");
            } else {
                if (st.ch.facing == -1) mvprintw(sy, sx, "<@");
                else if (st.ch.facing == 1) mvprintw(sy, sx, "@>");
                else mvprintw(sy, sx, "vv"); /* visually clear down aim */
            }
            attroff(COLOR_PAIR(cp) | A_BOLD);
        }
    }

    /* ── Draw Bullets ── */
    attron(COLOR_PAIR(COLOR_CHAR) | A_BOLD);
    for (int i = 0; i < st.num_bullets; i++) {
        int r = st.bullets[i][1];
        int c = st.bullets[i][0];
        int sy = start_y + 1 + r;
        int sx = start_x + 1 + c * CELL_W;
        /* Draw if within visible area (including above board for boss hitting) */
        if (r >= -4 && r < BOARD_H && c >= 0 && c < BOARD_W) {
            mvprintw(sy, sx, "^^");
        }
    }
    attroff(COLOR_PAIR(COLOR_CHAR) | A_BOLD);

    /* ── Draw Attacker Boss (2x2) ── */
    if (st.attacker_hp > 0) {
        int cp = (st.attacker_stun_timer > 0) ? COLOR_CHAR_STUN : COLOR_PIECE_Z;
        attron(COLOR_PAIR(cp) | A_BOLD);
        
        int boss_left = st.piece_c; /* Boss occupies piece_c and piece_c + 1 */
        for (int br = -2; br < 0; br++) {
            for (int bc = 0; bc < 2; bc++) {
                int sy = start_y + 1 + br;
                int sx = start_x + 1 + (boss_left + bc) * CELL_W;
                mvprintw(sy, sx, "[]");
            }
        }
        /* Face details */
        mvprintw(start_y, start_x + 1 + boss_left * CELL_W, "[@@]");
        attroff(COLOR_PAIR(cp) | A_BOLD);
    }

    draw_effects(&st, start_y, start_x);

    /* ── Side panel ── */
    int panel_x = start_x + board_w + 2;
    int panel_y = start_y + 1;

    attron(A_BOLD);
    mvprintw(panel_y, panel_x, "TETRIS CO-OP");
    attroff(A_BOLD);
    panel_y += 2;

    mvprintw(panel_y++, panel_x, "Score: %d", st.score);
    mvprintw(panel_y++, panel_x, "Level: %d", st.level);
    mvprintw(panel_y++, panel_x, "Lines: %d", st.lines);
    panel_y++;

    /* Next piece preview */
    mvprintw(panel_y++, panel_x, "Next:");
    if (st.next_type >= 1 && st.next_type <= 7) {
        const Shape *s = &SHAPES[st.next_type][0];
        for (int i = 0; i < 4; i++) {
            int r = s->cells[i][0];
            int c = s->cells[i][1];
            int sy = panel_y + 1 + r;
            int sx = panel_x + 2 + c * CELL_W;
            draw_cell(stdscr, sy, sx, st.next_type, 0, 0);
        }
    }
    panel_y += 4;

    /* Attacker status */
    mvprintw(panel_y++, panel_x, "--- Attacker ---");
    mvprintw(panel_y++, panel_x, "HP: ");
    for(int i=0; i<5; i++) {
        if(i < st.attacker_hp) printw("O ");
        else printw("X ");
    }
    if (st.attacker_stun_timer > 0) {
        attron(COLOR_PAIR(COLOR_CHAR_STUN) | A_BOLD);
        mvprintw(panel_y++, panel_x, "STUNNED! %.1fs",
                 ticks_to_seconds(st.attacker_stun_timer));
        attroff(COLOR_PAIR(COLOR_CHAR_STUN) | A_BOLD);
    } else {
        mvprintw(panel_y++, panel_x, "Status: OK");
    }
    panel_y++;

    /* Character status */
    mvprintw(panel_y++, panel_x, "--- Defender ---");
    if (st.ch.stun_timer > 0)
        mvprintw(panel_y++, panel_x, "STUNNED! %.1fs",
                 ticks_to_seconds(st.ch.stun_timer));
    else if (st.ch.stun_invuln_timer > 0)
        mvprintw(panel_y++, panel_x, "INVULN! %.1fs",
                 ticks_to_seconds(st.ch.stun_invuln_timer));
    else
        mvprintw(panel_y++, panel_x, "Status: OK");

    const char* items[] = {"", "Bomb", "Drill", "Shield", "Gun"};
    mvprintw(panel_y++, panel_x, "Items:");
    mvprintw(panel_y++, panel_x, "[%-6s] [%-6s] [%-6s]",
        st.ch.inv_count > 0 ? items[st.ch.inventory[0]] : "Empty",
        st.ch.inv_count > 1 ? items[st.ch.inventory[1]] : "Empty",
        st.ch.inv_count > 2 ? items[st.ch.inventory[2]] : "Empty");
    
    if (st.ch.shield_timer > 0) mvprintw(panel_y++, panel_x, "[ SHIELD ACTIVE ]");
    if (st.ch.drill_timer > 0) mvprintw(panel_y++, panel_x, "[ DRILL ACTIVE ]");

    if (st.ch.carrying)
        mvprintw(panel_y++, panel_x, "Carrying: Block");
    else
        mvprintw(panel_y++, panel_x, "Carrying: None");
    panel_y++;

    /* Role info */
    if (g_role == 0) {
        attron(COLOR_PAIR(COLOR_PIECE_I) | A_BOLD);
        mvprintw(panel_y++, panel_x, "You: TETRIS");
        attroff(COLOR_PAIR(COLOR_PIECE_I) | A_BOLD);
    } else {
        attron(COLOR_PAIR(COLOR_CHAR) | A_BOLD);
        mvprintw(panel_y++, panel_x, "You: CHARACTER");
        attroff(COLOR_PAIR(COLOR_CHAR) | A_BOLD);
    }
    panel_y++;

    /* Controls */
    mvprintw(panel_y++, panel_x, "--- Controls ---");
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
    mvprintw(panel_y++, panel_x, "Q   : Quit");

    /* Game status */
    if (!st.game_started) {
        int cy = max_y / 2;
        int cx = max_x / 2 - 12;
        attron(A_BOLD | A_BLINK);
        mvprintw(cy, cx, "Waiting for other player...");
        attroff(A_BOLD | A_BLINK);
    }

    if (st.game_over) {
        int cy = max_y / 2;
        int cx = max_x / 2 - 8;
        attron(A_BOLD | COLOR_PAIR(COLOR_PIECE_Z));
        mvprintw(cy - 1, cx, "                  ");
        mvprintw(cy,     cx, "   GAME  OVER !   ");
        if (st.attacker_hp <= 0)
            mvprintw(cy + 1, cx, "  DEFENDER WINS!  ");
        else
            mvprintw(cy + 1, cx, "  ATTACKER WINS!  ");
        mvprintw(cy + 2, cx, "                  ");
        attroff(A_BOLD | COLOR_PAIR(COLOR_PIECE_Z));
    }

    refresh();
}

/* ──────────── Input Handling ──────────── */
static void handle_input(void) {
    int ch = getch();
    if (ch == ERR) return;

    int key = KEY_NONE;

    if (ch == 'q' || ch == 'Q') {
        key = K_QUIT;
        g_running = 0;
    } else if (g_role == 0) {
        /* Tetris player: WASD + Space */
        switch (ch) {
        case 'a': case 'A': key = K_LEFT;      break;
        case 'd': case 'D': key = K_RIGHT;     break;
        case 'w': case 'W': key = K_ROTATE;    break;
        case 's': case 'S': key = K_SOFT_DROP;  break;
        case ' ':           key = K_HARD_DROP;  break;
        }
    } else {
        /* Character player: Arrow keys + Z/X */
        switch (ch) {
        case KEY_LEFT:  key = K_CH_LEFT;   break;
        case KEY_RIGHT: key = K_CH_RIGHT;  break;
        case KEY_UP:    key = K_CH_JUMP;   break;
        case KEY_DOWN:  key = K_CH_DOWN;   break;
        case 'z': case 'Z': key = K_CH_JUMP;   break;
        case 'x': case 'X': key = K_CH_PICKUP; break;
        case 'c': case 'C': key = K_CH_ITEM;   break;
        }
    }

    if (key != KEY_NONE)
        send_key(key);
}

/* ──────────── Main ──────────── */
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

    /* receive role assignment */
    MsgRole role_msg;
    if (recv_all(g_sock, &role_msg, sizeof(MsgRole)) < 0) {
        fprintf(stderr, "Failed to receive role\n");
        close(g_sock);
        return 1;
    }
    g_role = role_msg.role;
    printf("Connected! Your role: %s\n",
           g_role == 0 ? "TETRIS Player (WASD + Space)" :
                         "CHARACTER Player (Arrows + Z/X)");
    printf("Starting in 2 seconds...\n");
    sleep(2);

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

    // 5/21 수정 : 창 크기 자동 설정
    printf("\033[8;28;60t");
    fflush(stdout);
    usleep(50000);
    //

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
        if (rows < 28 || cols < 60) {
            endwin();
            fprintf(stderr,
                "Terminal too small! Need at least 60x28, got %dx%d\n",
                cols, rows);
            close(g_sock);
            return 1;
        }
    }

    /* start network receiver thread */
    pthread_t net_thread;
    pthread_create(&net_thread, NULL, net_receiver, NULL);

    memset(&g_state, 0, sizeof(g_state));

    /* ── Main Loop ── */
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
