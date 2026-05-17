/*
 * local.c - Tetris Co-op Local Mode (Single Player controls both)
 *
 * One player controls both Tetris (WASD + Space) and Character (Arrows + Z/X)
 * on the same keyboard. No network needed.
 *
 * System calls used: read, write, open, close, stat, ioctl,
 *   sigaction, signal, pthread_create, clock_gettime, usleep
 */

#include "common.h"
#include <ncurses.h>
#include <locale.h>

/* ──────────── Global State ──────────── */
static GameState g_state;
static volatile sig_atomic_t g_running = 1;
static int g_highscore = 0;
static int drop_counter = 0;
static int gravity_counter = 0;

#define HIGHSCORE_FILE "highscore.dat"

/* ──────────── Signal Handler ──────────── */
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ──────────── High Score ──────────── */
static void load_highscore(void) {
    struct stat st;
    if (stat(HIGHSCORE_FILE, &st) < 0) { g_highscore = 0; return; }
    int fd = open(HIGHSCORE_FILE, O_RDONLY);
    if (fd < 0) { g_highscore = 0; return; }
    if (read(fd, &g_highscore, sizeof(int)) < 0) {
        g_highscore = 0;
    }
    close(fd);
}

static void save_highscore(void) {
    if (g_state.score <= g_highscore) return;
    g_highscore = g_state.score;
    int fd = open(HIGHSCORE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, &g_highscore, sizeof(int)) < 0) {
        /* ignore error */
    }
    close(fd);
}

/* ──────────── Random Piece ──────────── */
static int random_piece(void) {
    return (rand() % 7) + 1;
}

/* ──────────── Piece Helpers ──────────── */
static int piece_cells(int type, int rot, int pr, int pc, int out[4][2]) {
    if (type < 1 || type > 7) return 0;
    const Shape *s = &SHAPES[type][rot % 4];
    for (int i = 0; i < 4; i++) {
        out[i][0] = pr + s->cells[i][0];
        out[i][1] = pc + s->cells[i][1];
    }
    return 1;
}

static int piece_valid(int type, int rot, int pr, int pc) {
    int cells[4][2];
    piece_cells(type, rot, pr, pc, cells);
    for (int i = 0; i < 4; i++) {
        int r = cells[i][0], c = cells[i][1];
        if (r < 0 || r >= BOARD_H || c < 0 || c >= BOARD_W) return 0;
        if (g_state.board[r][c] != 0) return 0;
    }
    return 1;
}

static int char_hit_by_piece(int cells[4][2]) {
    for (int i = 0; i < 4; i++)
        if (cells[i][0] == g_state.ch.y && cells[i][1] == g_state.ch.x)
            return 1;
    return 0;
}

/* ──────────── Lock & Spawn ──────────── */

/* 5/17, 다른 함수에서 apply_column_gravity 함수 호출 시 apply_column_gravity가 미정의된 문제가 발생하여 apply_column_gravity 함수 정의 위치를 위로 옮겼음.
 */

/* Apply gravity to a specific column after a block is removed.
 * Blocks above the removed position fall down to fill the gap. */
static void apply_column_gravity(int col) {
    for (int r = BOARD_H - 1; r > 0; r--) {
        if (g_state.board[r][col] == 0) {
            /* find nearest block above */
            for (int rr = r - 1; rr >= 0; rr--) {
                if (g_state.board[rr][col] != 0) {
                    g_state.board[r][col] = g_state.board[rr][col];
                    g_state.board[rr][col] = 0;
                    break;
                }
            }
        }
    }
}
static void lock_piece(void) {
    int cells[4][2];
    piece_cells(g_state.piece_type, g_state.piece_rot,
                g_state.piece_r, g_state.piece_c, cells);

    if (char_hit_by_piece(cells)) {
        g_state.ch.stun_timer = STUN_TICKS;
        for (int dx = 1; dx < BOARD_W; dx++) {
            int nx = g_state.ch.x + dx;
            if (nx < BOARD_W && g_state.board[g_state.ch.y][nx] == 0) {
                int overlap = 0;
                for (int j = 0; j < 4; j++)
                    if (cells[j][0] == g_state.ch.y && cells[j][1] == nx) overlap = 1;
                if (!overlap) { g_state.ch.x = nx; break; }
            }
            nx = g_state.ch.x - dx;
            if (nx >= 0 && g_state.board[g_state.ch.y][nx] == 0) {
                int overlap = 0;
                for (int j = 0; j < 4; j++)
                    if (cells[j][0] == g_state.ch.y && cells[j][1] == nx) overlap = 1;
                if (!overlap) { g_state.ch.x = nx; break; }
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        int r = cells[i][0], c = cells[i][1];
        if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W)
            g_state.board[r][c] = g_state.piece_type;
    }

    /* 5/17 : 멀티 모드에서처럼 빈 공간없이 위에 있는 블록이 떨어지도록 함*/
    for (int c = 0; c < BOARD_W; c++) {
	    apply_column_gravity(c);
    }
}


static int clear_lines(void) {
    int cleared = 0;
    for (int r = BOARD_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_W; c++)
            if (g_state.board[r][c] == 0) { full = 0; break; }
        if (full) {
            cleared++;
            if (g_state.ch.y == r) {
                if (r > 0) g_state.ch.y = r - 1;
            } else if (g_state.ch.y < r) {
                g_state.ch.y++;
            }
            for (int rr = r; rr > 0; rr--)
                memcpy(g_state.board[rr], g_state.board[rr-1], sizeof(int)*BOARD_W);
            memset(g_state.board[0], 0, sizeof(int)*BOARD_W);
            r++;
        }
    }
    return cleared;
}

