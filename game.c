#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "game.h"

/*
 * Convert a string to uppercase for case-insensitive comparison.
 */
static void to_uppercase(char *str)
{
    for (int i = 0; str[i]; i++) {
        str[i] = (char)toupper((unsigned char)str[i]);
    }
}

/*
 * Initialize the game state.
 */
game_state_t* game_init(void)
{
    game_state_t *game = malloc(sizeof(game_state_t));
    if (!game) {
        perror("malloc");
        return NULL;
    }

    memset(game, 0, sizeof(game_state_t));
    game->round_num = 0;
    game->num_players = 0;
    game->num_words = 0;
    game->word_guessed = 0;

    return game;
}

/*
 * Clean up the game state and free all resources.
 */
void game_cleanup(game_state_t *game)
{
    if (!game)
        return;

    /* Free words */
    for (uint32_t i = 0; i < game->num_words; i++) {
        if (game->words[i]) {
            free(game->words[i]);
        }
    }

    free(game);
}

/*
 * Load words from a file (one word per line).
 * Returns 0 on success, -1 on error.
 */
int game_load_words(game_state_t *game, const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("fopen");
        return -1;
    }

    char line[MAX_NAME_LEN];
    game->num_words = 0;

    while (fgets(line, sizeof(line), file) != NULL && 
           game->num_words < MAX_WORDS) {
        
        /* Strip trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        /* Skip empty lines */
        if (len == 0)
            continue;

        /* Allocate and store the word */
        game->words[game->num_words] = malloc(len + 1);
        if (!game->words[game->num_words]) {
            perror("malloc");
            fclose(file);
            return -1;
        }

        strcpy(game->words[game->num_words], line);
        game->num_words++;
    }

    fclose(file);

    if (game->num_words == 0) {
        fprintf(stderr, "No words loaded from %s\n", filename);
        return -1;
    }

    printf("Loaded %u words from %s\n", game->num_words, filename);
    return 0;
}

/*
 * Add a player to the game.
 * Returns 0 on success, -1 if game is full.
 */
int game_add_player(game_state_t *game, uint32_t id, const char *name)
{
    if (game->num_players >= MAX_PLAYERS) {
        fprintf(stderr, "Game is full\n");
        return -1;
    }

    uint32_t idx = game->num_players;
    game->players[idx].id = id;
    strncpy(game->players[idx].name, name, MAX_NAME_LEN - 1);
    game->players[idx].name[MAX_NAME_LEN - 1] = '\0';
    game->players[idx].score = 0;
    game->players[idx].is_artist = 0;
    game->players[idx].has_guessed = 0;

    game->num_players++;
    return 0;
}

/*
 * Remove a player from the game.
 */
void game_remove_player(game_state_t *game, uint32_t id)
{
    uint32_t idx = -1;

    /* Find the player */
    for (uint32_t i = 0; i < game->num_players; i++) {
        if (game->players[i].id == id) {
            idx = i;
            break;
        }
    }

    if (idx == (uint32_t)-1) {
        fprintf(stderr, "Player %u not found\n", id);
        return;
    }

    /* Shift remaining players down */
    for (uint32_t i = idx; i < game->num_players - 1; i++) {
        game->players[i] = game->players[i + 1];
    }

    game->num_players--;
}

/*
 * Start a new round: pick a random artist and word.
 * Simple algorithm: rotate artist (increment round number, pick next player).
 */
void game_start_round(game_state_t *game)
{
    if (game->num_players < 2) {
        fprintf(stderr, "Not enough players to start a round\n");
        return;
    }

    game->round_num++;
    game->word_guessed = 0;

    /* Reset has_guessed flag for all players */
    for (uint32_t i = 0; i < game->num_players; i++) {
        game->players[i].has_guessed = 0;
        game->players[i].is_artist = 0;
    }

    /* Pick artist: rotate by round number */
    uint32_t artist_idx = game->round_num % game->num_players;
    game->players[artist_idx].is_artist = 1;
    game->artist_id = game->players[artist_idx].id;

    /* Pick a random word */
    if (game->num_words == 0) {
        fprintf(stderr, "No words available\n");
        return;
    }

    uint32_t word_idx = rand() % game->num_words;
    strncpy(game->secret_word, game->words[word_idx], MAX_NAME_LEN - 1);
    game->secret_word[MAX_NAME_LEN - 1] = '\0';

    printf("Round %u started: artist is player %u, word is \"%s\"\n",
           game->round_num, game->artist_id, game->secret_word);
}

/*
 * Validate a guess (case-insensitive comparison).
 * Returns 1 if correct, 0 if incorrect.
 */
int game_validate_guess(game_state_t *game, const char *guess)
{
    if (game->word_guessed)
        return 0;  /* Already guessed this round */

    char guess_upper[MAX_NAME_LEN];
    char word_upper[MAX_NAME_LEN];

    strncpy(guess_upper, guess, MAX_NAME_LEN - 1);
    guess_upper[MAX_NAME_LEN - 1] = '\0';
    strncpy(word_upper, game->secret_word, MAX_NAME_LEN - 1);
    word_upper[MAX_NAME_LEN - 1] = '\0';

    to_uppercase(guess_upper);
    to_uppercase(word_upper);

    if (strcmp(guess_upper, word_upper) == 0) {
        game->word_guessed = 1;
        return 1;
    }

    return 0;
}

/*
 * Get points awarded to a guesser.
 * is_first_guesser: 1 if this is the first to guess correctly, 0 otherwise.
 */
uint32_t game_get_guesser_points(game_state_t *game, int is_first_guesser)
{
    (void)game;  /* Unused */
    return is_first_guesser ? 10 : 5;
}

/*
 * Get points awarded to the artist.
 * word_was_guessed: 1 if word was guessed, 0 if time ran out.
 */
uint32_t game_get_artist_points(game_state_t *game, int word_was_guessed)
{
    (void)game;  /* Unused */
    return word_was_guessed ? 5 : 0;
}

/*
 * Mark that a player has guessed correctly.
 */
void game_mark_guessed(game_state_t *game, uint32_t player_id)
{
    for (uint32_t i = 0; i < game->num_players; i++) {
        if (game->players[i].id == player_id) {
            game->players[i].has_guessed = 1;
            return;
        }
    }
}

/*
 * Get the current artist ID.
 */
uint32_t game_get_artist(game_state_t *game)
{
    return game->artist_id;
}

/*
 * Get the secret word (should only be called for the artist!).
 */
const char* game_get_secret_word(game_state_t *game)
{
    return game->secret_word;
}