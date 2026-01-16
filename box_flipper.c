#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 128
#define SCREEN_H 64

// Ring / arena layout
#define RING_TOP 10
#define RING_BOTTOM 58
#define RING_LEFT 6
#define RING_RIGHT 121

// Fighter dimensions (sprites are 16x24)
#define FIGHTER_W 16
#define FIGHTER_H 24

#define PLAYER_Y (RING_BOTTOM - FIGHTER_H - 2)
#define ENEMY_Y  (RING_TOP + 6)

// Timing (ms)
#define FRAME_MS 33
#define HIT_STUN_MS 260

// Movement
#define PLAYER_DODGE_OFFSET 20
#define ENEMY_SHUFFLE_RANGE 5
#define ENEMY_SHUFFLE_STEP  1

// Combat
#define MAX_HP 10
#define PUNCH_RANGE 16

// Messages
#define MSG_MS 2000

typedef enum {
    FighterStateIdle = 0,
    FighterStateTelegraph,   // enemy flashes before punching
    FighterStatePunching,
    FighterStateHitStun,
    FighterStateDodging,     // player dodge left/right then returns
    FighterStateKO,
} FighterState;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t home_x;

    FighterState state;
    uint32_t state_until_ms;

    uint8_t hp;
    uint8_t max_hp;

    // Flash effect (enemy telegraph)
    bool flash;
    uint32_t flash_next_ms;

    // Dodge (player)
    int8_t dodge_dir; // -1 left, +1 right

    // Enemy: after telegraph ends, punch must happen
    bool pending_punch;

} Fighter;

typedef struct {
    const char* name;

    uint8_t enemy_hp;

    uint16_t telegraph_ms;
    uint16_t punch_ms;
    uint16_t vulnerable_ms;

    uint16_t ai_base_delay;
    uint16_t ai_rand_delay;

    uint8_t punch_chance_near; // %
    uint8_t punch_chance_far;  // %

    // Difficulty helpers
    uint8_t player_damage;      // how much damage the player deals per successful hit
    bool telegraph_hittable;    // if true, player can hit enemy during telegraph

} BossDef;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    bool running;

    Fighter player;
    Fighter enemy;

    // Boss progression
    uint8_t boss_index; // 0..2
    BossDef bosses[3];

    // Enemy vulnerability window
    uint32_t enemy_vulnerable_until_ms;

    // Enemy AI timers
    uint32_t enemy_next_action_ms;
    uint32_t enemy_next_shuffle_ms;

    // Message
    bool show_msg;
    uint32_t msg_until_ms;
    const char* msg;

} App;

typedef struct {
    InputEvent event;
} InputEventWrap;

static uint32_t now_ms(void) {
    return furi_get_tick();
}

static int16_t abs16(int16_t v) {
    return (v < 0) ? -v : v;
}

static void clamp_i16(int16_t* v, int16_t lo, int16_t hi) {
    if(*v < lo) *v = lo;
    if(*v > hi) *v = hi;
}

static void set_msg(App* app, const char* msg, uint32_t duration_ms) {
    app->show_msg = true;
    app->msg = msg;
    app->msg_until_ms = now_ms() + duration_ms;
}

static void fighter_set_state(Fighter* f, FighterState st, uint32_t duration_ms) {
    f->state = st;
    f->state_until_ms = now_ms() + duration_ms;
}

static void fighter_update_state(Fighter* f) {
    if(f->state == FighterStateKO) return;

    // Flash toggling during telegraph
    if(f->state == FighterStateTelegraph) {
        uint32_t t = now_ms();
        if(t >= f->flash_next_ms) {
            f->flash = !f->flash;
            f->flash_next_ms = t + 80; // slower blink = more noticeable
        }
    }

    // End state by timeout
    if(f->state != FighterStateIdle && now_ms() >= f->state_until_ms) {
        if(f->state == FighterStateDodging) {
            f->x = f->home_x; // auto-return
        }
        f->state = FighterStateIdle;
    }
}

