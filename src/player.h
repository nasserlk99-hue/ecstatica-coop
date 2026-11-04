#ifndef PLAYER_H
#define PLAYER_H

#include "types.h"

/* Number of local players this fork supports. Online co-op will reuse slot 1
 * for the remote player, so the same input installation path drives both. */
#define COOP_MAX_PLAYERS 2

typedef struct player_input_s {
    bool up, down, left, right;
    bool use;          /* Space: reach, pick up, confirm */
    bool run;          /* Ctrl: run / attack with the stick (E2) */
    bool alt;          /* Alt: use what is held / magic (E2) */
    bool jump;         /* Shift: jump */
    bool pickup_left;  /* E1 Numpad 1 / Z: left hand */
    bool pickup_right; /* E1 Numpad 3 / C: right hand */
    bool swing_left;   /* E1 Numpad 7 / Q: quick left swing */
    bool swing_right;  /* E1 Numpad 9 / E: quick right swing */
} player_input_t;

typedef struct player_s {
    bool active;
    int index;
    actor_t *actor;
    player_input_t input;
} player_t;

extern player_t coop_players[COOP_MAX_PLAYERS];
extern int coop_num_players;

/* True when this build should offer local co-op. Controlled by
 * ECSTATICA_COOP=0 to disable; on by default. */
bool coop_enabled(void);

/* Called once per frame, after get_joystick() has filled the engine's input
 * globals. Snapshots player 1, polls player 2, and spawns/removes the second
 * hero when the scene is ready. */
void coop_frame_update(void);

/* Reset pointers when the game state is rebuilt (new game). */
void coop_reset_game(void);

/* The hero was (re)loaded by a CT_LOAD_HERO script. hero_index is its thing
 * table slot. Player 2 is recreated from the new hero on the next frame. */
void coop_on_hero_loaded(int16_t hero_index);

/* Called after a save is loaded and selected_thing has been resolved. */
void coop_on_hero_restored(actor_t *hero);

/* True when actor belongs to any local player. */
bool coop_is_player_actor(const actor_t *actor);
bool coop_is_player2_actor(const actor_t *actor);

/* Install/restore the engine key globals for one player's behaviour pass.
 * begin returns true when globals were swapped and end must be called. */
bool coop_begin_actor_input(actor_t *actor);
void coop_end_actor_input(void);

/* Load a second instance of a thing that is already in the world. Defined in
 * game.c because it touches the actor loader's static bookkeeping. */
actor_t *coop_load_actor_instance(int16_t actor_index);

#endif /* PLAYER_H */
