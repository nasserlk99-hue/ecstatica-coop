/**
 * player.c
 *
 * Local two-player support. The engine has one hero and one set of global key
 * flags, so player 2 is a second instance of the hero actor driven by its own
 * input state. Before behaviour() runs for player 2 we swap the engine's key
 * globals and selected_thing; afterwards everything is restored, leaving the
 * camera, HUD and scripts on player 1.
 *
 * Player 1 input: WASD only (plus the other keys on the left hand), never
 * arrows or numpad — those belong to player 2. Gamepad 0 also drives P1
 * (window_proc already merges it, so it comes through the globals).
 *
 * Player 2 input: the engine's own arrows + numpad mapping, so P2 can be
 * played one-handed on the right side. It also gets a second gamepad when
 * the platform exposes one. It can no longer use T/F/H/K/U/O/Y/J, because in
 * this layout those sit under P1's left hand.
 */

#include "player.h"
#include "display.h"
#include "edit.h"
#include "game.h"
#include "init.h"
#include "menu.h"
#include "platform.h"
#include "topo.h"
#include "win.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

player_t coop_players[COOP_MAX_PLAYERS];
int coop_num_players = 0;

static bool spawn_pending;
static bool remove_pending;
static bool input_swapped;

static actor_t *saved_selected;
static struct {
    char extra[256];
    char keys[256];
    bool space_pressed, ctrl_pressed, alt_pressed;
    bool key1, key2, key3, key4, key5, key6, key7, key8, key9;
    int16_t joystick;
    int16_t joy_button;
} saved_keys;

bool coop_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("ECSTATICA_COOP");
        enabled = (e && *e == '0') ? 0 : 1;
    }
    return enabled != 0;
}

static bool actor_in_display_list(const actor_t *actor) {
    for (actor_t *a = root_thing; a; a = a->next_in_display_list)
        if (a == actor) return true;
    return false;
}

static bool actor_in_thing_list(const actor_t *actor) {
    for (actor_t *a = thing_list; a; a = a->next_thing1)
        if (a == actor) return true;
    return false;
}

static bool actor_alive(const actor_t *actor) {
    return actor && (actor_in_display_list(actor) || actor_in_thing_list(actor));
}

bool coop_is_player_actor(const actor_t *actor) {
    if (!actor) return false;
    for (int i = 0; i < COOP_MAX_PLAYERS; i++)
        if (coop_players[i].active && coop_players[i].actor == actor)
            return true;
    return false;
}

bool coop_is_player2_actor(const actor_t *actor) {
    return actor && coop_players[1].active && coop_players[1].actor == actor;
}

static int16_t direction_index(const player_input_t *in) {
    if (in->up && in->left) return 7;
    if (in->up && in->right) return 1;
    if (in->down && in->left) return 5;
    if (in->down && in->right) return 3;
    if (in->up) return 0;
    if (in->right) return 2;
    if (in->down) return 4;
    if (in->left) return 6;
    return 1000;
}