static void draw_hp_bar(Canvas* canvas, int x, int y, int w, uint8_t hp, uint8_t max_hp) {
    canvas_draw_frame(canvas, x, y, w, 6);
    int inner_w = w - 2;
    int fill = (inner_w * hp) / max_hp;
    if(fill < 0) fill = 0;
    if(fill > inner_w) fill = inner_w;
    canvas_draw_box(canvas, x + 1, y + 1, fill, 4);
}

static void draw_ring(Canvas* canvas) {
    canvas_draw_frame(canvas, RING_LEFT, RING_TOP, (RING_RIGHT - RING_LEFT), (RING_BOTTOM - RING_TOP));

    int rope1 = RING_TOP + 3;
    int rope2 = RING_TOP + 6;
    int rope3 = RING_TOP + 9;
    canvas_draw_line(canvas, RING_LEFT + 2, rope1, RING_RIGHT - 2, rope1);
    canvas_draw_line(canvas, RING_LEFT + 2, rope2, RING_RIGHT - 2, rope2);
    canvas_draw_line(canvas, RING_LEFT + 2, rope3, RING_RIGHT - 2, rope3);

    canvas_draw_box(canvas, RING_LEFT, RING_TOP, 2, 6);
    canvas_draw_box(canvas, RING_RIGHT - 2, RING_TOP, 2, 6);
    canvas_draw_box(canvas, RING_LEFT, RING_BOTTOM - 6, 2, 6);
    canvas_draw_box(canvas, RING_RIGHT - 2, RING_BOTTOM - 6, 2, 6);
}

static bool enemy_is_vulnerable(const App* app) {
    return now_ms() < app->enemy_vulnerable_until_ms;
}

/* =========================================================
   16x24 XBM Sprites (48 bytes each)
   ========================================================= */

// PLAYER (idle)
static const uint8_t spr_p_idle1[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0x10,0x02, 0xB8,0x02, 0x10,0x02, 0xE0,0x01, 0x00,0x00,
    0x20,0x04, 0xF0,0x07, 0x20,0x04, 0x20,0x04, 0x70,0x07, 0x20,0x04,
    0x0C,0x30, 0x1E,0x78, 0x0C,0x30,
    0x20,0x04, 0x20,0x04, 0x60,0x03, 0x60,0x03,
    0xE0,0x03, 0xF0,0x07, 0x00,0x00
};

static const uint8_t spr_p_idle2[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0x10,0x02, 0xA8,0x02, 0x10,0x02, 0xE0,0x01, 0x00,0x00,
    0x20,0x04, 0xF0,0x07, 0x20,0x04, 0x20,0x04, 0x70,0x07, 0x20,0x04,
    0x0C,0x30, 0x1E,0x78, 0x0C,0x30,
    0x20,0x04, 0x20,0x04, 0x60,0x03, 0x60,0x03,
    0xE0,0x03, 0xF0,0x07, 0x00,0x00
};

// PLAYER (uppercut punch - fist goes UP, not sideways)
static const uint8_t spr_p_punch_up[] = {
    0x00,0x00, 0x18,0x00, 0x3C,0x00, 0x18,0x00, 0xE0,0x01, 0x10,0x02, 0xB8,0x02, 0x10,0x02,
    0xE0,0x01, 0x00,0x00, 0x20,0x04, 0xF0,0x07, 0x20,0x04, 0x20,0x04,
    0x70,0x07, 0x20,0x04, 0x0C,0x30, 0x0C,0x30, 0x0C,0x30, 0x20,0x04,
    0x20,0x04, 0x60,0x03, 0x60,0x03, 0xE0,0x03, 0xF0,0x07, 0x00,0x00
};

