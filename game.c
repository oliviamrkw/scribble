#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "game.h"

/*
 * Remove all characters that are not printable ASCII (letters, spaces, etc).
 * Keeps only bytes 32-126. This eliminates \r, \n, and any other junk.
 */
static void clean_string(char *str)
{
    int j = 0;
    for (int i = 0; str[i]; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 32 && c <= 126)
            str[j++] = str[i];
    }
    str[j] = '\0';
    /* Also trim trailing spaces */
    while (j > 0 && str[j - 1] == ' ')
        str[--j] = '\0';
}

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
    game->num_guessed = 0;
    game->round_active = 0;
    game->game_started = 0;

    srand((unsigned)time(NULL));

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
        
        /* Strip all trailing whitespace and control characters */
        clean_string(line);
        size_t len = strlen(line);

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
    game->num_guessed = 0;
    game->round_active = 1;

    /* Reset has_guessed flag for all players */
    for (uint32_t i = 0; i < game->num_players; i++) {
        game->players[i].has_guessed = 0;
        game->players[i].is_artist = 0;
    }

    /* Pick artist: round 1 -> index 0 (first player), round 2 -> index 1, etc. */
    uint32_t artist_idx = (game->round_num - 1) % game->num_players;
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
    clean_string(game->secret_word);

    printf("Round %u started: artist is player %u, word is \"%s\"\n",
           game->round_num, game->artist_id, game->secret_word);
}

/*
 * Validate a guess (case-insensitive comparison).
 * Returns 1 if correct, 0 if incorrect.
 */
int game_validate_guess(game_state_t *game, const char *guess)
{
    char guess_clean[MAX_NAME_LEN];
    char word_clean[MAX_NAME_LEN];

    strncpy(guess_clean, guess, MAX_NAME_LEN - 1);
    guess_clean[MAX_NAME_LEN - 1] = '\0';
    clean_string(guess_clean);
    to_uppercase(guess_clean);

    strncpy(word_clean, game->secret_word, MAX_NAME_LEN - 1);
    word_clean[MAX_NAME_LEN - 1] = '\0';
    clean_string(word_clean);
    to_uppercase(word_clean);

    return strcmp(guess_clean, word_clean) == 0;
}

/*
 * Get points for the next correct guesser.
 * First guesser: 10, second: 9, third: 8, ... minimum 5.
 */
uint32_t game_get_guesser_points(game_state_t *game)
{
    uint32_t points = 10 - game->num_guessed;
    if (points < 5)
        points = 5;
    return points;
}

/*
 * Get points awarded to the artist for a single correct guess.
 * First correct guesser: +5, each subsequent: +1.
 * Called BEFORE game_mark_guessed increments num_guessed.
 */
uint32_t game_get_artist_points_for_guess(game_state_t *game)
{
    return (game->num_guessed == 0) ? 5 : 1;
}

/*
 * Mark that a player has guessed correctly.
 */
void game_mark_guessed(game_state_t *game, uint32_t player_id)
{
    for (uint32_t i = 0; i < game->num_players; i++) {
        if (game->players[i].id == player_id) {
            game->players[i].has_guessed = 1;
            game->num_guessed++;
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

/*
 * Check if all non-artist players have guessed correctly.
 * Returns 1 if round should end, 0 otherwise.
 */
int game_all_guessed(game_state_t *game)
{
    /* num_players - 1 because the artist doesn't guess */
    return game->num_guessed >= game->num_players - 1;
}

/*
 * Check if all rounds are done (every player has drawn once).
 */
int game_is_over(game_state_t *game)
{
    return game->round_num >= game->total_rounds;
}

/*
 * Build underscore hint: "_ _ _ _ _" for a 5-letter word.
 * Spaces in the word are preserved as spaces; all other chars become "_ ".
 */
void game_get_hint(game_state_t *game, char *buf, size_t buflen)
{
    size_t pos = 0;
    for (size_t i = 0; game->secret_word[i] && pos + 2 < buflen; i++) {
        if (game->secret_word[i] == ' ') {
            buf[pos++] = ' ';
            buf[pos++] = ' ';
        } else {
            if (i > 0)
                buf[pos++] = ' ';
            buf[pos++] = '_';
        }
    }
    buf[pos] = '\0';
}