static void capture_player1_input(void) {
    player_input_t *in = &coop_players[0].input;
    /* Capture before trusting the globals: if a previous frame ended with
     * P2's keys swapped in, these still hold P2's state. */
    if (!input_swapped) {
        /* P1 never uses arrows or numpad — those are P2's. Read P1's keys
         * raw from the platform so that holding P1's Alt is not shadowed by
         * the Windows menu bar. */
        platform_t *p1p = win_platform();
        bool w_down = false, a_down = false, s_down = false, d_down = false;
        bool left_ctrl = false, left_shift = false, space = false;
        bool e_down = false, r_down = false, f_down = false, c_down = false;
        if (p1p) {
            w_down = platform_key_down(p1p, PKEY_W);
            a_down = platform_key_down(p1p, PKEY_A);
            s_down = platform_key_down(p1p, PKEY_S);
            d_down = platform_key_down(p1p, PKEY_D);
            e_down = platform_key_down(p1p, PKEY_E);
            r_down = platform_key_down(p1p, PKEY_R);
            f_down = platform_key_down(p1p, PKEY_F);
            c_down = platform_key_down(p1p, PKEY_C);
            left_ctrl = platform_key_down(p1p, PKEY_LCTRL);
            left_shift = platform_key_down(p1p, PKEY_LSHIFT);
            space = platform_key_down(p1p, PKEY_SPACE);
        }

        /* E1 two-hand keys are their own actions, not movement, like the
         * original (NUM7/9 quick swing, NUM1/3 pick up). P1's hand keys are
         * E / R for the left hand, F / C for the right hand. */
        in->up = w_down;
        in->down = s_down;
        in->left = a_down;
        in->right = d_down;
        in->use = space;
        in->run = left_ctrl;
        in->alt = alt_pressed || platform_key_down(p1p, PKEY_LALT);
        in->jump = left_shift;
        in->pickup_left = e_down;
        in->pickup_right = f_down;
        in->swing_left = r_down;
        in->swing_right = c_down;
    }
    /* Gamepad 0 (the one window_proc already merged into the globals) always
     * drives P1, even straight after a botched swap. */
    platform_t *p = win_platform();
    if (p) {
        platform_gamepad_state_t gp;
        if (platform_gamepad_poll_index(p, 0, &gp) && gp.connected) {
            int dz = GAMEPAD_STICK_DEADZONE;
            int ax = gp.left_x < 0 ? -gp.left_x : gp.left_x;
            int ay = gp.left_y < 0 ? -gp.left_y : gp.left_y;
            bool pushed = (int64_t)ax * ax + (int64_t)ay * ay > (int64_t)dz * dz;
            bool stick_vert = pushed && ay * 10 > ax * 24;
            bool stick_horz = pushed && ax * 10 > ay * 24;
            bool stick_diag = pushed && !stick_vert && !stick_horz;

            in->up    |= gp.dpad_up    || ((stick_vert || stick_diag) && gp.left_y > 0);
            in->down  |= gp.dpad_down  || ((stick_vert || stick_diag) && gp.left_y < 0);
            in->left  |= gp.dpad_left  || ((stick_horz || stick_diag) && gp.left_x < 0);
            in->right |= gp.dpad_right || ((stick_horz || stick_diag) && gp.left_x > 0);

            in->use  |= gp.btn_south;
            in->jump |= gp.btn_lb;
            in->run  |= gp.btn_rb || gp.btn_rt;
            in->alt  |= gp.btn_lt;
            in->pickup_left  |= gp.btn_west;
            in->pickup_right |= gp.btn_north;

            int rs_dz = GAMEPAD_STICK_DEADZONE;
            in->swing_left  |= gp.right_x < -rs_dz;
            in->swing_right |= gp.right_x >  rs_dz;
        }
    }
}