// PLAYER (dodge)
static const uint8_t spr_p_dodge[] = {
    0x00,0x00, 0xE0,0x01, 0x10,0x02, 0xB8,0x02, 0x10,0x02, 0xE0,0x01, 0x00,0x00, 0x00,0x00,
    0x10,0x02, 0xF8,0x07, 0x10,0x02, 0x10,0x02, 0x38,0x03, 0x10,0x02,
    0x06,0x18, 0x0F,0x3C, 0x06,0x18,
    0x10,0x02, 0x10,0x02, 0x30,0x01, 0x30,0x01,
    0x70,0x01, 0xF8,0x03, 0x00,0x00
};

// BOSS 1 (easy)
static const uint8_t b1_idle1[] = {
    0x00,0x00, 0x00,0x00, 0xC0,0x01, 0x20,0x02, 0x60,0x02, 0x20,0x02, 0xC0,0x01, 0x00,0x00,
    0x20,0x04, 0xE0,0x07, 0x20,0x04, 0x20,0x04, 0xE0,0x07, 0x20,0x04,
    0x08,0x10, 0x1C,0x38, 0x08,0x10,
    0x20,0x04, 0x20,0x04, 0x40,0x02, 0x40,0x02,
    0xC0,0x03, 0xE0,0x07, 0x00,0x00
};

static const uint8_t b1_idle2[] = {
    0x00,0x00, 0x00,0x00, 0xC0,0x01, 0x20,0x02, 0x40,0x02, 0x20,0x02, 0xC0,0x01, 0x00,0x00,
    0x20,0x04, 0xE0,0x07, 0x20,0x04, 0x20,0x04, 0xE0,0x07, 0x20,0x04,
    0x08,0x10, 0x1C,0x38, 0x08,0x10,
    0x20,0x04, 0x20,0x04, 0x40,0x02, 0x40,0x02,
    0xC0,0x03, 0xE0,0x07, 0x00,0x00
};

static const uint8_t b1_punch[] = {
    0x00,0x00, 0x00,0x00, 0xC0,0x01, 0x20,0x02, 0x60,0x02, 0x20,0x02, 0xC0,0x01, 0x00,0x00,
    0x20,0x04, 0xE0,0x07, 0x20,0x04, 0x20,0x04, 0xE0,0x07, 0x20,0x04,
    0x08,0x00, 0x1C,0x00, 0x7F,0x00,
    0x20,0x04, 0x20,0x04, 0x40,0x02, 0x40,0x02,
    0xC0,0x03, 0xE0,0x07, 0x00,0x00
};

static const uint8_t b1_hurt[] = {
    0x00,0x00, 0xC0,0x01, 0x20,0x02, 0x60,0x02, 0x20,0x02, 0xC0,0x01, 0x00,0x00, 0x00,0x00,
    0x20,0x04, 0xC0,0x03, 0x20,0x04, 0x20,0x04, 0xC0,0x03, 0x20,0x04,
    0x18,0x18, 0x00,0x00, 0x18,0x18,
    0x20,0x04, 0x20,0x04, 0x40,0x02, 0x40,0x02,
    0xC0,0x03, 0xE0,0x07, 0x00,0x00
};

// BOSS 2 (normal)
static const uint8_t b2_idle1[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0x90,0x02, 0xF8,0x03, 0x90,0x02, 0xE0,0x01, 0x00,0x00,
    0x20,0x04, 0xF8,0x0F, 0x20,0x04, 0x20,0x04, 0xF8,0x0F, 0x20,0x04,
    0x1C,0x38, 0x3E,0x7C, 0x1C,0x38,
    0x20,0x04, 0x20,0x04, 0x60,0x03, 0x60,0x03,
    0xF0,0x07, 0xF8,0x0F, 0x00,0x00
};

static const uint8_t b2_idle2[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0xD0,0x02, 0xF8,0x03, 0xD0,0x02, 0xE0,0x01, 0x00,0x00,
    0x20,0x04, 0xF8,0x0F, 0x20,0x04, 0x20,0x04, 0xF8,0x0F, 0x20,0x04,
    0x1C,0x38, 0x3E,0x7C, 0x1C,0x38,
    0x20,0x04, 0x20,0x04, 0x60,0x03, 0x60,0x03,
    0xF0,0x07, 0xF8,0x0F, 0x00,0x00
};

