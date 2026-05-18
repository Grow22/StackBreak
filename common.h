#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>

/* ──────────── Board ──────────── */
#define BOARD_W  10
#define BOARD_H  20

/* ──────────── Network ──────────── */
#define DEFAULT_PORT 9190
#define MAX_CLIENTS  2
#define MAX_BULLETS  3
#define MAX_EFFECTS  12

#define EFFECT_NONE      0
#define EFFECT_BOMB      1
#define EFFECT_DRILL     2
#define EFFECT_SHIELD    3
#define EFFECT_GUN_FIRE  4
#define EFFECT_GUN_HIT   5

/* ──────────── Timing (microseconds) ──────────── */
#define TICK_US       33333   /* ~30 fps */
#define INITIAL_DROP  30      /* ticks between auto-drops */
#define STUN_TICKS    60      /* 2 seconds at 30fps */
#define STUN_INVULN_TICKS 60  /* 2 seconds after stun ends */
#define LOCK_DELAY    15      /* ticks before piece locks */

/* ──────────── Piece types ──────────── */
typedef enum {
    PIECE_NONE = 0,
    PIECE_I, PIECE_O, PIECE_T,
    PIECE_S, PIECE_Z, PIECE_J, PIECE_L,
    PIECE_COUNT  /* 8, but 1-7 are valid */
} PieceType;

/* ──────────── Tetromino ──────────── */
/*  Each shape is stored as 4 cells (row, col) relative to pivot. */
typedef struct {
    int cells[4][2];  /* [i][0]=row, [i][1]=col */
} Shape;

/* All 7 pieces, 4 rotations each */
static const Shape SHAPES[8][4] = {
    { /* index 0 unused */
        { { {0,0},{0,0},{0,0},{0,0} } },
        { { {0,0},{0,0},{0,0},{0,0} } },
        { { {0,0},{0,0},{0,0},{0,0} } },
        { { {0,0},{0,0},{0,0},{0,0} } }
    },
    /* I */
    {
        { { {0,-1},{0,0},{0,1},{0,2} } },
        { { {-1,0},{0,0},{1,0},{2,0} } },
        { { {0,-1},{0,0},{0,1},{0,2} } },
        { { {-1,0},{0,0},{1,0},{2,0} } }
    },
    /* O */
    {
        { { {0,0},{0,1},{1,0},{1,1} } },
        { { {0,0},{0,1},{1,0},{1,1} } },
        { { {0,0},{0,1},{1,0},{1,1} } },
        { { {0,0},{0,1},{1,0},{1,1} } }
    },
    /* T */
    {
        { { {0,-1},{0,0},{0,1},{1,0} } },
        { { {-1,0},{0,0},{1,0},{0,1} } },
        { { {-1,0},{0,-1},{0,0},{0,1} } },
        { { {0,-1},{-1,0},{0,0},{1,0} } }
    },
    /* S */
    {
        { { {0,0},{0,1},{1,-1},{1,0} } },
        { { {-1,0},{0,0},{0,1},{1,1} } },
        { { {0,0},{0,1},{1,-1},{1,0} } },
        { { {-1,0},{0,0},{0,1},{1,1} } }
    },
    /* Z */
    {
        { { {0,-1},{0,0},{1,0},{1,1} } },
        { { {0,0},{1,0},{-1,1},{0,1} } },
        { { {0,-1},{0,0},{1,0},{1,1} } },
        { { {0,0},{1,0},{-1,1},{0,1} } }
    },
    /* J */
    {
        { { {0,-1},{0,0},{0,1},{1,1} } },
        { { {-1,0},{0,0},{1,0},{1,-1} } },
        { { {-1,-1},{0,-1},{0,0},{0,1} } },
        { { {-1,1},{-1,0},{0,0},{1,0} } }
    },
    /* L */
    {
        { { {0,-1},{0,0},{0,1},{1,-1} } },
        { { {-1,0},{0,0},{1,0},{-1,-1} } },
        { { {0,-1},{0,0},{0,1},{-1,1} } },
        { { {-1,0},{0,0},{1,0},{1,1} } }
    }
};

/* ──────────── Character ──────────── */
typedef struct {
    int x, y;           /* board position (col, row) */
    int carrying;       /* 0=none, 1-7=block type */
    int stun_timer;     /* ticks remaining */
    int stun_invuln_timer; /* ticks remaining until stun can happen again */
    int facing;         /* -1=left, 1=right */
    int jump_vel;       /* remaining upward ticks (0=not jumping) */
    int inventory[3];   /* 3 item slots */
    int inv_count;      /* 0 to 3 */
    int shield_timer;   /* ticks remaining for shield */
    int drill_timer;    /* ticks remaining for drill */
    int drill_target_x;
    int drill_target_y;
    int drill_crack_timer;
} Character;

typedef struct {
    int type;
    int x, y;           /* board position (col, row) */
    int timer;          /* ticks remaining */
    int param;          /* effect-specific size/state */
} VisualEffect;

/* ──────────── Game State ──────────── */
typedef struct {
    int board[BOARD_H][BOARD_W];   /* 0=empty, 1-7=block color */

    /* falling piece */
    int piece_type;    /* PieceType */
    int piece_rot;     /* 0-3 */
    int piece_r, piece_c;  /* pivot row, col */
    int piece_item_idx;  /* 0-3 index of cell with item, -1 if none */
    int piece_item_type; /* 0=None, 1=Bomb, 2=Drill, 3=Shield, 4=Gun */

    int next_type;     /* next piece preview */

    /* character */
    Character ch;

    /* scoring */
    int score;
    int level;
    int lines;

    int game_over;
    int game_started;  /* both players connected? */
    int attacker_hp;
    int attacker_stun_timer;
    int attacker_spawn_delay;
    int num_bullets;
    int bullets[MAX_BULLETS][2];  /* [i][0]=col, [i][1]=row */
    int num_effects;
    VisualEffect effects[MAX_EFFECTS];
} GameState;

/* ──────────── Protocol ──────────── */
#define MSG_INPUT  1
#define MSG_STATE  2
#define MSG_ROLE   3

/* client → server */
typedef struct {
    int type;   /* MSG_INPUT */
    int key;    /* raw key code */
} MsgInput;

/* server → client (role assignment) */
typedef struct {
    int type;   /* MSG_ROLE */
    int role;   /* 0=tetris player, 1=character player */
} MsgRole;

/* server → client (game state) - sent as raw GameState struct */

/* ──────────── Input keys (unified codes) ──────────── */
#define KEY_NONE      0
/* Tetris player */
#define K_LEFT        1
#define K_RIGHT       2
#define K_ROTATE      3
#define K_SOFT_DROP   4
#define K_HARD_DROP   5
/* Character player */
#define K_CH_LEFT     11
#define K_CH_RIGHT    12
#define K_CH_UP       13
#define K_CH_DOWN     14
#define K_CH_PICKUP   15  /* pick up / place block */
#define K_CH_JUMP     16
#define K_CH_ITEM     17  /* use item */
/* Common */
#define K_QUIT        99

/* ──────────── Helpers ──────────── */
static inline int send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, (const char*)buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static inline int recv_all(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char*)buf + got, len - got);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

#endif /* COMMON_H */
