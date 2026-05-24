/*
 * local.c - Tetris VS Local Mode (Enhanced Edition)
 *
 * Full-featured local mode with particle effects, screen shake,
 * enhanced jump physics (coyote time, jump buffer), combo system,
 * and polished visual feedback.
 */

#include "common.h"
#include <ncurses.h>
#include <locale.h>
#include <sys/ioctl.h>	// 창 크기 자동 조정
#include <unistd.h>	// ""
/* ──────────── Global State ──────────── */
static GameState g_state;
static volatile sig_atomic_t g_running = 1;
static int g_is_paused = 0;
static int g_highscore = 0;
static int drop_counter = 0;
static int gravity_counter = 0;

#define HIGHSCORE_FILE "highscore.dat"
#define CELL_W 2
#define HARD_DROP_COOLDOWN_TICKS 90
#define SOFT_DROP_COOLDOWN_TICKS 4		// 소프트드롭 쿨타임 ~0.13초

/* ──────────── Particle System ──────────── */
#define MAX_PARTICLES 48

typedef struct {
    float x, y;
    float vx, vy;
    int life, max_life;
    char ch[4];
    int color;
    int bold;
} Particle;

static Particle g_particles[MAX_PARTICLES];
static int g_num_particles = 0;

/* ──────────── Screen Shake ──────────── */
static int g_shake_timer = 0;
static int g_shake_intensity = 0;

/* ──────────── Lock Flash ──────────── */
static int g_lock_flash_timer = 0;
static int g_lock_cells[4][2];

/* ──────────── Score Popup ──────────── */
static int g_popup_score = 0;
static int g_popup_timer = 0;

/* ──────────── Jump Enhancement ──────────── */
#define COYOTE_TICKS 5
#define JUMP_BUFFER_TICKS 6
static int g_coyote_timer = 0;
static int g_jump_buffer = 0;
static int g_was_on_ground = 1;

/* ──────────── Combo ──────────── */
static int g_combo = 0;
static int g_combo_timer = 0;

/* ──────────── Bomb Flash ──────────── */
static int g_bomb_flash_timer = 0;

/* ──────────── Bowser Hit Flash ──────────── */
static int g_bowser_hit_timer = 0;

/* Attacker hard-drop cooldown */
static int g_harddrop_cooldown_timer = 0;
static int g_softdrop_cooldown_timer = 0;

/* Next piece item info */
static int g_next_item_idx = -1;
static int g_next_item_type = 0;

/* Forward declarations */
static void apply_column_gravity(int col);
static void give_item(int item_type);
static void add_effect(int type, int x, int y, int timer, int param);
static void push_character_from_piece(int cells[4][2]);
static void stun_character(void);
static int char_on_ground(void);

/* ──────────── Helpers ──────────── */
static float randf(float mn, float mx) {
    return mn + ((float)rand() / RAND_MAX) * (mx - mn);
}

static void trigger_shake(int intensity, int dur) {
    if (intensity >= g_shake_intensity) {
        g_shake_intensity = intensity;
        g_shake_timer = dur;
    }
}

/* ──────────── Signal Handler ──────────── */
static void handle_signal(int sig) { (void)sig; g_running = 0; }

/* ──────────── High Score ──────────── */
static void load_highscore(void) {
    struct stat st;
    if (stat(HIGHSCORE_FILE, &st) < 0) { g_highscore = 0; return; }
    int fd = open(HIGHSCORE_FILE, O_RDONLY);
    if (fd < 0) { g_highscore = 0; return; }
    if (read(fd, &g_highscore, sizeof(int)) < 0) g_highscore = 0;
    close(fd);
}

static void save_highscore(void) {
    if (g_state.score <= g_highscore) return;
    g_highscore = g_state.score;
    int fd = open(HIGHSCORE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, &g_highscore, sizeof(int)) < 0) { /* ignore */ }
    close(fd);
}

/* ──────────── Random Piece ──────────── */
static int random_piece(void) { return (rand() % 7) + 1; }

/* ──────────── Particle System ──────────── */
static void spawn_particle(float x, float y, float vx, float vy,
                           int life, const char *ch, int color, int bold) {
    if (g_num_particles >= MAX_PARTICLES) {
        memmove(&g_particles[0], &g_particles[1],
                sizeof(Particle) * (MAX_PARTICLES - 1));
        g_num_particles = MAX_PARTICLES - 1;
    }
    Particle *p = &g_particles[g_num_particles++];
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->life = life; p->max_life = life;
    strncpy(p->ch, ch, 3); p->ch[3] = '\0';
    p->color = color; p->bold = bold;
}

static void tick_particles(void) {
    for (int i = 0; i < g_num_particles; i++) {
        g_particles[i].x += g_particles[i].vx;
        g_particles[i].y += g_particles[i].vy;
        g_particles[i].vy += 0.02f;
        g_particles[i].life--;
        if (g_particles[i].life <= 0) {
            if (i < g_num_particles - 1)
                memmove(&g_particles[i], &g_particles[i+1],
                        sizeof(Particle) * (g_num_particles - i - 1));
            g_num_particles--;
            i--;
        }
    }
    if (g_shake_timer > 0) {
        g_shake_timer--;
        if (g_shake_timer == 0) g_shake_intensity = 0;
    }
}

/* ── Particle spawn helpers ── */
static void spawn_harddrop_impact(int cells[4][2]) {
    for (int i = 0; i < 4; i++)
        spawn_particle(cells[i][1], cells[i][0], 0, 0, 3, "==", 7, 1);
    trigger_shake(1, 2);
}

static void spawn_line_sparkle(int row) {
    for (int c = 0; c < BOARD_W; c++) {
        if (rand()%2)
            spawn_particle(c, row, randf(-0.3f,0.3f), randf(-0.5f,-0.05f),
                           8+rand()%6, "**", 7, 1);
        else
            spawn_particle(c, row, randf(-0.2f,0.2f), randf(-0.3f,0.1f),
                           6+rand()%4, "::", 2, 1);
    }
}

static void spawn_bomb_explosion(int x, int y) {
    /* Fewer particles, mostly the BOMB! text does the impact */
    for (int i = 0; i < 6; i++) {
        float vx = randf(-0.3f, 0.3f), vy = randf(-0.3f, 0.2f);
        spawn_particle(x, y, vx, vy, 5+rand()%4, "**", (rand()%2)?5:2, 1);
    }
    g_bomb_flash_timer = 20;  /* 0.65s at 30fps */
    trigger_shake(2, 8);
}

static void spawn_drill_sparks(int x, int y) {
    for (int i = 0; i < 5; i++)
        spawn_particle(x, y, randf(-0.35f,0.35f), randf(-0.4f,0.1f),
                       4+rand()%4, (rand()%2)?"* ":"**", 2, 1);
}

static void spawn_shield_burst(int x, int y) {
    /* Simple flash, shield visual is rendered above character */
    spawn_particle(x, y, 0, 0, 5, "<>", 16, 1);
    trigger_shake(1, 3);
}

static void spawn_gun_muzzle(int x, int y) {
    spawn_particle(x, y-0.5f, 0, -0.3f, 4, "||", 17, 1);
    spawn_particle(x-0.3f, y, -0.2f, -0.1f, 3, "* ", 2, 1);
    spawn_particle(x+0.3f, y,  0.2f, -0.1f, 3, " *", 2, 1);
}

static void spawn_boss_hit(int x) {
    for (int i = 0; i < 4; i++)
        spawn_particle(x+randf(-0.5f,0.5f), randf(-0.3f,0.3f),
                       randf(-0.2f,0.2f), randf(0.1f,0.3f),
                       6+rand()%4, "!!", 18, 1);
    trigger_shake(1, 4);
}

static void spawn_stun_stars(int x, int y) {
    spawn_particle(x-0.5f, y-0.8f, -0.08f, -0.06f, 15, "* ", 2, 1);
    spawn_particle(x+0.5f, y-0.8f,  0.08f, -0.06f, 15, " *", 2, 1);
    spawn_particle(x, y-1.0f, 0, -0.04f, 12, "**", 5, 1);
}

/* ──────────── Space Invader Sprite (compact 6×4) ──────────── */
#define INVADER_W 6
#define INVADER_H 4

static const char INVADER_F1[INVADER_H][INVADER_W + 1] = {
    ".#..#.",   /* antennae        */
    ".####.",   /* head            */
    "#.##.#",   /* body + eyes     */
    ".#..#.",   /* legs (inward)   */
};

static const char INVADER_F2[INVADER_H][INVADER_W + 1] = {
    ".#..#.",   /* antennae        */
    ".####.",   /* head            */
    "#.##.#",   /* body + eyes     */
    "#....#",   /* legs (outward)  */
};