static const uint8_t b2_punch[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0x90,0x02, 0xF8,0x03, 0x90,0x02, 0xE0,0x01, 0x00,0x00,
    0x20,0x04, 0xF8,0x0F, 0x20,0x04, 0x20,0x04, 0xF8,0x0F, 0x20,0x04,
    0x1C,0x00, 0x3E,0x00, 0xFF,0x01,
    0x20,0x04, 0x20,0x04, 0x60,0x03, 0x60,0x03,
    0xF0,0x07, 0xF8,0x0F, 0x00,0x00
};

static const uint8_t b2_hurt[] = {
    0x00,0x00, 0xE0,0x01, 0x10,0x02, 0xF8,0x03, 0x10,0x02, 0xE0,0x01, 0x00,0x00, 0x00,0x00,
    0x20,0x04, 0xF0,0x07, 0x20,0x04, 0x20,0x04, 0xF0,0x07, 0x20,0x04,
    0x18,0x18, 0x3C,0x3C, 0x18,0x18,
    0x20,0x04, 0x20,0x04, 0x60,0x03, 0x60,0x03,
    0xF0,0x07, 0xF8,0x0F, 0x00,0x00
};

// BOSS 3 (final/hard)
static const uint8_t b3_idle1[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0xD0,0x02, 0xF8,0x03, 0xD0,0x02, 0xE0,0x01, 0x00,0x00,
    0x70,0x0E, 0xFC,0x1F, 0x70,0x0E, 0x70,0x0E, 0xFC,0x1F, 0x70,0x0E,
    0x3C,0x3C, 0x7E,0x7E, 0x3C,0x3C,
    0x70,0x0E, 0x70,0x0E, 0xE0,0x07, 0xE0,0x07,
    0xF8,0x0F, 0xFC,0x1F, 0x00,0x00
};

static const uint8_t b3_idle2[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0x90,0x02, 0xF8,0x03, 0x90,0x02, 0xE0,0x01, 0x00,0x00,
    0x70,0x0E, 0xFC,0x1F, 0x70,0x0E, 0x70,0x0E, 0xFC,0x1F, 0x70,0x0E,
    0x3C,0x3C, 0x7E,0x7E, 0x3C,0x3C,
    0x70,0x0E, 0x70,0x0E, 0xE0,0x07, 0xE0,0x07,
    0xF8,0x0F, 0xFC,0x1F, 0x00,0x00
};

static const uint8_t b3_punch[] = {
    0x00,0x00, 0x00,0x00, 0xE0,0x01, 0xD0,0x02, 0xF8,0x03, 0xD0,0x02, 0xE0,0x01, 0x00,0x00,
    0x70,0x0E, 0xFC,0x1F, 0x70,0x0E, 0x70,0x0E, 0xFC,0x1F, 0x70,0x0E,
    0x3C,0x00, 0x7E,0x00, 0xFF,0x7F,
    0x70,0x0E, 0x70,0x0E, 0xE0,0x07, 0xE0,0x07,
    0xF8,0x0F, 0xFC,0x1F, 0x00,0x00
};

static const uint8_t b3_hurt[] = {
    0x00,0x00, 0xE0,0x01, 0x10,0x02, 0xF8,0x03, 0x10,0x02, 0xE0,0x01, 0x00,0x00, 0x00,0x00,
    0x70,0x0E, 0xF8,0x0F, 0x70,0x0E, 0x70,0x0E, 0xF8,0x0F, 0x70,0x0E,
    0x18,0x18, 0x00,0x00, 0x18,0x18,
    0x70,0x0E, 0x70,0x0E, 0xE0,0x07, 0xE0,0x07,
    0xF8,0x0F, 0xFC,0x1F, 0x00,0x00
};

