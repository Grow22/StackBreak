/*
 * server.c - Tetris Co-op Game Server
 *
 * Manages game logic and broadcasts state to two connected clients.
 * Player 0 = Tetris controller, Player 1 = Character controller.
 *
 * System calls used: socket, bind, listen, accept, read, write,
 *   close, fork, signal/sigaction, pthread_create, open, stat, ioctl
 */

#include "common.h"

/* ──────────── Global State ──────────── */
static GameState g_state;
static int       g_clients[MAX_CLIENTS];  /* client fds */
static int       g_num_clients = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_running = 1;
static int       g_highscore = 0;
static int       drop_counter = 0;
static int       gravity_counter = 0;
static int       g_coyote_timer = 0;
static int       g_jump_buffer = 0;
static int       g_was_on_ground = 1;

typedef VisualParticle Particle;

#define g_num_particles              g_state.num_particles
#define g_particles                  g_state.particles
#define g_shake_timer                g_state.shake_timer
#define g_shake_intensity            g_state.shake_intensity
#define g_lock_flash_timer           g_state.lock_flash_timer
#define g_lock_cells                 g_state.lock_cells
#define g_popup_score                g_state.popup_score
#define g_popup_timer                g_state.popup_timer
#define g_combo                      g_state.combo
#define g_combo_timer                g_state.combo_timer
#define g_bomb_flash_timer           g_state.bomb_flash_timer
#define g_bowser_hit_timer           g_state.boss_hit_timer
#define g_harddrop_cooldown_timer    g_state.harddrop_cooldown_timer
#define g_softdrop_cooldown_timer    g_state.softdrop_cooldown_timer
#define g_next_item_idx              g_state.next_item_idx
#define g_next_item_type             g_state.next_item_type
#define g_is_paused                  g_state.paused

#define HIGHSCORE_FILE "highscore.dat"
#define COYOTE_TICKS 5
#define JUMP_BUFFER_TICKS 6

/* Forward declarations */
static void apply_column_gravity(int col);
static void give_item(int item_type);
static void add_effect(int type, int x, int y, int timer, int param);
static void push_character_from_piece(int cells[4][2]);
static void stun_character(void);
static void escape_up(void);
static int char_on_ground(void);
static void update_total_score(void);
static void subtract_atk_score(void);
static void add_atk_score(void);
static void do_score_and_combo(int lc);
static void trigger_shake(int intensity, int dur);
static void tick_particles(void);
static void try_jump(void);
static int pick_item_cell_balanced(int type, int rot, int pc);
static QueuedPiece make_queued_piece(void);
static void sync_next_piece_preview(void);
static void init_piece_queue(void);
static QueuedPiece pop_piece_queue(void);
static void set_current_piece(QueuedPiece piece);

/* ──────────── Signal Handler ──────────── */
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ──────────── High Score (File I/O) ──────────── */
static void load_highscore(void) {
    struct stat st;
    if (stat(HIGHSCORE_FILE, &st) < 0) {
        g_highscore = 0;
        return;
    }
    int fd = open(HIGHSCORE_FILE, O_RDONLY);
    if (fd < 0) { g_highscore = 0; return; }
    if (read(fd, &g_highscore, sizeof(int)) < 0) {
        g_highscore = 0;
    }
    close(fd);
}

static void save_highscore(void) {
    update_total_score();
    if (g_state.score <= g_highscore) return;
    g_highscore = g_state.score;
    g_state.highscore = g_highscore;
    int fd = open(HIGHSCORE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, &g_highscore, sizeof(int)) < 0) {
        /* ignore error */
    }
    close(fd);
}

/* ──────────── Random Piece ──────────── */
static int random_piece(void) {
    return (rand() % 7) + 1;  /* 1..7 */
}

static float randf(float mn, float mx) {
    return mn + ((float)rand() / RAND_MAX) * (mx - mn);
}

static void spawn_particle(float x, float y, float vx, float vy,
                           int life, const char *ch, int color, int bold) {
    if (g_num_particles >= MAX_PARTICLES) {
        memmove(&g_particles[0], &g_particles[1],
                sizeof(Particle) * (MAX_PARTICLES - 1));
        g_num_particles = MAX_PARTICLES - 1;
    }

    Particle *p = &g_particles[g_num_particles++];
    p->x = x;
    p->y = y;
    p->vx = vx;
    p->vy = vy;
    p->life = life;
    p->max_life = life;
    strncpy(p->ch, ch, 3);
    p->ch[3] = '\0';
    p->color = color;
    p->bold = bold;
}

static void tick_particles(void) {
    for (int i = 0; i < g_num_particles; i++) {
        g_particles[i].x += g_particles[i].vx;
        g_particles[i].y += g_particles[i].vy;
        g_particles[i].vy += 0.02f;
        g_particles[i].life--;
        if (g_particles[i].life <= 0) {
            if (i < g_num_particles - 1) {
                memmove(&g_particles[i], &g_particles[i + 1],
                        sizeof(Particle) * (g_num_particles - i - 1));
            }
            g_num_particles--;
            i--;
        }
    }
}

static void spawn_harddrop_impact(int cells[4][2]) {
    for (int i = 0; i < 4; i++)
        spawn_particle(cells[i][1], cells[i][0], 0, 0, 3, "==", 7, 1);
    trigger_shake(1, 2);
}