static void draw_invader(int top_y, int left_x, int frame,
                         int stunned, int hit) {
    int scr_h, scr_w;
    getmaxyx(stdscr, scr_h, scr_w);
    const char (*spr)[INVADER_W + 1] = (frame % 2 == 0) ? INVADER_F1 : INVADER_F2;

    for (int r = 0; r < INVADER_H; r++) {
        for (int c = 0; c < INVADER_W; c++) {
            if (spr[r][c] != '#') continue;
            if (stunned && (g_state.attacker_stun_timer / 3) % 2 == 0)
                continue;

            int cp = 4; /* green (original arcade color) */
            if (g_harddrop_cooldown_timer > 0) {
                int elapsed = HARD_DROP_COOLDOWN_TICKS - g_harddrop_cooldown_timer;
                if (elapsed < 0) elapsed = 0;
                if (elapsed > HARD_DROP_COOLDOWN_TICKS)
                    elapsed = HARD_DROP_COOLDOWN_TICKS;
                int restored_rows = (elapsed * INVADER_H) / HARD_DROP_COOLDOWN_TICKS;
                if (r < INVADER_H - restored_rows)
                    cp = 31; /* cooldown heat: gray recedes upward */
            }
	    if (g_softdrop_cooldown_timer > 0) {
		int elapsed2 = SOFT_DROP_COOLDOWN_TICKS - g_softdrop_cooldown_timer;
		if (elapsed2 < 0) elapsed2 = 0;
		if (elapsed2 > SOFT_DROP_COOLDOWN_TICKS)
			elapsed2 = SOFT_DROP_COOLDOWN_TICKS;
	    }
            if (hit) cp = 5; /* flash red on hit */

            int dy = top_y + r;
            int dx = left_x + c * 2;
            if (dy < 0 || dy >= scr_h || dx < 0 || dx + 1 >= scr_w) continue;

            attron(COLOR_PAIR(cp));
            mvprintw(dy, dx, "  ");
            attroff(COLOR_PAIR(cp));
        }
    }
}

/* ──────────── Pac-Man (1-cell with mouth animation) ──────────── */
/* Mouth animation counter for wakka-wakka effect */
static int g_pac_anim = 0;

/* ──────────── Effects (from common.h) ──────────── */
static void add_effect(int type, int x, int y, int timer, int param) {
    if (type == EFFECT_NONE || timer <= 0) return;
    if (g_state.num_effects >= MAX_EFFECTS) {
        memmove(&g_state.effects[0], &g_state.effects[1],
                sizeof(VisualEffect) * (MAX_EFFECTS - 1));
        g_state.num_effects = MAX_EFFECTS - 1;
    }
    VisualEffect *fx = &g_state.effects[g_state.num_effects++];
    fx->type = type; fx->x = x; fx->y = y;
    fx->timer = timer; fx->param = param;
}

static void tick_effects(void) {
    for (int i = 0; i < g_state.num_effects; i++) {
        g_state.effects[i].timer--;
        if (g_state.effects[i].timer <= 0) {
            if (i < g_state.num_effects - 1)
                memmove(&g_state.effects[i], &g_state.effects[i+1],
                        sizeof(VisualEffect) * (g_state.num_effects - i - 1));
            g_state.num_effects--;
            i--;
        }
    }
}

/* ──────────── Column Gravity ──────────── */
static void apply_column_gravity(int col) {
    for (int r = BOARD_H - 1; r > 0; r--)
        if (g_state.board[r][col] == 0)
            for (int rr = r - 1; rr >= 0; rr--)
                if (g_state.board[rr][col] != 0) {
                    g_state.board[r][col] = g_state.board[rr][col];
                    g_state.board[rr][col] = 0;
                    break;
                }
}

/* ──────────── Give Item ──────────── */
static void give_item(int item_type) {
    if (item_type <= 0) return;
    if (g_state.ch.inv_count < 3)
        g_state.ch.inventory[g_state.ch.inv_count++] = item_type;
    else {
        g_state.ch.inventory[0] = g_state.ch.inventory[1];
        g_state.ch.inventory[1] = g_state.ch.inventory[2];
        g_state.ch.inventory[2] = item_type;
    }
}

/* ──────────── Character Helpers ──────────── */
static void push_character_from_piece(int cells[4][2]) {
    for (int dx = 1; dx < BOARD_W; dx++) {
        int nx = g_state.ch.x + dx;
        if (nx < BOARD_W && g_state.board[g_state.ch.y][nx] == 0) {
            int ov = 0;
            for (int j = 0; j < 4; j++)
                if (cells[j][0]==g_state.ch.y && cells[j][1]==nx) ov=1;
            if (!ov) { g_state.ch.x = nx; return; }
        }
        nx = g_state.ch.x - dx;
        if (nx >= 0 && g_state.board[g_state.ch.y][nx] == 0) {
            int ov = 0;
            for (int j = 0; j < 4; j++)
                if (cells[j][0]==g_state.ch.y && cells[j][1]==nx) ov=1;
            if (!ov) { g_state.ch.x = nx; return; }
        }
    }
}

/* Push character up until not inside a block */
static void escape_up(void) {
    while (g_state.ch.y >= 0 && g_state.board[g_state.ch.y][g_state.ch.x] != 0)
        g_state.ch.y--;
    if (g_state.ch.y < 0) g_state.ch.y = 0;
}

static void stun_character(void) {
    g_state.ch.stun_timer = STUN_TICKS;
    g_state.ch.stun_invuln_timer = STUN_TICKS + STUN_INVULN_TICKS;
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
        if (cells[i][0]==g_state.ch.y && cells[i][1]==g_state.ch.x) return 1;
    return 0;
}

/* ──────────── Lock Piece ──────────── */
static int harddrop_sweep_hits_character(int start_r, int end_r) {
    if (g_state.ch.stun_timer > 0 || g_state.ch.stun_invuln_timer > 0)
        return 0;

    for (int pr = start_r; pr <= end_r; pr++) {
        int cells[4][2];
        piece_cells(g_state.piece_type, g_state.piece_rot,
                    pr, g_state.piece_c, cells);
        if (!char_hit_by_piece(cells))
            continue;

        if (g_state.ch.shield_timer > 0) {
            g_state.attacker_stun_timer = 45;
            add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
            spawn_shield_burst(g_state.ch.x, g_state.ch.y);
        } else {
            stun_character();
            spawn_stun_stars(g_state.ch.x, g_state.ch.y);
        }
        push_character_from_piece(cells);
        return 1;
    }
    return 0;
}

static void lock_piece(void) {
    int cells[4][2];
    piece_cells(g_state.piece_type, g_state.piece_rot,
                g_state.piece_r, g_state.piece_c, cells);
    if (char_hit_by_piece(cells)) {
        if (g_state.ch.shield_timer > 0) {
            g_state.attacker_stun_timer = 45;
            add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
            spawn_shield_burst(g_state.ch.x, g_state.ch.y);
        } else if (g_state.ch.stun_invuln_timer == 0) {
            stun_character();
            spawn_stun_stars(g_state.ch.x, g_state.ch.y);
        }
        push_character_from_piece(cells);
    }
    for (int i = 0; i < 4; i++) {
        int r = cells[i][0], c = cells[i][1];
        if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W) {
            int val = g_state.piece_type;
            if (i == g_state.piece_item_idx)
                val += g_state.piece_item_type * 10;
            g_state.board[r][c] = val;
        }
    }
    for (int c = 0; c < BOARD_W; c++) apply_column_gravity(c);

    /* After gravity: check if character is now inside a block */
    if (g_state.ch.y >= 0 && g_state.ch.y < BOARD_H &&
        g_state.ch.x >= 0 && g_state.ch.x < BOARD_W &&
        g_state.board[g_state.ch.y][g_state.ch.x] != 0) {
        if (g_state.ch.shield_timer > 0) {
            g_state.attacker_stun_timer = 45;
            add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
            spawn_shield_burst(g_state.ch.x, g_state.ch.y);
        } else if (g_state.ch.stun_invuln_timer == 0) {
            stun_character();
            spawn_stun_stars(g_state.ch.x, g_state.ch.y);
        }
        escape_up();
    }
}

/* ──────────── Clear Lines ──────────── */
static int clear_lines(void) {
    int cleared = 0;
    for (int r = BOARD_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_W; c++)
            if (g_state.board[r][c] == 0) { full = 0; break; }
        if (full) {
            cleared++;
            spawn_line_sparkle(r);
            for (int c = 0; c < BOARD_W; c++)
                if (g_state.board[r][c] >= 10)
                    give_item(g_state.board[r][c] / 10);
            if (g_state.ch.y == r) { if (r > 0) g_state.ch.y = r-1; }
            else if (g_state.ch.y < r) g_state.ch.y++;
            for (int rr = r; rr > 0; rr--)
                memcpy(g_state.board[rr], g_state.board[rr-1], sizeof(int)*BOARD_W);
            memset(g_state.board[0], 0, sizeof(int)*BOARD_W);
            r++;
        }
    }
    if (cleared > 0) trigger_shake(1, 3);
    return cleared;
}

/* ──────────── Scoring ──────────── */
static void add_score(int lc) {
    static const int pts[] = {0, 100, 300, 500, 800};
    if (lc > 0 && lc <= 4) g_state.score += pts[lc] * g_state.level;
    g_state.lines += lc;
    g_state.level = 1 + g_state.lines / 10;
}

