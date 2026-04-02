#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include "net.h"

/* Maximum words in the bank */
#define MAX_WORDS 100

typedef struct {
    uint32_t id;
    char name[MAX_NAME_LEN];
    uint32_t score;
    int is_artist;
    int has_guessed;  /* Has this player guessed correctly this round? */
} player_t;

typedef struct {
    uint32_t round_num;
    char secret_word[MAX_NAME_LEN];
    uint32_t artist_id;
    
    player_t players[MAX_PLAYERS];
    uint32_t num_players;
    
    int word_guessed;       /* Has the word been guessed this round? */
    uint32_t num_guessed;   /* How many players have guessed correctly */
    int round_active;       /* Is a round currently in progress? */
    int game_started;       /* Has "play" been typed to start the game? */
    uint32_t total_rounds;  /* Total rounds = num_players when game started */

    /* Word bank */
    char *words[MAX_WORDS];
    uint32_t num_words;
} game_state_t;

/* Initialize game state */
game_state_t* game_init(void);

/* Clean up game state */
void game_cleanup(game_state_t *game);

/* Load words from file */
int game_load_words(game_state_t *game, const char *filename);

/* Add a player to the game */
int game_add_player(game_state_t *game, uint32_t id, const char *name);

/* Remove a player from the game */
void game_remove_player(game_state_t *game, uint32_t id);

/* Start a new round (pick random artist and word) */
void game_start_round(game_state_t *game);

/* Validate a guess (case-insensitive) — returns 1 if correct, 0 if wrong */
int game_validate_guess(game_state_t *game, const char *guess);

/* Get points for the next correct guesser (10, 9, 8, ... min 5) */
uint32_t game_get_guesser_points(game_state_t *game);

/* Get artist points for a correct guess (+5 first, +1 each after) */
uint32_t game_get_artist_points_for_guess(game_state_t *game);

/* Check if all guessers have guessed correctly */
int game_all_guessed(game_state_t *game);

/* Build underscore hint string for the secret word (e.g. "_ _ _ _ _") */
void game_get_hint(game_state_t *game, char *buf, size_t buflen);

/* Mark that a player has guessed correctly */
void game_mark_guessed(game_state_t *game, uint32_t player_id);

/* Get the current artist ID */
uint32_t game_get_artist(game_state_t *game);

/* Get the current secret word (only give to artist!) */
const char* game_get_secret_word(game_state_t *game);

/* Check if all rounds are done (every player has drawn once) */
int game_is_over(game_state_t *game);

#endif // GAME_H