static const uint8_t* boss_sprite_idle1(uint8_t bi) {
    return (bi == 0) ? b1_idle1 : (bi == 1) ? b2_idle1 : b3_idle1;
}
static const uint8_t* boss_sprite_idle2(uint8_t bi) {
    return (bi == 0) ? b1_idle2 : (bi == 1) ? b2_idle2 : b3_idle2;
}
static const uint8_t* boss_sprite_punch(uint8_t bi) {
    return (bi == 0) ? b1_punch : (bi == 1) ? b2_punch : b3_punch;
}
static const uint8_t* boss_sprite_hurt(uint8_t bi) {
    return (bi == 0) ? b1_hurt : (bi == 1) ? b2_hurt : b3_hurt;
}

static void draw_fighter(Canvas* canvas, const App* app, const Fighter* f, bool is_player) {
    bool alt = ((now_ms() / 200) & 1);

    if(is_player) {
        if(f->state == FighterStatePunching) {
            // Slightly higher to emphasize "up" punch
            canvas_draw_xbm(canvas, f->x, f->y - 1, FIGHTER_W, FIGHTER_H, spr_p_punch_up);
        } else if(f->state == FighterStateDodging) {
            canvas_draw_xbm(canvas, f->x, f->y, FIGHTER_W, FIGHTER_H, spr_p_dodge);
        } else {
            canvas_draw_xbm(canvas, f->x, f->y, FIGHTER_W, FIGHTER_H, alt ? spr_p_idle2 : spr_p_idle1);
        }
    } else {
        // Enemy telegraph: blink a frame around him for a BIG visual cue
        if(f->state == FighterStateTelegraph && f->flash) {
            canvas_draw_frame(canvas, f->x - 2, f->y - 2, FIGHTER_W + 4, FIGHTER_H + 4);
        }

        if(f->state == FighterStatePunching) {
            canvas_draw_xbm(canvas, f->x, f->y, FIGHTER_W, FIGHTER_H, boss_sprite_punch(app->boss_index));
        } else if(f->state == FighterStateHitStun) {
            canvas_draw_xbm(canvas, f->x, f->y, FIGHTER_W, FIGHTER_H, boss_sprite_hurt(app->boss_index));
        } else {
            canvas_draw_xbm(canvas, f->x, f->y, FIGHTER_W, FIGHTER_H,
                alt ? boss_sprite_idle2(app->boss_index) : boss_sprite_idle1(app->boss_index));
        }

        // Indicators (kept tight to enemy to avoid random "floating pixels")
        bool vuln = enemy_is_vulnerable(app);
        bool telegraph_hittable = (app->bosses[app->boss_index].telegraph_hittable && f->state == FighterStateTelegraph);
        if(vuln || telegraph_hittable) {
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, f->x + 3, f->y - 3, "!!");
        }
    }
}

static void app_draw(Canvas* canvas, void* ctx) {
    App* app = ctx;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontSecondary);

    draw_hp_bar(canvas, 2, 0, 56, app->enemy.hp, app->enemy.max_hp);
    canvas_draw_str(canvas, 4, 8, "ENEMY");

    draw_hp_bar(canvas, 70, 0, 56, app->player.hp, app->player.max_hp);
    canvas_draw_str(canvas, 74, 8, "YOU");

    const char* boss_name = app->bosses[app->boss_index].name ? app->bosses[app->boss_index].name : "";
    canvas_draw_str(canvas, 50, 8, boss_name);

    draw_ring(canvas);

    draw_fighter(canvas, app, &app->enemy, false);
    draw_fighter(canvas, app, &app->player, true);

    if(app->show_msg) {
        canvas_draw_box(canvas, 14, 24, 100, 16);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 18, 36, app->msg);
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 63, "< > dodge   OK punch");
}

static void input_cb(InputEvent* input_event, void* ctx) {
    App* app = ctx;
    InputEventWrap wrap = {.event = *input_event};
    furi_message_queue_put(app->input_queue, &wrap, 0);
}