static void do_score_and_combo(int lc) {
    if (lc > 0) {
        g_combo++;
        g_combo_timer = 60;
        if (g_combo > 1)
            g_state.score += g_combo * 50 * g_state.level;
        static const int pts[] = {0,100,300,500,800};
        g_popup_score = pts[lc] * g_state.level;
        if (g_combo > 1) g_popup_score += g_combo * 50 * g_state.level;
        g_popup_timer = 24;
    } else {
        g_combo = 0;
    }
    add_score(lc);
}

/* ──────────── Spawn Piece ──────────── */
/* Count existing items per board third (left 0-3, mid 4-6, right 7-9) */
static void count_board_items_zone(int *left, int *mid, int *right) {
    *left = 0; *mid = 0; *right = 0;
    for (int r = 0; r < BOARD_H; r++)
        for (int c = 0; c < BOARD_W; c++)
            if (g_state.board[r][c] >= 10) {
                if (c < 4) (*left)++;
                else if (c < 7) (*mid)++;
                else (*right)++;
            }
}

/* Pick which of the 4 piece cells gets the item, biased toward the
   zone of the board with fewer existing items.
   Stronger imbalance → stronger bias, but always some randomness. */
static int pick_item_cell_balanced(int type, int rot, int pc) {
    int cells[4][2];
    piece_cells(type, rot, 0, pc, cells);

    int zl, zm, zr;
    count_board_items_zone(&zl, &zm, &zr);
    int total = zl + zm + zr;

    /* If very few items on board, just random */
    if (total < 3) return rand() % 4;

    /* Find the lightest zone */
    int min_zone = 0; /* 0=left, 1=mid, 2=right */
    int min_val = zl;
    if (zm < min_val) { min_val = zm; min_zone = 1; }
    if (zr < min_val) { min_val = zr; min_zone = 2; }

    /* Classify cells by zone */
    int zone_cells[3][4], zone_n[3] = {0,0,0};
    for (int i = 0; i < 4; i++) {
        int z = (cells[i][1] < 4) ? 0 : (cells[i][1] < 7) ? 1 : 2;
        zone_cells[z][zone_n[z]++] = i;
    }

    /* If no cells in the lightest zone, try second lightest */
    if (zone_n[min_zone] == 0) {
        int vals[3] = {zl, zm, zr};
        int second = -1, sv = 999;
        for (int z = 0; z < 3; z++)
            if (z != min_zone && vals[z] < sv && zone_n[z] > 0)
                { sv = vals[z]; second = z; }
        if (second >= 0) min_zone = second;
        else return rand() % 4;
    }

    /* Adaptive bias: bigger imbalance → stronger bias (55%-90%) */
    int max_val = zl; if (zm>max_val) max_val=zm; if (zr>max_val) max_val=zr;
    int diff = max_val - min_val;
    int bias = 55 + diff * 10;
    if (bias > 90) bias = 90;

    if (rand() % 100 < bias)
        return zone_cells[min_zone][rand() % zone_n[min_zone]];
    return rand() % 4;
}

static void spawn_piece(void) {
    g_state.piece_type = g_state.next_type;
    g_state.piece_rot = 0;
    g_state.piece_r = 0;
    g_state.piece_c = BOARD_W / 2;
    g_state.piece_item_idx = g_next_item_idx;
    g_state.piece_item_type = g_next_item_type;
    g_state.next_type = random_piece();
    if (rand() % 100 < 50) {
        g_next_item_idx = rand() % 4;
        g_next_item_type = (rand() % 4) + 1;
    } else {
        g_next_item_idx = -1;
        g_next_item_type = 0;
    }
    if (!piece_valid(g_state.piece_type, g_state.piece_rot,
                     g_state.piece_r, g_state.piece_c)) {
        g_state.game_over = 1;
        save_highscore();
    }
}

/* ──────────── Character Physics ──────────── */
static int char_on_ground(void) {
    if (g_state.ch.y >= BOARD_H - 1) return 1;
    return g_state.board[g_state.ch.y + 1][g_state.ch.x] != 0;
}

static void character_physics(void) {
    if (g_state.ch.stun_timer > 0) return;
    if (g_state.ch.jump_vel > 0) {
        int ny = g_state.ch.y - 1;
        if (ny >= 0 && g_state.board[ny][g_state.ch.x] == 0) {
            g_state.ch.y = ny;
            g_state.ch.jump_vel--;
            g_state.ch.drill_crack_timer = 0;
        } else g_state.ch.jump_vel = 0;
    } else {
        int ny = g_state.ch.y + 1;
        if (ny < BOARD_H && g_state.board[ny][g_state.ch.x] == 0) {
            g_state.ch.y = ny;
            g_state.ch.drill_crack_timer = 0;
        }
    }
}

static void try_jump(void) {
    if (g_state.ch.stun_timer > 0 || g_state.ch.jump_vel > 0) return;
    int grounded = char_on_ground() || g_coyote_timer > 0;
    if (!grounded) return;
    g_state.ch.jump_vel = 3;
    g_coyote_timer = 0;
    g_jump_buffer = 0;
    int ny = g_state.ch.y - 1;
    if (ny >= 0 && g_state.board[ny][g_state.ch.x] == 0) {
        g_state.ch.y = ny;
        g_state.ch.jump_vel--;
    } else g_state.ch.jump_vel = 0;
}

/* ──────────── Init ──────────── */
static void init_game(void) {
    memset(&g_state, 0, sizeof(g_state));
    srand(time(NULL));
    g_state.piece_type = random_piece();
    g_state.piece_rot = 0;
    g_state.piece_r = 0;
    g_state.piece_c = BOARD_W / 2;
    g_state.next_type = random_piece();
    if (rand()%100 < 50) {
        g_state.piece_item_idx = rand()%4;
        g_state.piece_item_type = (rand()%4)+1;
    } else { g_state.piece_item_idx = -1; g_state.piece_item_type = 0; }
    if (rand()%100 < 50) {
        g_next_item_idx = rand()%4;
        g_next_item_type = (rand()%4)+1;
    } else { g_next_item_idx = -1; g_next_item_type = 0; }
    g_state.ch.x = BOARD_W/2; g_state.ch.y = BOARD_H-1;
    g_state.ch.facing = 1;
    g_state.ch.drill_target_x = -1; g_state.ch.drill_target_y = -1;
    g_state.level = 1; g_state.game_started = 1;
    g_state.attacker_hp = 5;
    g_num_particles = 0;
    g_shake_timer = 0; g_shake_intensity = 0;
    g_lock_flash_timer = 0;
    g_popup_timer = 0; g_combo = 0; g_combo_timer = 0;
    g_coyote_timer = 0; g_jump_buffer = 0;
    g_was_on_ground = 1;
    g_bomb_flash_timer = 0;
    g_bowser_hit_timer = 0;
    g_harddrop_cooldown_timer = 0;
    g_softdrop_cooldown_timer = 0;
}