static void poll_player2_input(void) {
    player_input_t *in = &coop_players[1].input;
    memset(in, 0, sizeof(*in));

    platform_t *p = win_platform();
    if (!p) return;

    /* P2 owns the engine's own arrows + numpad layout, so it can be played
     * one-handed on the right side. It reads the platform directly (not the
     * globals, which hold P1's input). P2 has its OWN modifiers and use key
     * (NUM0/NUM./NUM+/NUM5) so that pressing Space/Ctrl/Shift/Alt for P1
     * never triggers P2's actions. */
    in->up    |= platform_key_down(p, PKEY_UP)    || platform_key_down(p, PKEY_NUM8);
    in->down  |= platform_key_down(p, PKEY_DOWN)  || platform_key_down(p, PKEY_NUM2);
    in->left  |= platform_key_down(p, PKEY_LEFT)  || platform_key_down(p, PKEY_NUM4);
    in->right |= platform_key_down(p, PKEY_RIGHT) || platform_key_down(p, PKEY_NUM6);
    in->use   |= platform_key_down(p, PKEY_NUM5);
    in->run   |= platform_key_down(p, PKEY_NUM0);
    in->alt   |= platform_key_down(p, PKEY_NUMPLUS);
    in->jump  |= platform_key_down(p, PKEY_NUMDEC);
    in->pickup_left  |= platform_key_down(p, PKEY_NUM1);
    in->pickup_right |= platform_key_down(p, PKEY_NUM3);
    in->swing_left   |= platform_key_down(p, PKEY_NUM7);
    in->swing_right  |= platform_key_down(p, PKEY_NUM9);

    platform_gamepad_state_t gp;
    if (platform_gamepad_poll_index(p, 1, &gp) && gp.connected) {
        int dz = GAMEPAD_STICK_DEADZONE;
        int ax = gp.left_x < 0 ? -gp.left_x : gp.left_x;
        int ay = gp.left_y < 0 ? -gp.left_y : gp.left_y;
        bool pushed = (int64_t)ax * ax + (int64_t)ay * ay > (int64_t)dz * dz;
        bool stick_vert = pushed && ay * 10 > ax * 24;
        bool stick_horz = pushed && ax * 10 > ay * 24;
        bool stick_diag = pushed && !stick_vert && !stick_horz;

        in->up    |= gp.dpad_up    || ((stick_vert || stick_diag) && gp.left_y > 0);
        in->down  |= gp.dpad_down  || ((stick_vert || stick_diag) && gp.left_y < 0);
        in->left  |= gp.dpad_left  || ((stick_horz || stick_diag) && gp.left_x < 0);
        in->right |= gp.dpad_right || ((stick_horz || stick_diag) && gp.left_x > 0);

        in->use  |= gp.btn_south;
        in->jump |= gp.btn_lb;
        in->run  |= gp.btn_rb || gp.btn_rt;
        in->alt  |= gp.btn_lt;
        in->pickup_left  |= gp.btn_west;
        in->pickup_right |= gp.btn_north;

        int rs_dz = GAMEPAD_STICK_DEADZONE;
        in->swing_left  |= gp.right_x < -rs_dz;
        in->swing_right |= gp.right_x >  rs_dz;
    }
}

static void install_player_input(const player_input_t *in) {
    memset(extra_keys_pressed, 0, 256);
    memset(keys_pressed, 0, 256);

    extra_keys_pressed[72] = in->up;
    extra_keys_pressed[80] = in->down;
    extra_keys_pressed[75] = in->left;
    extra_keys_pressed[77] = in->right;
    extra_keys_pressed[56] = in->alt;
    extra_keys_pressed[57] = in->use;
    extra_keys_pressed[71] = in->swing_left;
    extra_keys_pressed[73] = in->swing_right;
    extra_keys_pressed[79] = in->pickup_left;
    extra_keys_pressed[81] = in->pickup_right;

    keys_pressed[42] = in->jump;

    key8_pressed = in->up;
    key2_pressed = in->down;
    key4_pressed = in->left;
    key6_pressed = in->right;
    key7_pressed = in->up && in->left;
    key9_pressed = in->up && in->right;
    key1_pressed = in->down && in->left;
    key3_pressed = in->down && in->right;
    key5_pressed = false;

    space_pressed = in->use;
    ctrl_pressed = in->run;
    alt_pressed = in->alt;
    joystick = direction_index(in);
    joy_button = in->use ? 1 : 0;
}

bool coop_begin_actor_input(actor_t *actor) {
    if (!coop_is_player2_actor(actor)) return false;

    saved_selected = selected_thing;
    memcpy(saved_keys.extra, extra_keys_pressed, sizeof(saved_keys.extra));
    memcpy(saved_keys.keys, keys_pressed, sizeof(saved_keys.keys));
    saved_keys.space_pressed = space_pressed;
    saved_keys.ctrl_pressed = ctrl_pressed;
    saved_keys.alt_pressed = alt_pressed;
    saved_keys.key1 = key1_pressed;
    saved_keys.key2 = key2_pressed;
    saved_keys.key3 = key3_pressed;
    saved_keys.key4 = key4_pressed;
    saved_keys.key5 = key5_pressed;
    saved_keys.key6 = key6_pressed;
    saved_keys.key7 = key7_pressed;
    saved_keys.key8 = key8_pressed;
    saved_keys.key9 = key9_pressed;
    saved_keys.joystick = joystick;
    saved_keys.joy_button = joy_button;

    install_player_input(&coop_players[1].input);
    selected_thing = actor;
    input_swapped = true;
    return true;
}