static void init_bosses(App* app) {
    // Boss 1: Easy (more telegraph, you can hit during telegraph, hits deal more)
    app->bosses[0] = (BossDef){
        .name = "B1 EASY",
        .enemy_hp = 6,
        .telegraph_ms = 700,     // more time to react
        .punch_ms = 320,
        .vulnerable_ms = 1200,   // longer "open" window
        .ai_base_delay = 900,
        .ai_rand_delay = 800,
        .punch_chance_near = 40,
        .punch_chance_far = 8,
        .player_damage = 2,
        .telegraph_hittable = true,
    };

    // Boss 2: Normal (still easier than before)
    app->bosses[1] = (BossDef){
        .name = "B2 MED",
        .enemy_hp = 8,
        .telegraph_ms = 520,
        .punch_ms = 260,
        .vulnerable_ms = 900,
        .ai_base_delay = 700,
        .ai_rand_delay = 650,
        .punch_chance_near = 55,
        .punch_chance_far = 14,
        .player_damage = 2,
        .telegraph_hittable = true,
    };

    // Boss 3: Final (hard, unchanged style: must dodge to open)
    app->bosses[2] = (BossDef){
        .name = "B3 HARD",
        .enemy_hp = 10,
        .telegraph_ms = 260,
        .punch_ms = 220,
        .vulnerable_ms = 520,
        .ai_base_delay = 550,
        .ai_rand_delay = 500,
        .punch_chance_near = 78,
        .punch_chance_far = 22,
        .player_damage = 1,
        .telegraph_hittable = false,
    };
}

static void start_boss(App* app, uint8_t idx, bool reset_player_hp) {
    BossDef* b = &app->bosses[idx];

    int16_t home = (SCREEN_W / 2) - (FIGHTER_W / 2);

    if(reset_player_hp) {
        app->player.home_x = home;
        app->player.x = home;
        app->player.y = PLAYER_Y;
        app->player.hp = MAX_HP;
        app->player.max_hp = MAX_HP;
        app->player.flash = false;
        app->player.flash_next_ms = now_ms() + 60;
        app->player.dodge_dir = 0;
        app->player.pending_punch = false;
        fighter_set_state(&app->player, FighterStateIdle, 0);
    }

    app->enemy.home_x = home;
    app->enemy.x = home;
    app->enemy.y = ENEMY_Y;
    app->enemy.hp = b->enemy_hp;
    app->enemy.max_hp = b->enemy_hp;
    app->enemy.flash = false;
    app->enemy.flash_next_ms = now_ms() + 60;
    app->enemy.dodge_dir = 0;
    app->enemy.pending_punch = false;
    fighter_set_state(&app->enemy, FighterStateIdle, 0);

    app->enemy_vulnerable_until_ms = 0;

    app->enemy_next_action_ms = now_ms() + 700;
    app->enemy_next_shuffle_ms = now_ms() + 450;

    app->show_msg = false;
    app->msg = NULL;

    if(idx == 0) set_msg(app, "BOSS 1", 700);
    else if(idx == 1) set_msg(app, "BOSS 2", 700);
    else set_msg(app, "FINAL", 700);
}

static void do_enemy_punch(App* app) {
    if(app->player.state == FighterStateKO) return;
    if(app->enemy.state == FighterStateKO) return;

    if(app->enemy.state != FighterStateIdle) return;

    BossDef* b = &app->bosses[app->boss_index];

    fighter_set_state(&app->enemy, FighterStatePunching, b->punch_ms);

    int16_t px_center = app->player.x + FIGHTER_W / 2;
    int16_t ex_center = app->enemy.x + FIGHTER_W / 2;
    int16_t dx = abs16(px_center - ex_center);

    if(app->player.state == FighterStateDodging) {
        app->enemy_vulnerable_until_ms = now_ms() + b->vulnerable_ms;
        set_msg(app, "OPEN!", 350);
        return;
    }

    if(dx <= PUNCH_RANGE && app->player.state != FighterStateHitStun) {
        uint8_t dmg = 1;
        app->player.hp = (app->player.hp > dmg) ? (app->player.hp - dmg) : 0;
        fighter_set_state(&app->player, FighterStateHitStun, HIT_STUN_MS);
        set_msg(app, "HIT!", 350);

        if(app->player.hp == 0) {
            app->player.state = FighterStateKO;
            set_msg(app, "YOU LOSE...", MSG_MS);
        }
    }
}