/* ──────────── Game Tick ──────────── */
static void game_tick(void) {
    if (g_state.game_over) return;

    tick_effects();
    tick_particles();

    /* Countdown timers */
    if (g_state.ch.stun_timer > 0) {
        g_state.ch.stun_timer--;
        if (g_state.ch.stun_timer == 0) {
            /* Stun ended: escape if stuck inside a block */
            if (g_state.ch.y >= 0 && g_state.ch.y < BOARD_H &&
                g_state.ch.x >= 0 && g_state.ch.x < BOARD_W &&
                g_state.board[g_state.ch.y][g_state.ch.x] != 0)
                escape_up();
            if (g_state.ch.stun_invuln_timer > 0) {
                add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
                spawn_shield_burst(g_state.ch.x, g_state.ch.y);
            }
        }
    }
    if (g_state.ch.stun_invuln_timer > 0) g_state.ch.stun_invuln_timer--;
    if (g_state.attacker_stun_timer > 0) g_state.attacker_stun_timer--;
    if (g_harddrop_cooldown_timer > 0) g_harddrop_cooldown_timer--;
    if (g_softdrop_cooldown_timer > 0) g_softdrop_cooldown_timer--;
    if (g_state.ch.shield_timer > 0) g_state.ch.shield_timer--;
    if (g_state.ch.drill_timer > 0) g_state.ch.drill_timer--;
    if (g_lock_flash_timer > 0) g_lock_flash_timer--;
    if (g_combo_timer > 0) g_combo_timer--;
    if (g_popup_timer > 0) g_popup_timer--;
    if (g_bomb_flash_timer > 0) g_bomb_flash_timer--;
    if (g_bowser_hit_timer > 0) g_bowser_hit_timer--;

    /* Drill cracking */
    if (g_state.ch.drill_crack_timer > 0) {
        g_state.ch.drill_crack_timer--;
        if (g_state.ch.drill_crack_timer == 0) {
            int tx = g_state.ch.drill_target_x, ty = g_state.ch.drill_target_y;
            if (ty>=0 && ty<BOARD_H && tx>=0 && tx<BOARD_W) {
                if (g_state.board[ty][tx] >= 10)
                    give_item(g_state.board[ty][tx] / 10);
                g_state.board[ty][tx] = 0;
                apply_column_gravity(tx);
                do_score_and_combo(clear_lines());
                add_effect(EFFECT_DRILL, tx, ty, 6, 0);
                spawn_drill_sparks(tx, ty);
            }
            g_state.ch.drill_target_x = -1; g_state.ch.drill_target_y = -1;
        }
    }

    /* Spawn delay */
    if (g_state.attacker_spawn_delay > 0) {
        g_state.attacker_spawn_delay--;
        if (g_state.attacker_spawn_delay == 0) spawn_piece();
    }

    /* Jump: coyote time, buffer, landing */
    int on_ground = char_on_ground();
    if (on_ground) {
        g_coyote_timer = COYOTE_TICKS;
        if (g_jump_buffer > 0 && g_state.ch.jump_vel == 0 && g_state.ch.stun_timer == 0)
            try_jump();
    } else {
        if (g_coyote_timer > 0) g_coyote_timer--;
    }
    g_was_on_ground = on_ground;
    if (g_jump_buffer > 0) g_jump_buffer--;

    /* Character physics */
    gravity_counter++;
    int phys_rate = 3;
    if (g_state.ch.jump_vel == 3) phys_rate = 2;
    else if (g_state.ch.jump_vel == 2) phys_rate = 3;
    else if (g_state.ch.jump_vel == 1) phys_rate = 6;
    if (gravity_counter >= phys_rate) {
        gravity_counter = 0;
        character_physics();
    }

    /* Auto-drop */
    if (g_state.piece_type != 0 && g_state.attacker_stun_timer == 0) {
        int spd = INITIAL_DROP - (g_state.level-1)*2;
        if (spd < 3) spd = 3;
        drop_counter++;
        if (drop_counter >= spd) {
            drop_counter = 0;
            if (piece_valid(g_state.piece_type, g_state.piece_rot,
                            g_state.piece_r+1, g_state.piece_c)) {
                g_state.piece_r++;
            } else {
                int cells[4][2];
                piece_cells(g_state.piece_type, g_state.piece_rot,
                            g_state.piece_r, g_state.piece_c, cells);
                memcpy(g_lock_cells, cells, sizeof(g_lock_cells));
                g_lock_flash_timer = 4;
                lock_piece();
                do_score_and_combo(clear_lines());
                g_state.piece_type = 0;
                g_state.attacker_spawn_delay = 18;
            }
        }
    }

    /* Collision check */
    if (g_state.piece_type != 0) {
        int cells[4][2];
        piece_cells(g_state.piece_type, g_state.piece_rot,
                    g_state.piece_r, g_state.piece_c, cells);
        if (char_hit_by_piece(cells) && g_state.ch.stun_timer==0 &&
            g_state.ch.stun_invuln_timer==0) {
            if (g_state.ch.shield_timer > 0) {
                if (g_state.attacker_stun_timer == 0)
                    add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
                g_state.attacker_stun_timer = 45;
                spawn_shield_burst(g_state.ch.x, g_state.ch.y);
            } else {
                stun_character();
                spawn_stun_stars(g_state.ch.x, g_state.ch.y);
            }
        }
    }

    /* Bullets (2 rows per tick = fast travel) */
    for (int i = 0; i < g_state.num_bullets; i++) {
        int removed = 0;
        for (int step = 0; step < 2 && !removed; step++) {
            g_state.bullets[i][1]--;
            int bx = g_state.bullets[i][0], by = g_state.bullets[i][1];
            if (by < 0) {
                /* Only hit if bullet aligns with boss (piece_c ~ piece_c+1) */
                int boss_hit = (g_state.attacker_hp > 0 &&
                                bx >= g_state.piece_c &&
                                bx <= g_state.piece_c + 1);
                if (boss_hit) {
                    add_effect(EFFECT_GUN_HIT, bx, -1, 10, 0);
                    g_state.attacker_hp--;
                    spawn_boss_hit(bx);
                    g_bowser_hit_timer = 6;
                    if (g_state.attacker_hp <= 0) {
                        g_state.attacker_hp = 0;
                        g_state.game_over = 1;
                        save_highscore();
                    }
                }
                removed = 1;
            } else if (by>=0 && by<BOARD_H && g_state.board[by][bx]!=0) {
                if (g_state.board[by][bx] >= 10)
                    give_item(g_state.board[by][bx] / 10);
                g_state.board[by][bx] = 0;
                apply_column_gravity(bx);
                do_score_and_combo(clear_lines());
                spawn_particle(bx, by, 0, 0, 4, "##", 2, 1);
                removed = 1;
            }
        }
        if (removed) {
            for (int j = i; j < g_state.num_bullets-1; j++) {
                g_state.bullets[j][0] = g_state.bullets[j+1][0];
                g_state.bullets[j][1] = g_state.bullets[j+1][1];
            }
            g_state.num_bullets--;
            i--;
        }
    }
}

/* ──────────── Colors ──────────── */
#define COLOR_PIECE_I 1
#define COLOR_PIECE_O 2
#define COLOR_PIECE_T 3
#define COLOR_PIECE_S 4
#define COLOR_PIECE_Z 5
#define COLOR_PIECE_J 6
#define COLOR_PIECE_L 7
#define COLOR_CHAR    8
#define COLOR_CHAR_STUN 9
#define COLOR_BORDER  10
#define COLOR_GHOST   11
#define COLOR_BG      12
#define COLOR_BOMB_PREVIEW 13
#define COLOR_BOMB_FLASH 14
#define COLOR_DRILL_FX 15
#define COLOR_SHIELD_FX 16
#define COLOR_GUN_FX  17
#define COLOR_HIT_FX  18
#define COLOR_BOMB_BLOCK_PREVIEW 19
#define COLOR_COMBO   20

static void init_colors(void) {
    start_color();
    init_pair(1, COLOR_WHITE, COLOR_CYAN);
    init_pair(2, COLOR_BLACK, COLOR_WHITE);
    init_pair(3, COLOR_WHITE, COLOR_MAGENTA);
    init_pair(4, COLOR_WHITE, COLOR_GREEN);
    init_pair(5, COLOR_WHITE, COLOR_RED);
    init_pair(6, COLOR_WHITE, COLOR_BLUE);
    init_pair(7, COLOR_BLACK, COLOR_WHITE);
    init_pair(8, COLOR_BLACK, COLOR_YELLOW);  /* Pac-Man yellow */
    init_pair(9, COLOR_WHITE, COLOR_RED);
    init_pair(10, COLOR_WHITE, COLOR_BLACK);
    init_pair(11, COLOR_WHITE, COLOR_BLACK);
    init_pair(12, COLOR_WHITE, COLOR_BLACK);
    init_pair(13, COLOR_WHITE, COLOR_BLACK);
    init_pair(14, COLOR_WHITE, COLOR_RED);
    init_pair(15, COLOR_BLACK, COLOR_WHITE);
    init_pair(16, COLOR_BLACK, COLOR_CYAN);
    init_pair(17, COLOR_BLACK, COLOR_GREEN);
    init_pair(18, COLOR_WHITE, COLOR_MAGENTA);
    init_pair(19, COLOR_BLACK, COLOR_YELLOW);
    /* Black-text versions of piece colors (for item letters) */
    init_pair(23, COLOR_BLACK, COLOR_CYAN);
    init_pair(24, COLOR_BLACK, COLOR_WHITE);   /* O-piece (white bg) */
    init_pair(25, COLOR_BLACK, COLOR_MAGENTA);
    init_pair(26, COLOR_BLACK, COLOR_GREEN);
    init_pair(27, COLOR_BLACK, COLOR_RED);
    init_pair(28, COLOR_BLACK, COLOR_BLUE);
    init_pair(29, COLOR_BLACK, COLOR_WHITE);   /* L-piece (white bg) */
    init_pair(20, COLOR_YELLOW, COLOR_BLACK);  /* Pac-Man body (half-block fg) */
    init_pair(21, COLOR_RED, COLOR_BLACK);     /* Pac-Man stunned (half-block fg) */
    init_pair(22, COLOR_GREEN, COLOR_BLACK);   /* Pac-Man drill (half-block fg) */

    init_pair(30, COLOR_WHITE, 208);	/* Bomb prev Color */
    init_pair(31, COLOR_WHITE, 240);	/* Boss HardDrop-CoolTime */
    init_pair(32, COLOR_WHITE, 201);    /* Heart alive (pink on black) */
    init_pair(33, COLOR_MAGENTA, 0);  /* Heart dead (dark) */
}

/* ──────────── Big Heart HP ──────────── */
/*  Pixel heart (2px wide x 3 tall, each pixel = "  " 2-char = 4 chars per heart):
 *  ##
 *  ##
 *  .#  (bottom tip, but we keep it 2-wide for simplicity)
 *
 *  Board width = 20 chars. 5 hearts x 4 chars = 20. Perfect fit.
 */
#define HEART_COUNT 5
#define HEART_ROWS 3
#define HEART_COLS 3