static void spawn_line_sparkle(int row) {
    for (int c = 0; c < BOARD_W; c++) {
        if (rand() % 2)
            spawn_particle(c, row, randf(-0.3f, 0.3f), randf(-0.5f, -0.05f),
                           8 + rand() % 6, "**", 7, 1);
        else
            spawn_particle(c, row, randf(-0.2f, 0.2f), randf(-0.3f, 0.1f),
                           6 + rand() % 4, "::", 2, 1);
    }
}

static void spawn_bomb_explosion(int x, int y) {
    for (int i = 0; i < 6; i++)
        spawn_particle(x, y, randf(-0.3f, 0.3f), randf(-0.3f, 0.2f),
                       5 + rand() % 4, "**", (rand() % 2) ? 5 : 2, 1);
    g_bomb_flash_timer = 20;
    trigger_shake(2, 8);
}

static void spawn_drill_sparks(int x, int y) {
    for (int i = 0; i < 5; i++)
        spawn_particle(x, y, randf(-0.35f, 0.35f), randf(-0.4f, 0.1f),
                       4 + rand() % 4, (rand() % 2) ? "* " : "**", 2, 1);
}

static void spawn_shield_burst(int x, int y) {
    spawn_particle(x, y, 0, 0, 5, "<>", 16, 1);
    trigger_shake(1, 3);
}

static void spawn_gun_muzzle(int x, int y) {
    spawn_particle(x, y - 0.5f, 0, -0.3f, 4, "||", 17, 1);
    spawn_particle(x - 0.3f, y, -0.2f, -0.1f, 3, "* ", 2, 1);
    spawn_particle(x + 0.3f, y, 0.2f, -0.1f, 3, " *", 2, 1);
}

static void spawn_boss_hit(int x) {
    for (int i = 0; i < 4; i++)
        spawn_particle(x + randf(-0.5f, 0.5f), randf(-0.3f, 0.3f),
                       randf(-0.2f, 0.2f), randf(0.1f, 0.3f),
                       6 + rand() % 4, "!!", 18, 1);
    trigger_shake(1, 4);
}

static void spawn_stun_stars(int x, int y) {
    spawn_particle(x - 0.5f, y - 0.8f, -0.08f, -0.06f, 15, "* ", 2, 1);
    spawn_particle(x + 0.5f, y - 0.8f, 0.08f, -0.06f, 15, " *", 2, 1);
    spawn_particle(x, y - 1.0f, 0, -0.04f, 12, "**", 5, 1);
}

static void add_effect(int type, int x, int y, int timer, int param) {
    if (type == EFFECT_NONE || timer <= 0) return;

    if (g_state.num_effects >= MAX_EFFECTS) {
        memmove(&g_state.effects[0], &g_state.effects[1],
                sizeof(VisualEffect) * (MAX_EFFECTS - 1));
        g_state.num_effects = MAX_EFFECTS - 1;
    }

    VisualEffect *fx = &g_state.effects[g_state.num_effects++];
    fx->type = type;
    fx->x = x;
    fx->y = y;
    fx->timer = timer;
    fx->param = param;
}

static void tick_effects(void) {
    for (int i = 0; i < g_state.num_effects; i++) {
        g_state.effects[i].timer--;
        if (g_state.effects[i].timer <= 0) {
            if (i < g_state.num_effects - 1) {
                memmove(&g_state.effects[i], &g_state.effects[i + 1],
                        sizeof(VisualEffect) * (g_state.num_effects - i - 1));
            }
            g_state.num_effects--;
            i--;
        }
    }
}

static void trigger_shake(int intensity, int dur) {
    if (intensity >= g_shake_intensity) {
        g_shake_intensity = intensity;
        g_shake_timer = dur;
    }
}

/* ──────────── Init Game ──────────── */
static void init_game(void) {
    memset(&g_state, 0, sizeof(g_state));

    /* empty board */
    for (int r = 0; r < BOARD_H; r++)
        for (int c = 0; c < BOARD_W; c++)
            g_state.board[r][c] = 0;

    /* spawn first piece */
    srand(time(NULL));
    set_current_piece(make_queued_piece());
    init_piece_queue();

    /* character starts at bottom center */
    g_state.ch.x = BOARD_W / 2;
    g_state.ch.y = BOARD_H - 1;
    g_state.ch.carrying = 0;
    g_state.ch.stun_timer = 0;
    g_state.ch.stun_invuln_timer = 0;
    g_state.ch.facing = 1;
    g_state.ch.inv_count = 0;
    g_state.ch.shield_timer = 0;
    g_state.ch.drill_timer = 0;
    g_state.ch.drill_target_x = -1;
    g_state.ch.drill_target_y = -1;
    g_state.ch.drill_crack_timer = 0;

    g_state.score = 0;
    g_state.defscore = 0;
    g_state.atkscore = 0;
    g_state.level = 1;
    g_state.lines = 0;
    g_state.highscore = g_highscore;
    g_state.game_over = 0;
    g_state.game_started = 0;
    g_is_paused = 0;
    g_state.attacker_hp = 5;
    g_state.attacker_stun_timer = 0;
    g_state.attacker_spawn_delay = 0;
    g_harddrop_cooldown_timer = 0;
    g_softdrop_cooldown_timer = 0;
    g_combo = 0;
    g_combo_timer = 0;
    g_popup_score = 0;
    g_popup_timer = 0;
    g_lock_flash_timer = 0;
    memset(g_lock_cells, 0, sizeof(g_lock_cells));
    g_bomb_flash_timer = 0;
    g_bowser_hit_timer = 0;
    g_shake_timer = 0;
    g_shake_intensity = 0;
    g_state.num_bullets = 0;
    g_state.num_effects = 0;
    g_num_particles = 0;
    g_coyote_timer = 0;
    g_jump_buffer = 0;
    g_was_on_ground = 1;
}