static void advance_boss_or_win(App* app) {
    if(app->boss_index < 2) {
        app->boss_index++;
        set_msg(app, "NEXT!", 700);
        start_boss(app, app->boss_index, true);
    } else {
        set_msg(app, "YOU WIN!", MSG_MS);
    }
}

static bool player_can_hit_enemy_now(const App* app) {
    const BossDef* b = &app->bosses[app->boss_index];
    if(enemy_is_vulnerable(app)) return true;
    if(b->telegraph_hittable && app->enemy.state == FighterStateTelegraph) return true;
    return false;
}

static void do_player_punch(App* app) {
    if(app->player.state == FighterStateKO) return;
    if(app->enemy.state == FighterStateKO) return;

    if(app->player.state != FighterStateIdle) return;

    const BossDef* b = &app->bosses[app->boss_index];

    fighter_set_state(&app->player, FighterStatePunching, b->punch_ms);

    int16_t px_center = app->player.x + FIGHTER_W / 2;
    int16_t ex_center = app->enemy.x + FIGHTER_W / 2;
    int16_t dx = abs16(px_center - ex_center);

    if(dx > PUNCH_RANGE) return;

    if(!player_can_hit_enemy_now(app)) {
        set_msg(app, "BLOCK", 240);
        return;
    }

    // Successful hit
    if(app->enemy.state != FighterStateKO && app->enemy.state != FighterStateHitStun) {
        uint8_t dmg = b->player_damage;
        app->enemy.hp = (app->enemy.hp > dmg) ? (app->enemy.hp - dmg) : 0;

        // If you hit during telegraph, you cancel the punch (only on bosses where hittable)
        if(b->telegraph_hittable && app->enemy.state == FighterStateTelegraph) {
            app->enemy.pending_punch = false;
        }

        fighter_set_state(&app->enemy, FighterStateHitStun, HIT_STUN_MS);
        app->enemy_vulnerable_until_ms = 0;
        set_msg(app, "GOOD!", 300);

        if(app->enemy.hp == 0) {
            app->enemy.state = FighterStateKO;
            set_msg(app, "BOSS DOWN!", 800);
            advance_boss_or_win(app);
        }
    }
}

static void start_player_dodge(App* app, int8_t dir) {
    if(app->player.state == FighterStateKO) return;
    if(app->enemy.state == FighterStateKO) return;

    if(app->player.state != FighterStateIdle) return;

    app->player.dodge_dir = dir;

    int16_t target = app->player.home_x + (dir * PLAYER_DODGE_OFFSET);
    app->player.x = target;
    clamp_i16(&app->player.x, RING_LEFT + 3, RING_RIGHT - 3 - FIGHTER_W);

    uint16_t dodge_ms = 220;
    fighter_set_state(&app->player, FighterStateDodging, dodge_ms);
}