/*  Pixel heart (5 wide × 4 tall, each pixel = "  " 2-char block):
 *  .#.#.
 *  #####
 *  .###.
 *  ..#..
 */
static const int HEART_PATTERN[3][3] = {
    {1,0,1},
    {1,1,1},
    {0,1,0},
};

static void draw_big_heart(int y, int x, int alive) {
    int cp = alive ? 32 : 33;
    for (int r = 0; r < HEART_ROWS; r++) {
        for (int c = 0; c < HEART_COLS; c++) {
            if (HEART_PATTERN[r][c]) {
                attron(COLOR_PAIR(cp));
                mvprintw(y + r, x + c * 2, "  ");
                attroff(COLOR_PAIR(cp));
            }
        }
    }
}

static void draw_hp_hearts(int y, int x, int hp, int max_hp) {
    int spacing = HEART_ROWS * 1.5; /* heart(6) + gap(4) */
    for (int i = 0; i < max_hp; i++) {
        draw_big_heart(y+i*spacing, x, i < hp);
    }
}

static int piece_color(int t) { return (t>=1&&t<=7)?t:12; }
static int item_color(int t) { return (t>=1&&t<=7)?(t+22):12; } /* black-text version */
static double ticks_to_sec(int t) { return t/30.0; }

/* ──────────── Ghost Row ──────────── */
static int ghost_row(void) {
    int gr = g_state.piece_r;
    if (g_state.piece_type<1||g_state.piece_type>7) return gr;
    while (1) {
        int ok = 1;
        const Shape *s = &SHAPES[g_state.piece_type][g_state.piece_rot%4];
        for (int i = 0; i < 4; i++) {
            int r = (gr+1)+s->cells[i][0], c = g_state.piece_c+s->cells[i][1];
            if (r>=BOARD_H||c<0||c>=BOARD_W) { ok=0; break; }
            if (r>=0&&g_state.board[r][c]!=0) { ok=0; break; }
        }
        if (!ok) break;
        gr++;
    }
    return gr;
}

/* ──────────── Rendering Helpers ──────────── */

static void draw_cell(int y, int x, int type, int is_ghost, int item_type) {
    if (is_ghost) {
        attron(COLOR_PAIR(11)|A_DIM);
        mvprintw(y, x, "[]");
        attroff(COLOR_PAIR(11)|A_DIM);
    } else {
        int ct = type%10, si = type/10;
        if (item_type==0) item_type = si;
        if (ct>=1&&ct<=7) {
            if (item_type>=1&&item_type<=4) {
                attron(COLOR_PAIR(item_color(ct)));
                if (item_type==1) mvprintw(y,x," B");
                else if (item_type==2) mvprintw(y,x," D");
                else if (item_type==3) mvprintw(y,x," S");
                else if (item_type==4) mvprintw(y,x," G");
                attroff(COLOR_PAIR(item_color(ct)));
            } else {
                attron(COLOR_PAIR(piece_color(ct)));
                mvprintw(y,x,"  ");
                attroff(COLOR_PAIR(piece_color(ct)));
            }
        } else mvprintw(y,x,"  ");
    }
}

static int has_ready_bomb(void) {
    return g_state.ch.inv_count>0 && g_state.ch.inventory[0]==1;
}
static int is_bomb_preview(int r, int c) {
    if (g_state.game_over||!has_ready_bomb()) return 0;
    if (r==g_state.ch.y&&c==g_state.ch.x) return 0;
    int l=g_state.ch.x-2, t=g_state.ch.y-2;
    return r>=t&&r<t+4&&c>=l&&c<l+4;
}

static void draw_bomb_prev(int y, int x, int type) {
    if (type==0) {
        attron(COLOR_PAIR(13)); mvprintw(y,x,".."); attroff(COLOR_PAIR(13));
    } else {
        int it=type/10;
        attron(COLOR_PAIR(30)|A_BOLD);
        if(it==1) mvprintw(y,x," B"); else if(it==2) mvprintw(y,x," D");
        else if(it==3) mvprintw(y,x," S"); else if(it==4) mvprintw(y,x," G");
        else mvprintw(y,x,"  ");
        attroff(COLOR_PAIR(19)|A_BOLD);
    }
}

static void draw_fx_cell(int y, int x, int color, const char *text) {
    int my, mx; getmaxyx(stdscr,my,mx);
    if (y<0||y>=my||x<0||x+1>=mx) return;
    attron(COLOR_PAIR(color)|A_BOLD);
    mvprintw(y,x,"%s",text);
    attroff(COLOR_PAIR(color)|A_BOLD);
}

static void draw_effects(int sy, int sx) {
    for (int i=0; i<g_state.num_effects; i++) {
        const VisualEffect *fx = &g_state.effects[i];
        if (fx->type==EFFECT_BOMB) {
            int l=fx->x-2, t=fx->y-2;
            const char *txt = (fx->timer>6)?"**":(fx->timer>3)?"!!":"..";
            for (int r=t;r<t+4;r++) for(int c=l;c<l+4;c++)
                if(r>=0&&r<BOARD_H&&c>=0&&c<BOARD_W)
                    draw_fx_cell(sy+1+r,sx+1+c*CELL_W,14,txt);
        } else if (fx->type==EFFECT_DRILL) {
            if(fx->y>=0&&fx->y<BOARD_H&&fx->x>=0&&fx->x<BOARD_W)
                draw_fx_cell(sy+1+fx->y,sx+1+fx->x*CELL_W,15,"!!");
        } else if (fx->type==EFFECT_SHIELD) {
            if(fx->y>=0&&fx->y<BOARD_H&&fx->x>=0&&fx->x<BOARD_W)
                draw_fx_cell(sy+1+fx->y,sx+1+fx->x*CELL_W,16,"<>");
        } else if (fx->type==EFFECT_GUN_FIRE) {
            if(fx->y>=0&&fx->y<BOARD_H&&fx->x>=0&&fx->x<BOARD_W)
                draw_fx_cell(sy+1+fx->y,sx+1+fx->x*CELL_W,17,"||");
        } else if (fx->type==EFFECT_GUN_HIT) {
            int ey=(fx->y<0)?sy:sy+1+fx->y;
            if(fx->x>=0&&fx->x<BOARD_W)
                draw_fx_cell(ey,sx+1+fx->x*CELL_W,18,"!!");
        }
    }
}

static void draw_particles(int sy, int sx) {
    for (int i = 0; i < g_num_particles; i++) {
        Particle *p = &g_particles[i];
        int px = sx + 1 + (int)(p->x * CELL_W);
        int py = sy + 1 + (int)(p->y);
        int my, mx; getmaxyx(stdscr,my,mx);
        if (py<0||py>=my||px<0||px+1>=mx) continue;
        int attr = COLOR_PAIR(p->color);
        float ratio = (float)p->life / p->max_life;
        if (p->bold && ratio > 0.3f) attr |= A_BOLD;
        if (ratio < 0.25f) attr |= A_DIM;
        attron(attr);
        mvprintw(py, px, "%s", p->ch);
        attroff(attr);
    }
}