void coop_end_actor_input(void) {
    if (!input_swapped) return;
    input_swapped = false;

    memcpy(extra_keys_pressed, saved_keys.extra, sizeof(saved_keys.extra));
    memcpy(keys_pressed, saved_keys.keys, sizeof(saved_keys.keys));
    space_pressed = saved_keys.space_pressed;
    ctrl_pressed = saved_keys.ctrl_pressed;
    alt_pressed = saved_keys.alt_pressed;
    key1_pressed = saved_keys.key1;
    key2_pressed = saved_keys.key2;
    key3_pressed = saved_keys.key3;
    key4_pressed = saved_keys.key4;
    key5_pressed = saved_keys.key5;
    key6_pressed = saved_keys.key6;
    key7_pressed = saved_keys.key7;
    key8_pressed = saved_keys.key8;
    key9_pressed = saved_keys.key9;
    joystick = saved_keys.joystick;
    joy_button = saved_keys.joy_button;

    selected_thing = saved_selected;
}

static void remove_player2_now(void) {
    actor_t *p2 = coop_players[1].actor;
    remove_pending = false;
    if (!p2) {
        coop_players[1].active = false;
        return;
    }

    if (actor_alive(p2)) {
        int16_t idx = p2->name_index;
        actor_t *slot = (idx >= 0 && idx < THING_TAB_SIZE) ? thing_tab[idx] : NULL;

        if (actor_in_display_list(p2))
            remove_from_display_list(p2);
        /* do_delete_thing clears thing_tab[name_index]; put player 1 back. */
        do_delete_thing(p2);
        if (idx >= 0 && idx < THING_TAB_SIZE && thing_tab[idx] == NULL &&
            slot != p2)
            thing_tab[idx] = slot;
    }

    coop_players[1].actor = NULL;
    coop_players[1].active = false;
}

static void tint_player2(actor_t *p2) {
    /* Material indices index shade_tab[17], not the palette. A small rotation
     * of the material keeps the second hero recognisably dressed but visibly
     * different. Index 0 is left alone: it is used for transparent/unshaded
     * parts. */
    for (part_t *part = p2->actor_parts_list; part; part = part->next_in_display_list) {
        int m = part->color;
        if (m > 0 && m < 17)
            m = 1 + ((m + 4) % 16);
        part->color = (int16_t)m;
        part->default_color = (int16_t)m;
    }
}

static void spawn_player2(void) {
    spawn_pending = false;

    actor_t *p1 = coop_players[0].actor;
    if (!p1 || !actor_alive(p1)) return;
    if (p1->name_index < 0 || p1->name_index >= THING_TAB_SIZE) return;

    actor_t *p2 = coop_load_actor_instance(p1->name_index);
    if (!p2) {
        DBG_LOG(1, "[COOP] couldn't create player 2 from thing %d\n",
                (int)p1->name_index);
        return;
    }

    p2->actor_behavior = BH_JOYSTICK;
    p2->move_type = -1;
    p2->actor_rep_index = p1->actor_rep_index;
    p2->actor_Speed_factor = p1->actor_Speed_factor;
    p2->actor_strength_factor = p1->actor_strength_factor;
    p2->actor_hit_factor = p1->actor_hit_factor;
    p2->actor_magic_factor = p1->actor_magic_factor;
    p2->actor_magic = p1->actor_magic;
    p2->full_actor_hp = p1->full_actor_hp;
    p2->actor_hitpoints = p1->actor_hitpoints;
    p2->flags |= ACTOR_FLAG_VISIBLE;

    /* One body-width to the hero's right. sine/cos are the forward vector. */
    int16_t yaw = p1->rotate_vector.Y;
    int16_t rx = (int16_t)(cosn_table[(uint16_t)yaw] >> 6);
    int16_t rz = (int16_t)(-sine_table[(uint16_t)yaw] >> 6);

    copy_vector(&p2->position_vector, &p1->position_vector);
    p2->position_vector.X = (int16_t)(p2->position_vector.X + rx);
    p2->position_vector.Z = (int16_t)(p2->position_vector.Z + rz);
    copy_vector(&p2->rotate_vector, &p1->rotate_vector);
    copy_vector(&p2->previous_position, &p2->position_vector);
    copy_vector(&p2->start_position, &p2->position_vector);
    copy_vector(&p2->actor_center, &p2->position_vector);

    int16_t ground = find_height_now(&p2->position_vector, p2);
    if (abs((int)ground - (int)p1->position_vector.Y) < 2000)
        p2->position_vector.Y = ground;
    else
        p2->position_vector.Y = p1->position_vector.Y;
    copy_vector(&p2->previous_position, &p2->position_vector);

    add_to_display_list(p2);
    tint_player2(p2);

    coop_players[1].actor = p2;
    coop_players[1].active = true;
    coop_num_players = 2;

    DBG_LOG(1, "[COOP] player 2 spawned: thing=%d at %d,%d,%d (p1 %d,%d,%d)\n",
            (int)p2->name_index,
            p2->position_vector.X, p2->position_vector.Y, p2->position_vector.Z,
            p1->position_vector.X, p1->position_vector.Y, p1->position_vector.Z);
}