/* ──────────── Piece Helpers ──────────── */

/* Get absolute board positions of current piece */
static int piece_cells(int type, int rot, int pr, int pc, int out[4][2]) {
    if (type < 1 || type > 7) return 0;
    const Shape *s = &SHAPES[type][rot % 4];
    for (int i = 0; i < 4; i++) {
        out[i][0] = pr + s->cells[i][0];
        out[i][1] = pc + s->cells[i][1];
    }
    return 1;
}

/* Check if piece position is valid */
static int piece_valid(int type, int rot, int pr, int pc) {
    int cells[4][2];
    piece_cells(type, rot, pr, pc, cells);
    for (int i = 0; i < 4; i++) {
        int r = cells[i][0], c = cells[i][1];
        if (r < 0 || r >= BOARD_H || c < 0 || c >= BOARD_W)
            return 0;
        if (g_state.board[r][c] != 0)
            return 0;
    }
    return 1;
}

/* Check if character overlaps with piece cells */
static int char_hit_by_piece(int cells[4][2]) {
    for (int i = 0; i < 4; i++) {
        if (cells[i][0] == g_state.ch.y && cells[i][1] == g_state.ch.x)
            return 1;
    }
    return 0;
}

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
            subtract_atk_score();
        } else {
            add_atk_score();
            stun_character();
            spawn_stun_stars(g_state.ch.x, g_state.ch.y);
        }
        push_character_from_piece(cells);
        return 1;
    }
    return 0;
}

static void push_character_from_piece(int cells[4][2]) {
    for (int dx = 1; dx < BOARD_W; dx++) {
        int nx = g_state.ch.x + dx;
        if (nx < BOARD_W && g_state.board[g_state.ch.y][nx] == 0) {
            int ov = 0;
            for (int j = 0; j < 4; j++)
                if (cells[j][0] == g_state.ch.y && cells[j][1] == nx)
                    ov = 1;
            if (!ov) { g_state.ch.x = nx; return; }
        }
        nx = g_state.ch.x - dx;
        if (nx >= 0 && g_state.board[g_state.ch.y][nx] == 0) {
            int ov = 0;
            for (int j = 0; j < 4; j++)
                if (cells[j][0] == g_state.ch.y && cells[j][1] == nx)
                    ov = 1;
            if (!ov) { g_state.ch.x = nx; return; }
        }
    }
}

static void escape_up(void) {
    while (g_state.ch.y >= 0 && g_state.board[g_state.ch.y][g_state.ch.x] != 0)
        g_state.ch.y--;
    if (g_state.ch.y < 0)
        g_state.ch.y = 0;
}

static void stun_character(void) {
    g_state.ch.stun_timer = STUN_TICKS;
    g_state.ch.stun_invuln_timer = STUN_TICKS + STUN_INVULN_TICKS;
}

/* Lock current piece onto board */
static void lock_piece(void) {
    int cells[4][2];
    piece_cells(g_state.piece_type, g_state.piece_rot,
                g_state.piece_r, g_state.piece_c, cells);

    /* check if character is hit */
    if (char_hit_by_piece(cells)) {
        if (g_state.ch.shield_timer > 0) {
            /* shield is active: attacker gets stunned, defender is safe */
            g_state.attacker_stun_timer = 45; /* 1.5 seconds */
            add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
            spawn_shield_burst(g_state.ch.x, g_state.ch.y);
            subtract_atk_score();
        } else if (g_state.ch.stun_invuln_timer == 0) {
            stun_character();
            add_atk_score();
            spawn_stun_stars(g_state.ch.x, g_state.ch.y);
        }
        push_character_from_piece(cells);
    }

    /* place blocks on board */
    for (int i = 0; i < 4; i++) {
        int r = cells[i][0], c = cells[i][1];
        if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W) {
            int val = g_state.piece_type;
            if (i == g_state.piece_item_idx) {
                val += g_state.piece_item_type * 10;
            }
            g_state.board[r][c] = val;
        }
    }

    /* apply global gravity to eliminate floating blocks */
    for (int c = 0; c < BOARD_W; c++) {
        apply_column_gravity(c);
    }

    if (g_state.ch.y >= 0 && g_state.ch.y < BOARD_H &&
        g_state.ch.x >= 0 && g_state.ch.x < BOARD_W &&
        g_state.board[g_state.ch.y][g_state.ch.x] != 0) {
        if (g_state.ch.shield_timer > 0) {
            g_state.attacker_stun_timer = 45;
            add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
            subtract_atk_score();
            spawn_shield_burst(g_state.ch.x, g_state.ch.y);
        } else if (g_state.ch.stun_invuln_timer == 0) {
            add_atk_score();
            stun_character();
            spawn_stun_stars(g_state.ch.x, g_state.ch.y);
        }
        escape_up();
    }
}

/* Clear completed lines */
static int clear_lines(void) {
    int cleared = 0;
    for (int r = BOARD_H - 1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < BOARD_W; c++)
            if (g_state.board[r][c] == 0) { full = 0; break; }
        if (full) {
            cleared++;
            spawn_line_sparkle(r);
            /* Mine items from the cleared line */
            for (int c = 0; c < BOARD_W; c++) {
                if (g_state.board[r][c] >= 10) {
                    give_item(g_state.board[r][c] / 10);
                }
            }
            /* check if character is on this line - push up */
            if (g_state.ch.y == r) {
                if (r > 0) g_state.ch.y = r - 1;
            } else if (g_state.ch.y < r) {
                g_state.ch.y++;  /* shift down with the blocks above */
            }
            /* shift everything above down */
            for (int rr = r; rr > 0; rr--)
                memcpy(g_state.board[rr], g_state.board[rr-1],
                       sizeof(int) * BOARD_W);
            memset(g_state.board[0], 0, sizeof(int) * BOARD_W);
            r++;  /* re-check this row */
        }
    }
    if (cleared > 0)
        trigger_shake(1, 3);
    return cleared;
}