static void add_score(int lc) {
    static const int pts[] = {0, 100, 300, 500, 800};
    if (lc > 0 && lc <= 4) g_state.score += pts[lc] * g_state.level;
    g_state.lines += lc;
    g_state.level = 1 + g_state.lines / 10;
}

static void spawn_piece(void) {
    g_state.piece_type = g_state.next_type;
    g_state.piece_rot  = 0;
    g_state.piece_r    = 0;
    g_state.piece_c    = BOARD_W / 2;
    g_state.next_type  = random_piece();
    if (!piece_valid(g_state.piece_type, g_state.piece_rot,
                     g_state.piece_r, g_state.piece_c)) {
        g_state.game_over = 1;
        save_highscore();
    }
}

/* ──────────── Character ──────────── */
static int char_on_ground(void) {
    if (g_state.ch.y >= BOARD_H - 1) return 1;
    return g_state.board[g_state.ch.y + 1][g_state.ch.x] != 0;
}

/* Character physics: jump velocity + gravity, called every physics tick */
static void character_physics(void) {
    if (g_state.ch.stun_timer > 0) return;

    if (g_state.ch.jump_vel > 0) {
        /* rising: move up if cell above is empty */
        int ny = g_state.ch.y - 1;
        if (ny >= 0 && g_state.board[ny][g_state.ch.x] == 0) {
            g_state.ch.y = ny;
            g_state.ch.jump_vel--;
        } else {
            g_state.ch.jump_vel = 0;  /* hit ceiling or block, stop rising */
        }
    } else {
        /* falling: gravity pulls down */
        int ny = g_state.ch.y + 1;
        if (ny < BOARD_H && g_state.board[ny][g_state.ch.x] == 0)
            g_state.ch.y = ny;
    }
}


/* ──────────── Init ──────────── */
static void init_game(void) {
    memset(&g_state, 0, sizeof(g_state));
    srand(time(NULL));
    g_state.piece_type = random_piece();
    g_state.piece_rot  = 0;
    g_state.piece_r    = 0;
    g_state.piece_c    = BOARD_W / 2;
    g_state.next_type  = random_piece();
    g_state.ch.x = BOARD_W / 2;
    g_state.ch.y = BOARD_H - 1;
    g_state.ch.carrying = 0;
    g_state.ch.stun_timer = 0;
    g_state.ch.facing = 1;
    g_state.ch.jump_vel = 0;
    g_state.level = 1;
    g_state.game_started = 1;
}

/* ──────────── Game Tick ──────────── */
static void game_tick(void) {
    if (g_state.game_over) return;

    if (g_state.ch.stun_timer > 0) g_state.ch.stun_timer--;

    gravity_counter++;
    if (gravity_counter >= 2) {
        gravity_counter = 0;
        character_physics();
    }

    int drop_speed = INITIAL_DROP - (g_state.level - 1) * 2;
    if (drop_speed < 3) drop_speed = 3;
    drop_counter++;
    if (drop_counter >= drop_speed) {
        drop_counter = 0;
        if (piece_valid(g_state.piece_type, g_state.piece_rot,
                        g_state.piece_r + 1, g_state.piece_c)) {
            g_state.piece_r++;
        } else {
            lock_piece();
            add_score(clear_lines());
            spawn_piece();
        }
    }

    int cells[4][2];
    piece_cells(g_state.piece_type, g_state.piece_rot,
                g_state.piece_r, g_state.piece_c, cells);
    if (char_hit_by_piece(cells) && g_state.ch.stun_timer == 0)
        g_state.ch.stun_timer = STUN_TICKS;
}

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

