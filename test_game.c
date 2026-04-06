#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "game.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (cond) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else { tests_failed++; printf("  FAIL: %s\n", msg); } \
} while (0)

static void test_init_cleanup(void)
{
    printf("\n--- test_init_cleanup ---\n");
    game_state_t *g = game_init();
    ASSERT(g != NULL, "game_init returns non-NULL");
    ASSERT(g->round_num == 0, "round_num starts at 0");
    ASSERT(g->num_players == 0, "num_players starts at 0");
    ASSERT(g->num_words == 0, "num_words starts at 0");
    ASSERT(g->word_guessed == 0, "word_guessed starts at 0");
    ASSERT(g->round_active == 0, "round_active starts at 0");
    ASSERT(g->num_guessed == 0, "num_guessed starts at 0");
    game_cleanup(g);
}

static void test_add_remove_players(void)
{
    printf("\n--- test_add_remove_players ---\n");
    game_state_t *g = game_init();

    ASSERT(game_add_player(g, 0, "Alice") == 0, "add Alice");
    ASSERT(g->num_players == 1, "num_players == 1");
    ASSERT(game_add_player(g, 1, "Bob") == 0, "add Bob");
    ASSERT(g->num_players == 2, "num_players == 2");

    for (uint32_t i = 2; i < MAX_PLAYERS; i++)
        game_add_player(g, i, "Extra");
    ASSERT(g->num_players == MAX_PLAYERS, "game is full");
    ASSERT(game_add_player(g, 99, "Overflow") == -1, "reject when full");

    game_remove_player(g, 0);
    ASSERT(g->num_players == MAX_PLAYERS - 1, "num_players after remove");

    game_remove_player(g, 99);
    ASSERT(g->num_players == MAX_PLAYERS - 1, "remove non-existent is no-op");

    game_cleanup(g);
}

static void test_load_words(void)
{
    printf("\n--- test_load_words ---\n");

    FILE *f = fopen("test_words.txt", "w");
    fprintf(f, "apple\nbanana\ncherry\n\ndog\n");
    fclose(f);

    game_state_t *g = game_init();
    ASSERT(game_load_words(g, "test_words.txt") == 0, "load words succeeds");
    ASSERT(g->num_words == 4, "loaded 4 words (skipped blank line)");
    ASSERT(strcmp(g->words[0], "apple") == 0, "first word is apple");
    ASSERT(strcmp(g->words[3], "dog") == 0, "last word is dog");

    game_state_t *g2 = game_init();
    ASSERT(game_load_words(g2, "no_such_file.txt") == -1, "missing file returns -1");

    game_cleanup(g);
    game_cleanup(g2);
    remove("test_words.txt");
}

static void test_start_round(void)
{
    printf("\n--- test_start_round ---\n");
    game_state_t *g = game_init();

    game_add_player(g, 0, "Solo");
    game_start_round(g);
    ASSERT(g->round_num == 0, "no round start with 1 player");

    game_add_player(g, 1, "Duo");
    FILE *f = fopen("test_words.txt", "w");
    fprintf(f, "cat\nhat\nmat\n");
    fclose(f);
    game_load_words(g, "test_words.txt");

    game_start_round(g);
    ASSERT(g->round_num == 1, "round_num incremented to 1");
    ASSERT(g->word_guessed == 0, "word_guessed reset");
    ASSERT(g->num_guessed == 0, "num_guessed reset");
    ASSERT(g->round_active == 1, "round_active set");
    ASSERT(strlen(g->secret_word) > 0, "secret_word is set");

    int artist_count = 0;
    for (uint32_t i = 0; i < g->num_players; i++) {
        if (g->players[i].is_artist) artist_count++;
        ASSERT(g->players[i].has_guessed == 0, "has_guessed reset");
    }
    ASSERT(artist_count == 1, "exactly one artist");

    uint32_t first_artist = g->artist_id;
    game_start_round(g);
    ASSERT(g->round_num == 2, "round_num incremented to 2");
    ASSERT(g->artist_id != first_artist, "artist rotated");

    game_cleanup(g);
    remove("test_words.txt");
}