static void count_board_items_zone(int *left, int *mid, int *right) {
    *left = 0;
    *mid = 0;
    *right = 0;
    for (int r = 0; r < BOARD_H; r++) {
        for (int c = 0; c < BOARD_W; c++) {
            if (g_state.board[r][c] >= 10) {
                if (c < 4) (*left)++;
                else if (c < 7) (*mid)++;
                else (*right)++;
            }
        }
    }
}

static int pick_item_cell_balanced(int type, int rot, int pc) {
    int cells[4][2];
    piece_cells(type, rot, 0, pc, cells);

    int zl, zm, zr;
    count_board_items_zone(&zl, &zm, &zr);
    int total = zl + zm + zr;
    if (total < 3)
        return rand() % 4;

    int min_zone = 0;
    int min_val = zl;
    if (zm < min_val) { min_val = zm; min_zone = 1; }
    if (zr < min_val) { min_val = zr; min_zone = 2; }

    int zone_cells[3][4], zone_n[3] = {0, 0, 0};
    for (int i = 0; i < 4; i++) {
        int z = (cells[i][1] < 4) ? 0 : (cells[i][1] < 7) ? 1 : 2;
        zone_cells[z][zone_n[z]++] = i;
    }

    if (zone_n[min_zone] == 0) {
        int vals[3] = {zl, zm, zr};
        int second = -1, sv = 999;
        for (int z = 0; z < 3; z++) {
            if (z != min_zone && vals[z] < sv && zone_n[z] > 0) {
                sv = vals[z];
                second = z;
            }
        }
        if (second >= 0) min_zone = second;
        else return rand() % 4;
    }

    int max_val = zl;
    if (zm > max_val) max_val = zm;
    if (zr > max_val) max_val = zr;
    int bias = 55 + (max_val - min_val) * 10;
    if (bias > 90) bias = 90;

    if (rand() % 100 < bias)
        return zone_cells[min_zone][rand() % zone_n[min_zone]];
    return rand() % 4;
}

static QueuedPiece make_queued_piece(void) {
    QueuedPiece piece;
    piece.type = random_piece();
    if (rand() % 100 < 50) {
        piece.item_idx = pick_item_cell_balanced(piece.type, 0, BOARD_W / 2);
        piece.item_type = (rand() % 4) + 1;
    } else {
        piece.item_idx = -1;
        piece.item_type = 0;
    }
    return piece;
}

static void sync_next_piece_preview(void) {
    g_state.next_type = g_state.piece_queue[0].type;
    g_next_item_idx = g_state.piece_queue[0].item_idx;
    g_next_item_type = g_state.piece_queue[0].item_type;
}

static void init_piece_queue(void) {
    for (int i = 0; i < PIECE_QUEUE_DEPTH; i++)
        g_state.piece_queue[i] = make_queued_piece();
    sync_next_piece_preview();
}

static QueuedPiece pop_piece_queue(void) {
    QueuedPiece piece = g_state.piece_queue[0];
    for (int i = 0; i < PIECE_QUEUE_DEPTH - 1; i++)
        g_state.piece_queue[i] = g_state.piece_queue[i + 1];
    g_state.piece_queue[PIECE_QUEUE_DEPTH - 1] = make_queued_piece();
    sync_next_piece_preview();
    return piece;
}

static void set_current_piece(QueuedPiece piece) {
    g_state.piece_type = piece.type;
    g_state.piece_rot = 0;
    g_state.piece_r = 0;
    g_state.piece_c = BOARD_W / 2;
    g_state.piece_item_idx = piece.item_idx;
    g_state.piece_item_type = piece.item_type;
}

/* Spawn new piece */
static void spawn_piece(void) {
    set_current_piece(pop_piece_queue());

    /* game over check */
    if (!piece_valid(g_state.piece_type, g_state.piece_rot,
                     g_state.piece_r, g_state.piece_c)) {
        g_state.game_over = 1;
        save_highscore();
    }
}

/* ──────────── Scoring ──────────── */
static void update_total_score(void) {
    g_state.score = (g_state.defscore > g_state.atkscore)
        ? g_state.defscore : g_state.atkscore;
}

static void add_score(int lc) {
    static const int pts[] = {0, 100, 300, 500, 800};
    if (lc > 0 && lc <= 4)
        g_state.defscore += pts[lc] * g_state.level;
    g_state.lines += lc;
    g_state.level = 1 + g_state.lines / 10;
    update_total_score();
}

static void subtract_atk_score(void) {
    if (g_state.atkscore < 100 * g_state.level)
        g_state.atkscore = 0;
    else
        g_state.atkscore -= 100 * g_state.level;
    g_popup_score = -100 * g_state.level;
    g_popup_timer = 30;
    update_total_score();
}

static void add_atk_score(void) {
    g_state.atkscore += 200 * g_state.level;
    g_popup_score = 200 * g_state.level;
    g_popup_timer = 30;
    update_total_score();
}