static void init_colors(void) {
    start_color();
    use_default_colors();
    init_pair(COLOR_PIECE_I,   COLOR_WHITE,   COLOR_CYAN);
    init_pair(COLOR_PIECE_O,   COLOR_WHITE,   COLOR_YELLOW);
    init_pair(COLOR_PIECE_T,   COLOR_WHITE,   COLOR_MAGENTA);
    init_pair(COLOR_PIECE_S,   COLOR_WHITE,   COLOR_GREEN);
    init_pair(COLOR_PIECE_Z,   COLOR_WHITE,   COLOR_RED);
    init_pair(COLOR_PIECE_J,   COLOR_WHITE,   COLOR_BLUE);
    init_pair(COLOR_PIECE_L,   COLOR_BLACK,   COLOR_WHITE);
    init_pair(COLOR_CHAR,      COLOR_BLACK,   COLOR_GREEN);
    init_pair(COLOR_CHAR_STUN, COLOR_WHITE,   COLOR_RED);
    init_pair(COLOR_BORDER,    COLOR_WHITE,   -1);
    init_pair(COLOR_GHOST,     COLOR_BLACK,   COLOR_BLACK);
}

/* ──────────── Ghost Row ──────────── */
static int ghost_row(void) {
    int gr = g_state.piece_r;
    while (1) {
        int ok = 1;
        const Shape *s = &SHAPES[g_state.piece_type][g_state.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = (gr + 1) + s->cells[i][0];
            int c = g_state.piece_c + s->cells[i][1];
            if (r >= BOARD_H || c < 0 || c >= BOARD_W) { ok = 0; break; }
            if (r >= 0 && g_state.board[r][c] != 0) { ok = 0; break; }
        }
        if (!ok) break;
        gr++;
    }
    return gr;
}

/* ──────────── Render ──────────── */
#define CELL_W 2

static void draw_cell(int y, int x, int type, int is_ghost) {
    if (is_ghost) {
        attron(A_DIM);
        mvprintw(y, x, "::");
        attroff(A_DIM);
    } else if (type >= 1 && type <= 7) {
        attron(COLOR_PAIR(type));
        mvprintw(y, x, "  ");
        attroff(COLOR_PAIR(type));
    } else {
        mvprintw(y, x, "  ");
    }
}