static void test_guess_validation(void)
{
    printf("\n--- test_guess_validation ---\n");
    game_state_t *g = game_init();

    game_add_player(g, 0, "Alice");
    game_add_player(g, 1, "Bob");

    strncpy(g->secret_word, "Apple", MAX_NAME_LEN - 1);
    g->word_guessed = 0;

    ASSERT(game_validate_guess(g, "banana") == 0, "wrong guess returns 0");
    ASSERT(game_validate_guess(g, "APPLE") == 1, "case-insensitive match");
    ASSERT(game_validate_guess(g, "apple") == 1, "second correct guess also returns 1");

    game_cleanup(g);
}

static void test_scoring(void)
{
    printf("\n--- test_scoring ---\n");
    game_state_t *g = game_init();

    g->num_guessed = 0;
    ASSERT(game_get_guesser_points(g) == 10, "first guesser gets 10");
    g->num_guessed = 1;
    ASSERT(game_get_guesser_points(g) == 9, "second guesser gets 9");
    g->num_guessed = 2;
    ASSERT(game_get_guesser_points(g) == 8, "third guesser gets 8");
    g->num_guessed = 3;
    ASSERT(game_get_guesser_points(g) == 7, "fourth guesser gets 7");
    g->num_guessed = 4;
    ASSERT(game_get_guesser_points(g) == 6, "fifth guesser gets 6");
    g->num_guessed = 5;
    ASSERT(game_get_guesser_points(g) == 5, "sixth guesser gets 5 (min)");
    g->num_guessed = 10;
    ASSERT(game_get_guesser_points(g) == 5, "many guessers still get 5 (min)");

    g->num_guessed = 0;
    ASSERT(game_get_artist_points_for_guess(g) == 5,
           "artist gets 5 for first correct guess");
    g->num_guessed = 1;
    ASSERT(game_get_artist_points_for_guess(g) == 1,
           "artist gets 1 for subsequent correct guesses");

    game_cleanup(g);
}

static void test_mark_guessed_and_all_guessed(void)
{
    printf("\n--- test_mark_guessed_and_all_guessed ---\n");
    game_state_t *g = game_init();
    game_add_player(g, 0, "Alice");
    game_add_player(g, 1, "Bob");
    game_add_player(g, 2, "Carol");

    /* Simulate: player 0 is artist, 1 and 2 are guessers */
    g->players[0].is_artist = 1;
    g->num_guessed = 0;

    ASSERT(game_all_guessed(g) == 0, "not all guessed initially");

    game_mark_guessed(g, 1);
    ASSERT(g->players[1].has_guessed == 1, "Bob marked as guessed");
    ASSERT(g->num_guessed == 1, "num_guessed == 1");
    ASSERT(game_all_guessed(g) == 0, "not all guessed yet (1 of 2)");

    game_mark_guessed(g, 2);
    ASSERT(g->num_guessed == 2, "num_guessed == 2");
    ASSERT(game_all_guessed(g) == 1, "all guessers done (2 of 2)");

    game_cleanup(g);
}

static void test_hint(void)
{
    printf("\n--- test_hint ---\n");
    game_state_t *g = game_init();

    strncpy(g->secret_word, "cat", MAX_NAME_LEN - 1);
    char hint[128];
    game_get_hint(g, hint, sizeof(hint));
    ASSERT(strcmp(hint, "_ _ _") == 0, "hint for 'cat' is '_ _ _'");

    strncpy(g->secret_word, "a", MAX_NAME_LEN - 1);
    game_get_hint(g, hint, sizeof(hint));
    ASSERT(strcmp(hint, "_") == 0, "hint for 'a' is '_'");

    strncpy(g->secret_word, "hi there", MAX_NAME_LEN - 1);
    game_get_hint(g, hint, sizeof(hint));
    /* "hi there" -> "_ _   _ _ _ _ _" (space preserved as double space) */
    printf("    hint='%s'\n", hint);
    ASSERT(strstr(hint, "  ") != NULL, "hint preserves space gap");

    game_cleanup(g);
}

int main(void)
{
    test_init_cleanup();
    test_add_remove_players();
    test_load_words();
    test_start_round();
    test_guess_validation();
    test_scoring();
    test_mark_guessed_and_all_guessed();
    test_hint();

    printf("\n=============================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("=============================\n");
    return tests_failed > 0 ? 1 : 0;
}