static void do_score_and_combo(int lc) {
    if (lc > 0) {
        static const int pts[] = {0, 100, 300, 500, 800};
        g_combo++;
        g_combo_timer = 150;
        if (g_combo > 1)
            g_state.defscore += g_combo * 50 * g_state.level;
        g_popup_score = pts[lc] * g_state.level;
        if (g_combo > 1)
            g_popup_score += g_combo * 50 * g_state.level;
        g_popup_timer = 24;
    }
    add_score(lc);
}

/* ──────────── Column Gravity ──────────── */
static void apply_column_gravity(int col) {
    for (int r = BOARD_H - 1; r > 0; r--) {
        if (g_state.board[r][col] == 0) {
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

/* ──────────── Character Physics ──────────── */
static int char_on_ground(void) {
    if (g_state.ch.y >= BOARD_H - 1) return 1;
    if (g_state.board[g_state.ch.y + 1][g_state.ch.x] != 0) return 1;
    return 0;
}

static void character_physics(void) {
    if (g_state.ch.stun_timer > 0) return;

    if (g_state.ch.jump_vel > 0) {
        int ny = g_state.ch.y - 1;
        if (ny >= 0 && g_state.board[ny][g_state.ch.x] == 0) {
            g_state.ch.y = ny;
            g_state.ch.jump_vel--;
            /* cancel drill if moving */
            g_state.ch.drill_crack_timer = 0;
        } else {
            g_state.ch.jump_vel = 0;
        }
    } else {
        int ny = g_state.ch.y + 1;
        if (ny < BOARD_H && g_state.board[ny][g_state.ch.x] == 0) {
            g_state.ch.y = ny;
            /* cancel drill if falling */
            g_state.ch.drill_crack_timer = 0;
        }
    }
}

/* ──────────── Process Input ──────────── */
static void try_jump(void) {
    if (g_state.ch.stun_timer > 0 || g_state.ch.jump_vel > 0)
        return;
    int grounded = char_on_ground() || g_coyote_timer > 0;
    if (!grounded)
        return;
    g_state.ch.jump_vel = 3;
    g_coyote_timer = 0;
    g_jump_buffer = 0;
    int ny = g_state.ch.y - 1;
    if (ny >= 0 && g_state.board[ny][g_state.ch.x] == 0) {
        g_state.ch.y = ny;
        g_state.ch.jump_vel--;
    } else {
        g_state.ch.jump_vel = 0;
    }
}

static void process_tetris_input(int key) {
    if (g_state.game_over) return;
    if (g_state.attacker_stun_timer > 0) return; /* attacker is stunned */
    if (g_state.piece_type == 0 || g_state.attacker_spawn_delay > 0) return;

    int nr, nc, nrot;
    switch (key) {
    case K_LEFT:
        nc = g_state.piece_c - 1;
        if (piece_valid(g_state.piece_type, g_state.piece_rot,
                        g_state.piece_r, nc))
            g_state.piece_c = nc;
        break;
    case K_RIGHT:
        nc = g_state.piece_c + 1;
        if (piece_valid(g_state.piece_type, g_state.piece_rot,
                        g_state.piece_r, nc))
            g_state.piece_c = nc;
        break;
    case K_ROTATE:
        nrot = (g_state.piece_rot + 1) % 4;
        if (piece_valid(g_state.piece_type, nrot,
                        g_state.piece_r, g_state.piece_c))
            g_state.piece_rot = nrot;
        /* wall kick: try shifting left/right */
        else if (piece_valid(g_state.piece_type, nrot,
                             g_state.piece_r, g_state.piece_c - 1)) {
            g_state.piece_rot = nrot;
            g_state.piece_c--;
        } else if (piece_valid(g_state.piece_type, nrot,
                               g_state.piece_r, g_state.piece_c + 1)) {
            g_state.piece_rot = nrot;
            g_state.piece_c++;
        }
        break;
    case K_SOFT_DROP:
        if (g_softdrop_cooldown_timer > 0)
            break;
        nr = g_state.piece_r + 1;
        if (piece_valid(g_state.piece_type, g_state.piece_rot, nr,
                        g_state.piece_c)) {
            g_state.piece_r = nr;
            g_state.score += 1;
        }
        g_softdrop_cooldown_timer = SOFT_DROP_COOLDOWN_TICKS;
        break;
    case K_HARD_DROP: {
        if (g_harddrop_cooldown_timer > 0)
            break;
        int start_r = g_state.piece_r;
        while (piece_valid(g_state.piece_type, g_state.piece_rot,
                           g_state.piece_r + 1, g_state.piece_c)) {
            g_state.piece_r++;
            g_state.score += 2;
        }
        harddrop_sweep_hits_character(start_r, g_state.piece_r);
        int cells[4][2];
        piece_cells(g_state.piece_type, g_state.piece_rot,
                    g_state.piece_r, g_state.piece_c, cells);
        memcpy(g_lock_cells, cells, sizeof(g_lock_cells));
        g_lock_flash_timer = 5;
        if (g_state.piece_r - start_r > 1)
            spawn_harddrop_impact(cells);
        lock_piece();
        do_score_and_combo(clear_lines());
        g_state.piece_type = 0;
        g_state.attacker_spawn_delay = 18; /* 0.6 sec delay */
        g_harddrop_cooldown_timer = HARD_DROP_COOLDOWN_TICKS;
        g_softdrop_cooldown_timer = SOFT_DROP_COOLDOWN_TICKS;
        drop_counter = 0;
        break;
    }
    }
}

static void give_item(int item_type) {
    if (item_type <= 0) return;
    if (g_state.ch.inv_count < 3) {
        g_state.ch.inventory[g_state.ch.inv_count++] = item_type;
    } else {
        /* full: shift left and drop oldest */
        g_state.ch.inventory[0] = g_state.ch.inventory[1];
        g_state.ch.inventory[1] = g_state.ch.inventory[2];
        g_state.ch.inventory[2] = item_type;
    }
}

static void process_char_input(int key) {
    if (g_state.game_over) return;
    if (g_state.ch.stun_timer > 0) return;

    int nx, ny;
    switch (key) {
    case K_CH_LEFT:
        g_state.ch.facing = -1;
        nx = g_state.ch.x - 1;
        if (nx >= 0) {
            if (g_state.board[g_state.ch.y][nx] == 0) {
                g_state.ch.x = nx;
                g_state.ch.drill_crack_timer = 0;
            } else if (g_state.ch.drill_timer > 0) {
                /* Start cracking block */
                if (g_state.ch.drill_target_x != nx || g_state.ch.drill_target_y != g_state.ch.y) {
                    g_state.ch.drill_target_x = nx;
                    g_state.ch.drill_target_y = g_state.ch.y;
                    g_state.ch.drill_crack_timer = 8;
                }
            }
        }
        break;
    case K_CH_RIGHT:
        g_state.ch.facing = 1;
        nx = g_state.ch.x + 1;
        if (nx < BOARD_W) {
            if (g_state.board[g_state.ch.y][nx] == 0) {
                g_state.ch.x = nx;
                g_state.ch.drill_crack_timer = 0;
            } else if (g_state.ch.drill_timer > 0) {
                /* Start cracking block */
                if (g_state.ch.drill_target_x != nx || g_state.ch.drill_target_y != g_state.ch.y) {
                    g_state.ch.drill_target_x = nx;
                    g_state.ch.drill_target_y = g_state.ch.y;
                    g_state.ch.drill_crack_timer = 8;
                }
            }
        }
        break;
    case K_CH_UP:
    case K_CH_JUMP:
        g_jump_buffer = JUMP_BUFFER_TICKS;
        try_jump();
        break;
    case K_CH_DOWN:
        g_state.ch.facing = 0; /* aim down */
        /* move down if possible */
        ny = g_state.ch.y + 1;
        if (ny < BOARD_H && g_state.board[ny][g_state.ch.x] == 0) {
            g_state.ch.y = ny;
            g_state.ch.drill_crack_timer = 0;
        } else if (ny < BOARD_H && g_state.ch.drill_timer > 0) {
            if (g_state.ch.drill_target_x != g_state.ch.x ||
                g_state.ch.drill_target_y != ny) {
                g_state.ch.drill_target_x = g_state.ch.x;
                g_state.ch.drill_target_y = ny;
                g_state.ch.drill_crack_timer = 8;
            }
        }
        break;
    case K_CH_PICKUP:
        if (g_state.ch.carrying == 0) {
            int dx = (g_state.ch.facing == 0) ? 0 : g_state.ch.facing;
            int dy = (g_state.ch.facing == 0) ? 1 : 0;
            int tx = g_state.ch.x + dx;
            int ty = g_state.ch.y + dy;
            int picked = 0;
            if (tx >= 0 && tx < BOARD_W && ty >= 0 && ty < BOARD_H &&
                g_state.board[ty][tx] != 0) {
                g_state.ch.carrying = g_state.board[ty][tx] % 10;
                if (g_state.board[ty][tx] >= 10) {
                    give_item(g_state.board[ty][tx] / 10);
                }
                g_state.board[ty][tx] = 0;
                apply_column_gravity(tx);
                do_score_and_combo(clear_lines());
                picked = 1;
            }
            if (!picked && g_state.ch.facing != 0) {
                ty = g_state.ch.y + 1;
                if (tx >= 0 && tx < BOARD_W && ty >= 0 && ty < BOARD_H &&
                    g_state.board[ty][tx] != 0) {
                    g_state.ch.carrying = g_state.board[ty][tx] % 10;
                    if (g_state.board[ty][tx] >= 10) {
                        give_item(g_state.board[ty][tx] / 10);
                    }
                    g_state.board[ty][tx] = 0;
                    apply_column_gravity(tx);
                    do_score_and_combo(clear_lines());
                }
            }
        } else {
            int dx = (g_state.ch.facing == 0) ? 0 : g_state.ch.facing;
            int dy = (g_state.ch.facing == 0) ? 1 : 0;
            int tx = g_state.ch.x + dx;
            int ty = g_state.ch.y + dy;
            if (tx >= 0 && tx < BOARD_W && ty >= 0 && ty < BOARD_H &&
                g_state.board[ty][tx] == 0) {
                g_state.board[ty][tx] = g_state.ch.carrying;
                g_state.ch.carrying = 0;
                apply_column_gravity(tx);
                do_score_and_combo(clear_lines());
            }
        }
        break;
    case K_CH_ITEM:
        if (g_state.ch.inv_count > 0) {
            int item = g_state.ch.inventory[0];
            /* shift items */
            for (int i = 0; i < 2; i++) {
                g_state.ch.inventory[i] = g_state.ch.inventory[i+1];
            }
            g_state.ch.inv_count--;
            
            if (item == 1) {
                /* Bomb: clear 4x4 around character and mine items */
                int sx = g_state.ch.x - 2;
                int sy = g_state.ch.y - 2;
                int destroyed_count = 0;
                for (int r = sy; r < sy + 4; r++) {
                    for (int c = sx; c < sx + 4; c++) {
                        if (r >= 0 && r < BOARD_H && c >= 0 && c < BOARD_W) {
                            if (g_state.board[r][c] != 0) {
                                if (g_state.board[r][c] >= 10) {
                                    give_item(g_state.board[r][c] / 10);
                                }
                                g_state.board[r][c] = 0;
                                destroyed_count++;
                            }
                        }
                    }
                }
                for (int c = sx; c < sx + 4; c++) {
                    if (c >= 0 && c < BOARD_W) apply_column_gravity(c);
                }
                if (destroyed_count > 0) {
                    g_state.defscore += destroyed_count * 10 * g_state.level;
                    g_popup_score = destroyed_count * 10 * g_state.level;
                    g_popup_timer = 30;
                    update_total_score();
                }
                do_score_and_combo(clear_lines());
                add_effect(EFFECT_BOMB, g_state.ch.x, g_state.ch.y, 10, 4);
                spawn_bomb_explosion(g_state.ch.x, g_state.ch.y);
            } else if (item == 2) {
                /* Drill: activate for 3 seconds */
                g_state.ch.drill_timer = 90;
            } else if (item == 3) {
                /* Shield: activate for 3.5 seconds */
                g_state.ch.shield_timer = 105;
            } else if (item == 4) {
                /* Gun: spawn a bullet traveling up */
                if (g_state.num_bullets < MAX_BULLETS) {
                    g_state.bullets[g_state.num_bullets][0] = g_state.ch.x;
                    g_state.bullets[g_state.num_bullets][1] = g_state.ch.y;
                    g_state.num_bullets++;
                    add_effect(EFFECT_GUN_FIRE, g_state.ch.x, g_state.ch.y, 6, 0);
                    spawn_gun_muzzle(g_state.ch.x, g_state.ch.y);
                }
            }
        }
        break;
    }
}

/* ──────────── Broadcast State ──────────── */
static void broadcast_state(void) {
    for (int i = 0; i < g_num_clients; i++) {
        int msg_type = MSG_STATE;
        if (send_all(g_clients[i], &msg_type, sizeof(int)) < 0 ||
            send_all(g_clients[i], &g_state, sizeof(GameState)) < 0) {
            /* client disconnected */
        }
    }
}

/* ──────────── Client Reader Thread ──────────── */
typedef struct {
    int fd;
    int role;  /* 0=tetris, 1=character */
} ClientArg;

static void *client_reader(void *arg) {
    ClientArg *ca = (ClientArg *)arg;
    MsgInput msg;

    while (g_running) {
        if (recv_all(ca->fd, &msg, sizeof(MsgInput)) < 0)
            break;
        if (msg.type != MSG_INPUT) continue;

        pthread_mutex_lock(&g_lock);
        if (msg.key == K_QUIT) {
            g_running = 0;
        } else if (msg.key == K_RESTART) {
            int started = g_state.game_started;
            init_game();
            g_state.game_started = started;
            drop_counter = 0;
            gravity_counter = 0;
        } else if (msg.key == K_PAUSE && !g_state.game_over) {
            g_is_paused = !g_is_paused;
        } else if (g_is_paused) {
            /* ignore gameplay input while paused */
        } else if (ca->role == 0) {
            process_tetris_input(msg.key);
        } else {
            process_char_input(msg.key);
        }
        pthread_mutex_unlock(&g_lock);
    }

    free(ca);
    return NULL;
}

static void game_tick(void) {
    if (!g_state.game_started || g_state.game_over || g_is_paused)
        return;

    tick_effects();
    tick_particles();

    if (g_state.ch.stun_timer > 0) {
        g_state.ch.stun_timer--;
        if (g_state.ch.stun_timer == 0) {
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
    if (g_combo_timer > 0) {
        g_combo_timer--;
        if (g_combo_timer == 0)
            g_combo = 0;
    }
    if (g_popup_timer > 0) g_popup_timer--;
    if (g_bomb_flash_timer > 0) g_bomb_flash_timer--;
    if (g_bowser_hit_timer > 0) g_bowser_hit_timer--;
    if (g_shake_timer > 0) {
        g_shake_timer--;
        if (g_shake_timer == 0)
            g_shake_intensity = 0;
    }

    if (g_state.ch.drill_crack_timer > 0) {
        g_state.ch.drill_crack_timer--;
        if (g_state.ch.drill_crack_timer == 0) {
            int tx = g_state.ch.drill_target_x;
            int ty = g_state.ch.drill_target_y;
            if (ty >= 0 && ty < BOARD_H && tx >= 0 && tx < BOARD_W) {
                if (g_state.board[ty][tx] >= 10)
                    give_item(g_state.board[ty][tx] / 10);
                g_state.board[ty][tx] = 0;
                g_state.defscore += 10 * g_state.level;
                g_popup_score = 10 * g_state.level;
                g_popup_timer = 30;
                update_total_score();
                apply_column_gravity(tx);
                do_score_and_combo(clear_lines());
                add_effect(EFFECT_DRILL, tx, ty, 6, 0);
                spawn_drill_sparks(tx, ty);
            }
            g_state.ch.drill_target_x = -1;
            g_state.ch.drill_target_y = -1;
        }
    }

    if (g_state.attacker_spawn_delay > 0) {
        g_state.attacker_spawn_delay--;
        if (g_state.attacker_spawn_delay == 0)
            spawn_piece();
    }

    int on_ground = char_on_ground();
    if (on_ground) {
        g_coyote_timer = COYOTE_TICKS;
        if (g_jump_buffer > 0 && g_state.ch.jump_vel == 0 &&
            g_state.ch.stun_timer == 0)
            try_jump();
    } else if (g_coyote_timer > 0) {
        g_coyote_timer--;
    }
    g_was_on_ground = on_ground;
    if (g_jump_buffer > 0)
        g_jump_buffer--;

    gravity_counter++;
    int phys_rate = 3;
    if (g_state.ch.jump_vel == 3) phys_rate = 2;
    else if (g_state.ch.jump_vel == 2) phys_rate = 3;
    else if (g_state.ch.jump_vel == 1) phys_rate = 6;

    if (gravity_counter >= phys_rate) {
        gravity_counter = 0;
        character_physics();
    }

    if (g_state.piece_type != 0 && g_state.attacker_stun_timer == 0) {
        int drop_speed = INITIAL_DROP - (g_state.level - 1) * 2;
        if (drop_speed < 3) drop_speed = 3;

        drop_counter++;
        if (drop_counter >= drop_speed) {
            drop_counter = 0;
            if (piece_valid(g_state.piece_type, g_state.piece_rot,
                            g_state.piece_r + 1, g_state.piece_c)) {
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

    if (g_state.piece_type != 0) {
        int cells[4][2];
        piece_cells(g_state.piece_type, g_state.piece_rot,
                    g_state.piece_r, g_state.piece_c, cells);
        if (char_hit_by_piece(cells) && g_state.ch.stun_timer == 0 &&
            g_state.ch.stun_invuln_timer == 0) {
            if (g_state.ch.shield_timer > 0) {
                if (g_state.attacker_stun_timer == 0)
                    add_effect(EFFECT_SHIELD, g_state.ch.x, g_state.ch.y, 10, 0);
                g_state.attacker_stun_timer = 45;
                subtract_atk_score();
                spawn_shield_burst(g_state.ch.x, g_state.ch.y);
            } else {
                add_atk_score();
                stun_character();
                spawn_stun_stars(g_state.ch.x, g_state.ch.y);
            }
        }
    }

    for (int i = 0; i < g_state.num_bullets; i++) {
        int removed = 0;
        for (int step = 0; step < 2 && !removed; step++) {
            g_state.bullets[i][1]--;
            int bx = g_state.bullets[i][0];
            int by = g_state.bullets[i][1];
            if (by < 0) {
                int boss_hit = (g_state.attacker_hp > 0 &&
                                bx >= g_state.piece_c - 1 &&
                                bx <= g_state.piece_c + 2);
                if (boss_hit) {
                    add_effect(EFFECT_GUN_HIT, bx, -1, 10, 0);
                    g_state.attacker_hp--;
                    g_state.defscore += 100;
                    g_popup_score = 100;
                    g_popup_timer = 60;
                    g_bowser_hit_timer = 6;
                    spawn_boss_hit(bx);
                    update_total_score();
                    if (g_state.attacker_hp <= 0) {
                        g_state.attacker_hp = 0;
                        update_total_score();
                        g_state.game_over = 1;
                        save_highscore();
                    }
                }
                removed = 1;
            }
        }

        if (removed) {
            for (int j = i; j < g_state.num_bullets - 1; j++) {
                g_state.bullets[j][0] = g_state.bullets[j + 1][0];
                g_state.bullets[j][1] = g_state.bullets[j + 1][1];
            }
            g_state.num_bullets--;
            i--;
        }
    }
}

/* ──────────── Main ──────────── */
int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    /* set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    load_highscore();
    init_game();

    /* create server socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); exit(1);
    }
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen"); close(server_fd); exit(1);
    }

    printf("[Server] Listening on port %d\n", port);
    printf("[Server] Waiting for 2 players to connect...\n");

    /* accept 2 clients */
    pthread_t reader_threads[MAX_CLIENTS];

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cfd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cfd < 0) {
            if (!g_running) break;
            perror("accept");
            continue;
        }

        g_clients[i] = cfd;
        g_num_clients++;

        /* send role assignment */
        MsgRole role_msg;
        role_msg.type = MSG_ROLE;
        role_msg.role = i;
        send_all(cfd, &role_msg, sizeof(MsgRole));

        printf("[Server] Player %d connected (%s) from %s\n",
               i + 1, i == 0 ? "Tetris" : "Character",
               inet_ntoa(cli_addr.sin_addr));

        /* start reader thread */
        ClientArg *ca = malloc(sizeof(ClientArg));
        ca->fd   = cfd;
        ca->role = i;
        pthread_create(&reader_threads[i], NULL, client_reader, ca);
    }

    if (g_num_clients == MAX_CLIENTS) {
        printf("[Server] Both players connected! Game starting.\n");
        g_state.game_started = 1;
    }

    /* ──── Game Loop ──── */
    struct timespec ts;
    while (g_running) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long start_us = ts.tv_sec * 1000000L + ts.tv_nsec / 1000;

        pthread_mutex_lock(&g_lock);
        game_tick();
        broadcast_state();
        pthread_mutex_unlock(&g_lock);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        long end_us = ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
        long elapsed = end_us - start_us;
        if (elapsed < TICK_US)
            usleep(TICK_US - elapsed);
    }

    /* game over - send final state */
    pthread_mutex_lock(&g_lock);
    broadcast_state();
    save_highscore();
    pthread_mutex_unlock(&g_lock);

    printf("[Server] Game Over! Score: %d  High Score: %d\n",
           g_state.score, g_highscore);

    /* cleanup */
    usleep(500000);  /* let clients read final state */
    for (int i = 0; i < g_num_clients; i++)
        close(g_clients[i]);
    close(server_fd);

    return 0;
}