/* ──────────── Render ──────────── */
int my, mx;
int bh, bw;
int sy, sx;
static void render(void) {
    getmaxyx(stdscr,my,mx);
    bh = BOARD_H+2, bw = BOARD_W*CELL_W+2;  // 5/21 수정 : BOARD_H - 2 -> BOARD_H+2
    sy = (my-bh)/2, sx = (mx-bw)/2-8;	// 5/21 수정
    if (sy<6) sy=6;
    if (sx<0) sx=0;

    /* Screen shake */
    if (g_shake_timer > 0 && g_shake_intensity > 0) {
        sy += (rand()%(g_shake_intensity*2+1)) - g_shake_intensity;
        sx += (rand()%(g_shake_intensity*2+1)) - g_shake_intensity;
    }

    erase();

    /* Attacker HP (top-left area) */
    {
	const char *title1 = "HP";
	const char *title2 = "DEAD";
        attron(COLOR_PAIR(33)|A_BOLD);
	if (g_state.attacker_hp <= 0)
		mvprintw(sy/2+3.75, sx-6.5, "%s", title2);
	else
		mvprintw(sy/2+3.75, sx-6.5, "%s", title1);
        attroff(COLOR_PAIR(33)|A_BOLD);
        draw_hp_hearts(sy/2+5, sx-8.5, g_state.attacker_hp, 5);
    }

    /* Border */
    attron(COLOR_PAIR(10));
    for (int r=0;r<bh;r++) { mvprintw(sy+r,sx,"|"); mvprintw(sy+r,sx+bw-1,"|"); }
    for (int c=0;c<bw;c++) { mvprintw(sy,sx+c,"-"); mvprintw(sy+bh-1,sx+c,"-"); }
    mvprintw(sy,sx,"+"); mvprintw(sy,sx+bw-1,"+");
    mvprintw(sy+bh-1,sx,"+"); mvprintw(sy+bh-1,sx+bw-1,"+");
    attroff(COLOR_PAIR(10));

    /* Board cells */
    for (int r=0;r<BOARD_H;r++) for(int c=0;c<BOARD_W;c++) {
        int cy=sy+1+r, cx=sx+1+c*CELL_W;
        if (r==g_state.ch.drill_target_y && c==g_state.ch.drill_target_x &&
            g_state.ch.drill_crack_timer>0) {
            int phase = ((g_state.ch.drill_crack_timer-1)*4)/8+1;
            if(phase<1) phase=1;
            if(phase>4) phase=4;
            attron(COLOR_PAIR(15)|A_BOLD);
            mvprintw(cy,cx,"%d ",phase);
            attroff(COLOR_PAIR(15)|A_BOLD);
        } else if (is_bomb_preview(r,c)) {
            draw_cell(cy,cx,g_state.board[r][c],0,0);
            draw_bomb_prev(cy,cx,g_state.board[r][c]);
        } else {
            draw_cell(cy,cx,g_state.board[r][c],0,0);
        }
    }

    /* Lock flash */
    if (g_lock_flash_timer > 0) {
        int attr = COLOR_PAIR(7)|A_BOLD;
        if (g_lock_flash_timer <= 2) attr |= A_DIM;
        attron(attr);
        for (int i=0;i<4;i++) {
            int r=g_lock_cells[i][0], c=g_lock_cells[i][1];
            if(r>=0&&r<BOARD_H&&c>=0&&c<BOARD_W)
                mvprintw(sy+1+r, sx+1+c*CELL_W, "[]");
        }
        attroff(attr);
    }

    /* Ghost piece */
    if (g_state.piece_type>=1 && g_state.piece_type<=7 && !g_state.game_over) {
        int gr = ghost_row();
        const Shape *s = &SHAPES[g_state.piece_type][g_state.piece_rot%4];
        for (int i=0;i<4;i++) {
            int r=gr+s->cells[i][0], c=g_state.piece_c+s->cells[i][1];
            if(r>=0&&r<BOARD_H&&c>=0&&c<BOARD_W&&g_state.board[r][c]==0)
                draw_cell(sy+1+r, sx+1+c*CELL_W, 0, 1, 0);
        }
    }

    /* Falling piece */
    if (g_state.piece_type>=1 && g_state.piece_type<=7 && !g_state.game_over) {
        const Shape *s = &SHAPES[g_state.piece_type][g_state.piece_rot%4];
        for (int i=0;i<4;i++) {
            int r=g_state.piece_r+s->cells[i][0], c=g_state.piece_c+s->cells[i][1];
            if(r>=0&&r<BOARD_H&&c>=0&&c<BOARD_W) {
                int item = (i==g_state.piece_item_idx)?g_state.piece_item_type:0;
                draw_cell(sy+1+r, sx+1+c*CELL_W, g_state.piece_type, 0, item);
            }
        }
    }

    /* Character (Pac-Man 1-cell with wakka-wakka animation) */
    {
        int cr=g_state.ch.y, cc=g_state.ch.x;
        if(cr>=0&&cr<BOARD_H&&cc>=0&&cc<BOARD_W) {
            int cy=sy+1+cr, cx=sx+1+cc*CELL_W;
            g_pac_anim++;
            int mouth = (g_pac_anim / 6) % 2; /* 0=closed, 1=open */

            if (g_state.ch.stun_timer > 0) {
                /* Stunned: red bg, dizzy eyes */
                attron(COLOR_PAIR(9)|A_BOLD);
                if (g_state.ch.stun_timer%4<2) mvprintw(cy,cx,"XX");
                else mvprintw(cy,cx,"xx");
                attroff(COLOR_PAIR(9)|A_BOLD);
            } else if (g_state.ch.carrying) {
                /* Carrying block: yellow bg, carry indicator */
                attron(COLOR_PAIR(8)|A_BOLD);
                if(g_state.ch.facing==1)       mvprintw(cy,cx,"o]");
                else if(g_state.ch.facing==-1) mvprintw(cy,cx,"[o");
                else                           mvprintw(cy,cx,"oo");
                attroff(COLOR_PAIR(8)|A_BOLD);
            } else if (g_state.ch.drill_timer > 0) {
                /* Drill active: bright white bg, drill chars */
                attron(COLOR_PAIR(15)|A_BOLD);
                if(g_state.ch.facing==1)       mvprintw(cy,cx,">>");
                else if(g_state.ch.facing==-1) mvprintw(cy,cx,"<<");
                else                           mvprintw(cy,cx,"vv");
                attroff(COLOR_PAIR(15)|A_BOLD);
            } else {
                /* Normal: yellow bg, eye + mouth animation */
                attron(COLOR_PAIR(8)|A_BOLD);
                if (mouth) {
                    if(g_state.ch.facing==1)       mvprintw(cy,cx,"o>");
                    else if(g_state.ch.facing==-1) mvprintw(cy,cx,"<o");
                    else                           mvprintw(cy,cx,"oo");
                } else {
                    mvprintw(cy,cx,"oo");
                }
                attroff(COLOR_PAIR(8)|A_BOLD);
            }

            /* Shield bubble (1-cell size) */
            int invuln_shield_timer = 0;
            if (g_state.ch.shield_timer > 0)
                invuln_shield_timer = g_state.ch.shield_timer;
            else if (g_state.ch.stun_timer == 0 && g_state.ch.stun_invuln_timer > 0)
                invuln_shield_timer = g_state.ch.stun_invuln_timer;
            if (invuln_shield_timer > 0 && !g_state.ch.stun_timer) {
                int attr = COLOR_PAIR(16)|A_BOLD;
                if (invuln_shield_timer%4<2) attr |= A_REVERSE;
                attron(attr);
                if (cy-1 > sy)
                    mvprintw(cy-1, cx-1, "/--\\");
                mvprintw(cy, cx-1, "|");
                mvprintw(cy, cx+2, "|");
                if (cy+1 < sy+BOARD_H+1)
                    mvprintw(cy+1, cx-1, "\\--/");
                attroff(attr);
            }
        }
    }

    /* Bullets */
    attron(COLOR_PAIR(8)|A_BOLD);
    for (int i=0;i<g_state.num_bullets;i++) {
        int r=g_state.bullets[i][1], c=g_state.bullets[i][0];
        if(r>=-4&&r<BOARD_H&&c>=0&&c<BOARD_W)
            mvprintw(sy+1+r, sx+1+c*CELL_W, "^^");
    }
    attroff(COLOR_PAIR(8)|A_BOLD);

    /* Space Invader boss (moves with piece_c) */
    if (g_state.attacker_hp > 0) {
        int inv_x = sx + 1 + (g_state.piece_c - 2) * CELL_W;
        int inv_y = sy - INVADER_H - 1;

        static int inv_anim = 0;
        inv_anim++;
        draw_invader(inv_y, inv_x, inv_anim / 15,
                     g_state.attacker_stun_timer > 0,
                     g_bowser_hit_timer > 0);
    }

    draw_effects(sy, sx);
    draw_particles(sy, sx);

    /* ── Side Panel ── */
    int px = sx+bw+4, py = sy/3-1; // 5/21 수정: py = sy + 1 -> py = sy/3-1, px = sx+bw+2 -> px = sx+bw+4
    attron(A_BOLD); mvprintw(py,px,"TETRIS VS"); attroff(A_BOLD);
    py+=1;	// 5/21 수정: py+=2 -> py+=1
    mvprintw(py++,px,"Score: %d",g_state.score);
    mvprintw(py++,px,"Level: %d",g_state.level);
    mvprintw(py++,px,"Lines: %d",g_state.lines);
    mvprintw(py++,px,"High : %d",g_highscore);
	// 5/21 수정: py++ 제거

    /* Combo */
    if (g_combo_timer > 0 && g_combo > 1) {
        attron(COLOR_PAIR(20)|A_BOLD);
        mvprintw(py++,px,"x%d COMBO!",g_combo);
        attroff(COLOR_PAIR(20)|A_BOLD);
    } else py++;

    /* Next piece */
    {
        const char *item_names[] = {"","Bomb","Drill","Shield","Gun"};
        if (g_next_item_idx >= 0 && g_next_item_type >= 1 && g_next_item_type <= 4) {
            mvprintw(py++,px,"Next: [%s]", item_names[g_next_item_type]);
        } else {
            mvprintw(py++,px,"Next:");
        }
    }
    if (g_state.next_type>=1&&g_state.next_type<=7) {
        const Shape *s = &SHAPES[g_state.next_type][0];
        for (int i=0;i<4;i++) {
            int item = (i==g_next_item_idx) ? g_next_item_type : 0;
            draw_cell(py+1+s->cells[i][0], px+2+s->cells[i][1]*CELL_W,
                      g_state.next_type, 0, item);
        }
    }
    py+=4;

    /* Attacker */
    mvprintw(py++,px,"--- Attacker ---");
    if (g_state.attacker_stun_timer>0) {
        attron(COLOR_PAIR(9)|A_BOLD);
        mvprintw(py++,px,"STUNNED! %.1fs",ticks_to_sec(g_state.attacker_stun_timer));
        attroff(COLOR_PAIR(9)|A_BOLD);
    } else mvprintw(py++,px,"Status: OK");
    if (g_harddrop_cooldown_timer > 0) {
        attron(COLOR_PAIR(5)|A_BOLD);
        mvprintw(py++,px,"Harddrop CD: %.1fs",ticks_to_sec(g_harddrop_cooldown_timer));
        attroff(COLOR_PAIR(5)|A_BOLD);
    } else {
        attron(COLOR_PAIR(4)|A_BOLD);
        mvprintw(py++,px,"Harddrop: READY");
        attroff(COLOR_PAIR(4)|A_BOLD);
    }
    py++;

    /* Defender */
    mvprintw(py++,px,"--- Defender ---");
    if (g_state.ch.stun_timer>0)
        mvprintw(py++,px,"STUNNED! %.1fs",ticks_to_sec(g_state.ch.stun_timer));
    else if (g_state.ch.stun_invuln_timer>0)
        mvprintw(py++,px,"INVULN! %.1fs",ticks_to_sec(g_state.ch.stun_invuln_timer));
    else mvprintw(py++,px,"Status: OK");

    const char* itm[]={"","Bomb","Drill","Shld","Gun"};
    mvprintw(py++,px,"[%-5s][%-5s][%-5s]",
        g_state.ch.inv_count>0?itm[g_state.ch.inventory[0]]:"Empty",
        g_state.ch.inv_count>1?itm[g_state.ch.inventory[1]]:"Empty",
        g_state.ch.inv_count>2?itm[g_state.ch.inventory[2]]:"Empty");

    if (g_state.ch.shield_timer>0) {
        attron(COLOR_PAIR(16)|A_BOLD);
        mvprintw(py++,px,"[ SHIELD %.1fs ]",ticks_to_sec(g_state.ch.shield_timer));
        attroff(COLOR_PAIR(16)|A_BOLD);
    }
    if (g_state.ch.drill_timer>0) {
        attron(COLOR_PAIR(15)|A_BOLD);
        mvprintw(py++,px,"[ DRILL  %.1fs ]",ticks_to_sec(g_state.ch.drill_timer));
        attroff(COLOR_PAIR(15)|A_BOLD);
    }
    mvprintw(py++,px,"Carry: %s",g_state.ch.carrying?"Block":"None");
    py++;

    attron(A_BOLD); mvprintw(py++,px,"--- Controls ---"); attroff(A_BOLD);
    mvprintw(py++,px,"WASD+Space: Tetris");
    mvprintw(py++,px,"Arrows+ZXC: Char");
    mvprintw(py++,px,"R=Restart  Q=Quit");

    /* Score popup */
    if (g_popup_timer > 0) {
        int pop_y = sy + BOARD_H/2 - (24-g_popup_timer)/3;
        int pop_x = sx + bw/2 - 3;
        attron(COLOR_PAIR(2)|A_BOLD);
        mvprintw(pop_y, pop_x, "+%d", g_popup_score);
        attroff(COLOR_PAIR(2)|A_BOLD);
    }

    /* Game Over */
    if (g_state.game_over) {
        int cy=my/2, cx=mx/2-11;
        attron(A_BOLD|COLOR_PAIR(5));
        mvprintw(cy-2,cx,"                       ");
        mvprintw(cy-1,cx,"   ==================  ");
        mvprintw(cy,  cx,"     GAME  OVER !      ");
        if (g_state.attacker_hp<=0)
            mvprintw(cy+1,cx,"    DEFENDER WINS!     ");
        else
            mvprintw(cy+1,cx,"    ATTACKER WINS!     ");
        mvprintw(cy+2,cx,"    Score: %-8d    ",g_state.score);
        mvprintw(cy+3,cx,"   R=Restart  Q=Quit   ");
        mvprintw(cy+4,cx,"   ==================  ");
        mvprintw(cy+5,cx,"                       ");
        attroff(A_BOLD|COLOR_PAIR(5));
    }

    /* ── BOMB! overlay (drawn LAST = topmost layer) ── */
    if (g_bomb_flash_timer > 0) {
        /*  5×7 pixel bitmaps for B, O, M, B, ! */
        static const char bomb_bmp[5][7][6] = {
            /* B */ {"1111.","1...1","1...1","1111.","1...1","1...1","1111."},
            /* O */ {".111.","1...1","1...1","1...1","1...1","1...1",".111."},
            /* M */ {"1...1","11.11","1.1.1","1...1","1...1","1...1","1...1"},
            /* B */ {"1111.","1...1","1...1","1111.","1...1","1...1","1111."},
            /* ! */ {"..1..","..1..","..1..","..1..","..1..",".....","..1.."},
        };
        int lw = 5, lh = 7, nl = 5, gp = 1;
        int total_bmp_w = nl * lw + (nl - 1) * gp; /* 29 */

        /* Scale to ~85% width, ~75% height */
        int pw = (mx * 85 / 100) / total_bmp_w;
        int ph = (my * 75 / 100) / lh;
        if (pw < 1) pw = 1;
        if (ph < 1) ph = 1;

        int text_w = total_bmp_w * pw;
        int text_h = lh * ph;
        int ox = (mx - text_w) / 2;
        int oy = (my - text_h) / 2;

        /* High-quality phased color:
         *  24-22: white flash (initial impact)
         *  21-8:  bold red with subtle pulse
         *  7-1:   dim fade out */
        int attr;
        if (g_bomb_flash_timer >= 18) {
            /* Initial white flash */
            attr = COLOR_PAIR(7) | A_BOLD;
        } else if (g_bomb_flash_timer > 5) {
            /* Bold red with pulse (alternate bold every 3 ticks) */
            if ((g_bomb_flash_timer / 3) % 2 == 0)
                attr = COLOR_PAIR(14) | A_BOLD;
            else
                attr = COLOR_PAIR(14);
        } else {
            /* Fade out */
            attr = COLOR_PAIR(14) | A_DIM;
        }

        /* Pre-build a fill string of pw spaces */
        char fill[64];
        int flen = pw;
        if (flen > 63) flen = 63;
        memset(fill, ' ', flen);
        fill[flen] = '\0';

        attron(attr);
        for (int li = 0; li < nl; li++) {
            int letter_x = ox + li * (lw + gp) * pw;
            for (int br = 0; br < lh; br++) {
                for (int bc = 0; bc < lw; bc++) {
                    if (bomb_bmp[li][br][bc] == '1') {
                        int px_x = letter_x + bc * pw;
                        for (int dy = 0; dy < ph; dy++) {
                            int py_y = oy + br * ph + dy;
                            if (py_y >= 0 && py_y < my &&
                                px_x >= 0 && px_x + flen <= mx)
                                mvprintw(py_y, px_x, "%s", fill);
                        }
                    }
                }
            }
        }
        attroff(attr);
    }

    refresh();
}