static void render(void) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int board_h = BOARD_H + 2;
    int board_w = BOARD_W * CELL_W + 2;
    int start_y = (max_y - board_h) / 2;
    int start_x = (max_x - board_w) / 2 - 10;
    if (start_y < 0) start_y = 0;
    if (start_x < 0) start_x = 0;

    erase();

    /* border */
    attron(COLOR_PAIR(COLOR_BORDER) | A_BOLD);
    for (int r = 0; r <= board_h - 1; r++) {
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
    attroff(COLOR_PAIR(COLOR_BORDER) | A_BOLD);

    /* board cells */
    for (int r = 0; r < BOARD_H; r++)
        for (int c = 0; c < BOARD_W; c++)
            draw_cell(start_y + 1 + r, start_x + 1 + c * CELL_W,
                      g_state.board[r][c], 0);

    /* ghost piece */
    if (g_state.piece_type >= 1 && !g_state.game_over) {
        int gr = ghost_row();
        const Shape *s = &SHAPES[g_state.piece_type][g_state.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = gr + s->cells[i][0];
            int c = g_state.piece_c + s->cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W &&
                g_state.board[r][c] == 0)
                draw_cell(start_y + 1 + r, start_x + 1 + c * CELL_W, 0, 1);
        }
    }

    /* falling piece */
    if (g_state.piece_type >= 1 && !g_state.game_over) {
        const Shape *s = &SHAPES[g_state.piece_type][g_state.piece_rot % 4];
        for (int i = 0; i < 4; i++) {
            int r = g_state.piece_r + s->cells[i][0];
            int c = g_state.piece_c + s->cells[i][1];
            if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W)
                draw_cell(start_y + 1 + r, start_x + 1 + c * CELL_W,
                          g_state.piece_type, 0);
        }
    }

    /* character */
    {
        int cr = g_state.ch.y, cc = g_state.ch.x;
        if (cr >= 0 && cr < BOARD_H && cc >= 0 && cc < BOARD_W) {
            int sy = start_y + 1 + cr;
            int sx = start_x + 1 + cc * CELL_W;
            int cp = (g_state.ch.stun_timer > 0) ? COLOR_CHAR_STUN : COLOR_CHAR;
            attron(COLOR_PAIR(cp) | A_BOLD);
            if (g_state.ch.stun_timer > 0)
                mvprintw(sy, sx, "XX");
            else if (g_state.ch.carrying)
                mvprintw(sy, sx, "@+");
            else
                mvprintw(sy, sx, "@@");
            attroff(COLOR_PAIR(cp) | A_BOLD);
        }
    }

    /* side panel */
    int px = start_x + board_w + 2;
    int py = start_y + 1;

    attron(A_BOLD);
    mvprintw(py, px, "TETRIS CO-OP");
    attroff(A_BOLD);
    py += 2;

    mvprintw(py++, px, "Score : %d", g_state.score);
    mvprintw(py++, px, "Level : %d", g_state.level);
    mvprintw(py++, px, "Lines : %d", g_state.lines);
    mvprintw(py++, px, "High  : %d", g_highscore);
    py++;

    mvprintw(py++, px, "Next:");
    if (g_state.next_type >= 1 && g_state.next_type <= 7) {
        const Shape *s = &SHAPES[g_state.next_type][0];
        for (int i = 0; i < 4; i++) {
            int r = s->cells[i][0], c = s->cells[i][1];
            draw_cell(py + 1 + r, px + 2 + c * CELL_W, g_state.next_type, 0);
        }
    }
    py += 4;

    mvprintw(py++, px, "--- Character ---");
    if (g_state.ch.stun_timer > 0)
        mvprintw(py++, px, "STUNNED! (%d)", g_state.ch.stun_timer);
    else
        mvprintw(py++, px, "Status : OK");
    mvprintw(py++, px, "Carry  : %s",
             g_state.ch.carrying ? "Block" : "None");
    py++;

    attron(A_BOLD);
    mvprintw(py++, px, "--- Controls ---");
    attroff(A_BOLD);
    mvprintw(py++, px, "[Tetris]");
    mvprintw(py++, px, " A/D   Move L/R");
    mvprintw(py++, px, " W     Rotate");
    mvprintw(py++, px, " S     Soft Drop");
    mvprintw(py++, px, " Space Hard Drop");
    py++;
    mvprintw(py++, px, "[Character]");
    mvprintw(py++, px, " Arrows Move");
    mvprintw(py++, px, " Z      Jump");
    mvprintw(py++, px, " X      Pick/Place");
    py++;
    mvprintw(py++, px, " R  Restart");
    mvprintw(py++, px, " Q  Quit");

    if (g_state.game_over) {
        int cy = max_y / 2;
        int cx = max_x / 2 - 10;
        attron(A_BOLD | COLOR_PAIR(COLOR_PIECE_Z));
        mvprintw(cy - 1, cx, "                      ");
        mvprintw(cy,     cx, "     GAME  OVER !     ");
        mvprintw(cy + 1, cx, "   Score: %-8d     ", g_state.score);
        mvprintw(cy + 2, cx, "   R=Restart  Q=Quit  ");
        mvprintw(cy + 3, cx, "                      ");
        attroff(A_BOLD | COLOR_PAIR(COLOR_PIECE_Z));
    }

    refresh();
}