static void enemy_ai_step(App* app) {
    uint32_t t = now_ms();

    if(app->enemy.state == FighterStateKO || app->player.state == FighterStateKO) return;

    const BossDef* b = &app->bosses[app->boss_index];

    if(app->enemy.state == FighterStateIdle && t >= app->enemy_next_shuffle_ms) {
        if((rand() % 4) == 0) {
            int8_t step = (rand() & 1) ? +ENEMY_SHUFFLE_STEP : -ENEMY_SHUFFLE_STEP;
            int16_t newx = app->enemy.x + step;

            if(newx < app->enemy.home_x - ENEMY_SHUFFLE_RANGE) newx = app->enemy.home_x - ENEMY_SHUFFLE_RANGE;
            if(newx > app->enemy.home_x + ENEMY_SHUFFLE_RANGE) newx = app->enemy.home_x + ENEMY_SHUFFLE_RANGE;

            app->enemy.x = newx;
            clamp_i16(&app->enemy.x, RING_LEFT + 3, RING_RIGHT - 3 - FIGHTER_W);
        }
        app->enemy_next_shuffle_ms = t + 350 + (rand() % 400);
    }

    if(t < app->enemy_next_action_ms) return;

    if(app->enemy.state == FighterStateIdle) {
        int16_t px_center = app->player.x + FIGHTER_W / 2;
        int16_t ex_center = app->enemy.x + FIGHTER_W / 2;
        int16_t dx = abs16(px_center - ex_center);

        int punch_roll = rand() % 100;
        bool near = (dx <= (PUNCH_RANGE + 2));
        bool should_punch = near ? (punch_roll < b->punch_chance_near) : (punch_roll < b->punch_chance_far);

        if(should_punch) {
            app->enemy.flash = false;
            app->enemy.flash_next_ms = t + 80;
            app->enemy.pending_punch = true;
            fighter_set_state(&app->enemy, FighterStateTelegraph, b->telegraph_ms);

            app->enemy_next_action_ms = t + b->telegraph_ms + b->ai_base_delay + (rand() % b->ai_rand_delay);
        } else {
            app->enemy_next_action_ms = t + 300 + (rand() % 500);
        }
    }
}

static void reset_game(App* app) {
    init_bosses(app);
    app->boss_index = 0;
    start_boss(app, 0, true);
    set_msg(app, "DODGE / PUNCH", 900);
}

static void input_cb_queue(InputEvent* input_event, void* ctx) {
    input_cb(input_event, ctx);
}

int32_t box_flipper_app(void* p) {
    UNUSED(p);

    App* app = malloc(sizeof(App));
    furi_check(app);
    memset(app, 0, sizeof(App));

    srand((unsigned)now_ms());

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEventWrap));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, app_draw, app);
    view_port_input_callback_set(app->view_port, input_cb_queue, app);

    // IMPORTANT: reset_game BEFORE gui_add_view_port (prevents freeze/crash on first draw)
    app->running = true;
    reset_game(app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    uint32_t last_frame = now_ms();

    while(app->running) {
        InputEventWrap wrap;
        while(furi_message_queue_get(app->input_queue, &wrap, 0) == FuriStatusOk) {
            InputEvent* e = &wrap.event;

            if(e->type == InputTypeShort && e->key == InputKeyBack) {
                app->running = false;
                continue;
            }

            if(e->type == InputTypeShort && e->key == InputKeyOk) {
                if(app->player.state == FighterStateKO) {
                    reset_game(app);
                } else {
                    do_player_punch(app);
                }
                continue;
            }

            if(e->type == InputTypeShort && e->key == InputKeyLeft) {
                start_player_dodge(app, -1);
                continue;
            }

            if(e->type == InputTypeShort && e->key == InputKeyRight) {
                start_player_dodge(app, +1);
                continue;
            }
        }

        fighter_update_state(&app->player);
        fighter_update_state(&app->enemy);

        // If telegraph finished -> enemy becomes Idle -> execute punch
        if(app->enemy.pending_punch && app->enemy.state == FighterStateIdle) {
            app->enemy.pending_punch = false;
            do_enemy_punch(app);
        }

        if(app->show_msg && now_ms() >= app->msg_until_ms) {
            app->show_msg = false;
        }

        enemy_ai_step(app);

        uint32_t t = now_ms();
        if(t - last_frame >= FRAME_MS) {
            last_frame = t;
            view_port_update(app->view_port);
        } else {
            furi_delay_ms(2);
        }
    }

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);

    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    free(app);

    return 0;
}