/* ──────────── Input ──────────── */
static void handle_input(void) {
    int ch = getch();
    if (ch == ERR) return;
    switch (ch) {
	    case 'q': case 'Q':
		    g_running = 0;
		    break;
	    case 'r': case 'R':
    		    init_game();
		    drop_counter=0;
		    g_is_paused=0;
		    gravity_counter=0;
		    break;
    }
    if (g_state.game_over) return;
    if (ch == 'p' || ch == 'P') {
	    g_is_paused = !g_is_paused;
	    return;
    }
    if (g_is_paused) return;

    int nc;
    /* Tetris (WASD+Space) */
    if (g_state.attacker_stun_timer==0 && g_state.piece_type!=0 &&
        g_state.attacker_spawn_delay==0) {
	switch (ch) {
        case 'a': case 'A':
            nc=g_state.piece_c-1;
            if(piece_valid(g_state.piece_type,g_state.piece_rot,g_state.piece_r,nc))
                g_state.piece_c=nc;
            break;
        case 'd': case 'D':
            nc=g_state.piece_c+1;
            if(piece_valid(g_state.piece_type,g_state.piece_rot,g_state.piece_r,nc))
                g_state.piece_c=nc;
            break;
        case 'w': case 'W':
            int nr=(g_state.piece_rot+1)%4;
            if(piece_valid(g_state.piece_type,nr,g_state.piece_r,g_state.piece_c))
                g_state.piece_rot=nr;
            else if(piece_valid(g_state.piece_type,nr,g_state.piece_r,g_state.piece_c-1))
                { g_state.piece_rot=nr; g_state.piece_c--; }
            else if(piece_valid(g_state.piece_type,nr,g_state.piece_r,g_state.piece_c+1))
                { g_state.piece_rot=nr; g_state.piece_c++; }
            break;
        case 's': case 'S':
		if (g_softdrop_cooldown_timer > 0)
			break;	
		if(piece_valid(g_state.piece_type,g_state.piece_rot,
                           g_state.piece_r+1,g_state.piece_c))
                { g_state.piece_r++; g_state.score+=1; }

	        g_softdrop_cooldown_timer = SOFT_DROP_COOLDOWN_TICKS;
            break;
        case ' ': {
            if (g_harddrop_cooldown_timer > 0)
                break;
            int sr = g_state.piece_r;
            while(piece_valid(g_state.piece_type,g_state.piece_rot,
                              g_state.piece_r+1,g_state.piece_c))
                { g_state.piece_r++; g_state.score+=2; }
            harddrop_sweep_hits_character(sr, g_state.piece_r);
            int cells[4][2];
            piece_cells(g_state.piece_type,g_state.piece_rot,
                        g_state.piece_r,g_state.piece_c,cells);
            memcpy(g_lock_cells,cells,sizeof(g_lock_cells));
            g_lock_flash_timer = 5;
            if (g_state.piece_r - sr > 1) spawn_harddrop_impact(cells);
            lock_piece();
            do_score_and_combo(clear_lines());
            g_state.piece_type=0;
            g_state.attacker_spawn_delay=18;
            g_harddrop_cooldown_timer = HARD_DROP_COOLDOWN_TICKS;
	    g_softdrop_cooldown_timer = SOFT_DROP_COOLDOWN_TICKS;
            drop_counter=0;
            break; }
        }
    }

    /* Character (Arrows+Z/X/C) */
    if (g_state.ch.stun_timer > 0) return;
    switch(ch) {
    case KEY_LEFT: {
        g_state.ch.facing=-1;
        int nx=g_state.ch.x-1;
        if(nx>=0) {
            if(g_state.board[g_state.ch.y][nx]==0) {
                g_state.ch.x=nx; g_state.ch.drill_crack_timer=0;
            } else if(g_state.ch.drill_timer>0) {
                if(g_state.ch.drill_target_x!=nx||g_state.ch.drill_target_y!=g_state.ch.y) {
                    g_state.ch.drill_target_x=nx; g_state.ch.drill_target_y=g_state.ch.y;
                    g_state.ch.drill_crack_timer=8;
                }
            }
        }
        break; }
    case KEY_RIGHT: {
        g_state.ch.facing=1;
        int nx=g_state.ch.x+1;
        if(nx<BOARD_W) {
            if(g_state.board[g_state.ch.y][nx]==0) {
                g_state.ch.x=nx; g_state.ch.drill_crack_timer=0;
            } else if(g_state.ch.drill_timer>0) {
                if(g_state.ch.drill_target_x!=nx||g_state.ch.drill_target_y!=g_state.ch.y) {
                    g_state.ch.drill_target_x=nx; g_state.ch.drill_target_y=g_state.ch.y;
                    g_state.ch.drill_crack_timer=8;
                }
            }
        }
        break; }
    case KEY_UP: case 'z': case 'Z':
        g_jump_buffer = JUMP_BUFFER_TICKS;
        try_jump();
        break;
    case KEY_DOWN: {
        g_state.ch.facing=0;
        int ny=g_state.ch.y+1;
        if(ny<BOARD_H) {
            if(g_state.board[ny][g_state.ch.x]==0) {
                g_state.ch.y=ny; g_state.ch.drill_crack_timer=0;
            } else if(g_state.ch.drill_timer>0) {
                if(g_state.ch.drill_target_x!=g_state.ch.x||g_state.ch.drill_target_y!=ny) {
                    g_state.ch.drill_target_x=g_state.ch.x; g_state.ch.drill_target_y=ny;
                    g_state.ch.drill_crack_timer=8;
                }
            }
        }
        break; }
    case 'x': case 'X': {
        if(g_state.ch.carrying==0) {
            int dx=(g_state.ch.facing==0)?0:g_state.ch.facing;
            int dy=(g_state.ch.facing==0)?1:0;
            int tx=g_state.ch.x+dx, ty=g_state.ch.y+dy;
            int picked=0;
            if(tx>=0&&tx<BOARD_W&&ty>=0&&ty<BOARD_H&&g_state.board[ty][tx]!=0) {
                g_state.ch.carrying=g_state.board[ty][tx]%10;
                if(g_state.board[ty][tx]>=10) give_item(g_state.board[ty][tx]/10);
                g_state.board[ty][tx]=0; apply_column_gravity(tx);
                do_score_and_combo(clear_lines()); picked=1;
            }
            if(!picked&&g_state.ch.facing!=0) {
                ty=g_state.ch.y+1;
                if(tx>=0&&tx<BOARD_W&&ty>=0&&ty<BOARD_H&&g_state.board[ty][tx]!=0) {
                    g_state.ch.carrying=g_state.board[ty][tx]%10;
                    if(g_state.board[ty][tx]>=10) give_item(g_state.board[ty][tx]/10);
                    g_state.board[ty][tx]=0; apply_column_gravity(tx);
                    do_score_and_combo(clear_lines());
                }
            }
        } else {
            int dx=(g_state.ch.facing==0)?0:g_state.ch.facing;
            int dy=(g_state.ch.facing==0)?1:0;
            int tx=g_state.ch.x+dx, ty=g_state.ch.y+dy;
            if(tx>=0&&tx<BOARD_W&&ty>=0&&ty<BOARD_H&&g_state.board[ty][tx]==0) {
                g_state.board[ty][tx]=g_state.ch.carrying; g_state.ch.carrying=0;
                apply_column_gravity(tx); do_score_and_combo(clear_lines());
            }
        }
        break; }
    case 'c': case 'C':
        if(g_state.ch.inv_count>0) {
            int item=g_state.ch.inventory[0];
            for(int i=0;i<2;i++) g_state.ch.inventory[i]=g_state.ch.inventory[i+1];
            g_state.ch.inv_count--;
            if(item==1) {
                int bx=g_state.ch.x-2, by=g_state.ch.y-2;
                for(int r=by;r<by+4;r++) for(int c=bx;c<bx+4;c++)
                    if(r>=0&&r<BOARD_H&&c>=0&&c<BOARD_W) {
                        if(g_state.board[r][c]>=10) give_item(g_state.board[r][c]/10);
                        g_state.board[r][c]=0;
                    }
                for(int c=bx;c<bx+4;c++) if(c>=0&&c<BOARD_W) apply_column_gravity(c);
                do_score_and_combo(clear_lines());
                add_effect(EFFECT_BOMB,g_state.ch.x,g_state.ch.y,10,4);
                spawn_bomb_explosion(g_state.ch.x,g_state.ch.y);
            } else if(item==2) {
                g_state.ch.drill_timer=90;
            } else if(item==3) {
                g_state.ch.shield_timer=105;
            } else if(item==4) {
                if(g_state.num_bullets<MAX_BULLETS) {
                    g_state.bullets[g_state.num_bullets][0]=g_state.ch.x;
                    g_state.bullets[g_state.num_bullets][1]=g_state.ch.y;
                    g_state.num_bullets++;
                    add_effect(EFFECT_GUN_FIRE,g_state.ch.x,g_state.ch.y,6,0);
                    spawn_gun_muzzle(g_state.ch.x,g_state.ch.y);
                }
            }
        }
        break;
    }
}

