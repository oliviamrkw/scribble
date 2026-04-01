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
    
    int word_guessed;  /* Has the word been guessed this round? */
    
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

/* Validate a guess (case-insensitive) */
int game_validate_guess(game_state_t *game, const char *guess);

/* Get points for a guesser */
uint32_t game_get_guesser_points(game_state_t *game, int is_first_guesser);

/* Get points for artist */
uint32_t game_get_artist_points(game_state_t *game, int word_was_guessed);

/* Mark that a player has guessed correctly */
void game_mark_guessed(game_state_t *game, uint32_t player_id);

/* Get the current artist ID */
uint32_t game_get_artist(game_state_t *game);

/* Get the current secret word (only give to artist!) */
const char* game_get_secret_word(game_state_t *game);

#endif // GAME_H