void coop_on_hero_loaded(int16_t hero_index) {
    spawn_pending = true;
    if (coop_players[1].actor)
        remove_pending = true;

    coop_players[0].actor =
        (hero_index >= 0 && hero_index < THING_TAB_SIZE) ? thing_tab[hero_index] : NULL;
    coop_players[0].active = coop_players[0].actor != NULL;
    coop_num_players = coop_players[0].active ? 1 : 0;

    DBG_LOG(1, "[COOP] hero loaded: index=%d actor=%p\n",
            (int)hero_index, (void *)coop_players[0].actor);
}

void coop_on_hero_restored(actor_t *hero) {
    if (!hero) return;
    coop_players[0].actor = hero;
    coop_players[0].active = true;
    coop_num_players = 1;
    spawn_pending = true;
    if (coop_players[1].actor)
        remove_pending = true;
    DBG_LOG(1, "[COOP] hero restored: thing=%d\n", (int)hero->name_index);
}

void coop_reset_game(void) {
    memset(coop_players, 0, sizeof(coop_players));
    coop_num_players = 0;
    spawn_pending = false;
    remove_pending = false;
    input_swapped = false;

    /* A menuless autostart (no intro) should still get player 2 as soon as
     * the game loop begins. */
    spawn_pending = true;
}

void coop_frame_update(void) {
    if (!coop_enabled()) {
        /* Terminal states call coop_frame_update() while the keys are still
         * swapped to player 2 (e.g. a menu/quit path returns without
         * coop_end_actor_input). Restore them so the next thing that reads the
         * engine globals can't observe P2's input. */
        if (input_swapped)
            coop_end_actor_input();
        return;
    }

    if (remove_pending)
        remove_player2_now();

    /* Recover player 1 if CT_LOAD_HERO was missed (e.g. autostart paths). */
    if (!coop_players[0].actor && selected_thing &&
        selected_thing->actor_behavior == BH_JOYSTICK) {
        coop_on_hero_restored(selected_thing);
    }

    if (coop_players[1].actor && !actor_alive(coop_players[1].actor)) {
        coop_players[1].actor = NULL;
        coop_players[1].active = false;
        coop_num_players = coop_players[0].active ? 1 : 0;
        spawn_pending = true;
    }

    capture_player1_input();
    poll_player2_input();

    /* Re-assert player 1's keys. collect_behaviour (get_mouse/window_proc)
     * may run other input readers before the first behaviour pass of the
     * frame (present_delay, menus), and a path that returns early from the
     * main loop can leave the globals swapped to player 2. The engine reads
     * keyboard/cursor/Alt state for its own UI first, then P1's behaviour()
     * reads it — so the freshest copy wins for P1. */
    if (coop_players[0].active)
        install_player_input(&coop_players[0].input);

    if (spawn_pending && !intro_flag && coop_players[0].actor)
        spawn_player2();
}