/* ──────────── Input ──────────── */
static void handle_input(void) {
    int ch = getch();
    if (ch == ERR) return;

    /* common */
    if (ch == 'q' || ch == 'Q') { g_running = 0; return; }
    if (ch == 'r' || ch == 'R') {
        init_game();
        drop_counter = 0;
        gravity_counter = 0;
        return;
    }

    if (g_state.game_over) return;

    /* ── Tetris controls (WASD + Space) ── */
    switch (ch) {
    case 'a': case 'A': {
        int nc = g_state.piece_c - 1;
        if (piece_valid(g_state.piece_type, g_state.piece_rot, g_state.piece_r, nc))
            g_state.piece_c = nc;
        break;
    }
    case 'd': case 'D': {
        int nc = g_state.piece_c + 1;
        if (piece_valid(g_state.piece_type, g_state.piece_rot, g_state.piece_r, nc))
            g_state.piece_c = nc;
        break;
    }
    case 'w': case 'W': {
        int nrot = (g_state.piece_rot + 1) % 4;
        if (piece_valid(g_state.piece_type, nrot, g_state.piece_r, g_state.piece_c))
            g_state.piece_rot = nrot;
        else if (piece_valid(g_state.piece_type, nrot, g_state.piece_r, g_state.piece_c - 1)) {
            g_state.piece_rot = nrot; g_state.piece_c--;
        } else if (piece_valid(g_state.piece_type, nrot, g_state.piece_r, g_state.piece_c + 1)) {
            g_state.piece_rot = nrot; g_state.piece_c++;
        }
        break;
    }
    case 's': case 'S': {
        if (piece_valid(g_state.piece_type, g_state.piece_rot,
                        g_state.piece_r + 1, g_state.piece_c)) {
            g_state.piece_r++;
            g_state.score += 1;
        }
        break;
    }
    case ' ': {
        while (piece_valid(g_state.piece_type, g_state.piece_rot,
                           g_state.piece_r + 1, g_state.piece_c)) {
            g_state.piece_r++;
            g_state.score += 2;
        }
        lock_piece();
        add_score(clear_lines());
        spawn_piece();
        drop_counter = 0;
        break;
    }
    }

    /* ── Character controls (Arrows + Z/X) ── */
    if (g_state.ch.stun_timer > 0) return;

    switch (ch) {
    case KEY_LEFT: {
        g_state.ch.facing = -1;
        int nx = g_state.ch.x - 1;
        if (nx >= 0 && g_state.board[g_state.ch.y][nx] == 0)
            g_state.ch.x = nx;
        break;
    }
    case KEY_RIGHT: {
        g_state.ch.facing = 1;
        int nx = g_state.ch.x + 1;
        if (nx < BOARD_W && g_state.board[g_state.ch.y][nx] == 0)
            g_state.ch.x = nx;
        break;
    }
    case KEY_UP:
    case 'z': case 'Z': {
        /* velocity-based jump: press to launch, physics handles the rest */
        if (char_on_ground() && g_state.ch.jump_vel == 0) {
            g_state.ch.jump_vel = 3;  /* jump height: 3 cells */
            /* immediate first step for responsiveness */
            int ny = g_state.ch.y - 1;
            if (ny >= 0 && g_state.board[ny][g_state.ch.x] == 0) {
                g_state.ch.y = ny;
                g_state.ch.jump_vel--;
            } else {
                g_state.ch.jump_vel = 0;
            }
        }
        break;
    }
    case KEY_DOWN: {
        int ny = g_state.ch.y + 1;
        if (ny < BOARD_H && g_state.board[ny][g_state.ch.x] == 0)
            g_state.ch.y = ny;
        break;
    }
    case 'x': case 'X': {
        if (g_state.ch.carrying == 0) {
            /* pick up block in facing direction (same row or one below) */
            int tx = g_state.ch.x + g_state.ch.facing;
            int ty = g_state.ch.y;
            int picked = 0;
            if (tx >= 0 && tx < BOARD_W && ty >= 0 && ty < BOARD_H &&
                g_state.board[ty][tx] != 0) {
                g_state.ch.carrying = g_state.board[ty][tx];
                g_state.board[ty][tx] = 0;
                apply_column_gravity(tx);
                picked = 1;
            }
            if (!picked) {
                ty = g_state.ch.y + 1;
                if (tx >= 0 && tx < BOARD_W && ty >= 0 && ty < BOARD_H &&
                    g_state.board[ty][tx] != 0) {
                    g_state.ch.carrying = g_state.board[ty][tx];
                    g_state.board[ty][tx] = 0;
                    apply_column_gravity(tx);
                }
            }
        } else {
            /* place block in facing direction */
            int tx = g_state.ch.x + g_state.ch.facing;
            int ty = g_state.ch.y;
            if (tx >= 0 && tx < BOARD_W && ty >= 0 && ty < BOARD_H &&
                g_state.board[ty][tx] == 0) {
                g_state.board[ty][tx] = g_state.ch.carrying;
                g_state.ch.carrying = 0;
            }
        }
        break;
    }
    }
}

/* ──────────── Main ──────────── */
int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    load_highscore();
    init_game();

    /* init ncurses */
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    init_colors();

    /* check terminal size */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row < 24 || ws.ws_col < 55) {
            endwin();
            fprintf(stderr,
                "Terminal too small! Need 55x24+, got %dx%d\n",
                ws.ws_col, ws.ws_row);
            return 1;
        }
    }

    /* game loop */
    struct timespec ts;
    while (g_running) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long start_us = ts.tv_sec * 1000000L + ts.tv_nsec / 1000;

        handle_input();
        game_tick();
        render();

        clock_gettime(CLOCK_MONOTONIC, &ts);
        long end_us = ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
        long elapsed = end_us - start_us;
        if (elapsed < TICK_US)
            usleep(TICK_US - elapsed);
    }

    endwin();
    save_highscore();
    printf("Game Over! Score: %d  High Score: %d\n", g_state.score, g_highscore);
    return 0;
}