/* ──────────── Main ──────────── */
int main(void) {
    struct sigaction sa;
    memset(&sa,0,sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,&sa,NULL);
    sigaction(SIGTERM,&sa,NULL);

    load_highscore();
    init_game();

    // 5/21 수정 : 창 크기 자동 설정
    printf("\033[8;28;60t");
    fflush(stdout);
    usleep(50000);
    //

    setlocale(LC_ALL,"");
    initscr(); cbreak(); noecho(); curs_set(0);
    keypad(stdscr,TRUE); nodelay(stdscr,TRUE);
    init_colors();

    struct winsize ws;
    if (ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0) {
        if (ws.ws_row<28||ws.ws_col<60) {
            endwin();
            fprintf(stderr,"Terminal too small! Need 60x28+, got %dx%d\n",ws.ws_col,ws.ws_row);
            return 1;
        }
    }

    getmaxyx(stdscr,my,mx);
    bh = BOARD_H+2, bw = BOARD_W*CELL_W+2;
    sy = (my-bh)/2, sx = (mx-bw)/2-8;
    struct timespec ts;

    while (g_running) {
        clock_gettime(CLOCK_MONOTONIC,&ts);
        long s_us = ts.tv_sec*1000000L + ts.tv_nsec/1000;
        handle_input();

	if (!g_is_paused) {
		game_tick();
		render();
	}

	render();

	if (g_is_paused) {
		attron(A_BOLD | COLOR_PAIR(31));
		mvprintw(sy*2, sx, "        PAUSED       ");
		mvprintw(sy*2+1, sx, " Press 'P' to Resume ");
		mvprintw(sy*2+2, sx, " Retry='R', Quit='Q' ");
		attroff(A_BOLD | COLOR_PAIR(31));
		refresh();
	}

        clock_gettime(CLOCK_MONOTONIC,&ts);
        long e_us = ts.tv_sec*1000000L + ts.tv_nsec/1000;
        long el = e_us - s_us;
        if (el < TICK_US) usleep(TICK_US - el);
    }

    endwin();
    save_highscore();
    printf("Game Over! Score: %d  High Score: %d\n",g_state.score,g_highscore);
    return 0;
}
