/* Unit tests for src/subtitles.c — build and run with `make test`.
 *
 * Subtitles only render during playback, which can't be exercised off
 * hardware (mplayer writes into a real framebuffer), so this is the only
 * place the clean-up can actually be checked. The inputs below are the shapes
 * Jellyfin really returns: asking for Stream.srt converts the container to
 * SubRip but leaves ASS/SSA inline override codes in the dialogue text. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "subtitles.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do {                          \
    checks++;                                           \
    if (!(cond)) {                                      \
        failures++;                                     \
        printf("  FAIL %s:%d: ", __func__, __LINE__);   \
        printf(__VA_ARGS__);                            \
        printf("\n");                                   \
    }                                                   \
} while (0)

static const char *TMP = "/tmp/misterfin_test_sub.srt";

/* Runs the sanitiser over `input` and returns the result in a static buffer. */
static const char *sanitize(const char *input)
{
    static char out[8192];
    FILE *f = fopen(TMP, "wb");
    if (!f) { out[0] = '\0'; return out; }
    fwrite(input, 1, strlen(input), f);
    fclose(f);

    subtitles_sanitize_srt(TMP);

    f = fopen(TMP, "rb");
    if (!f) { out[0] = '\0'; return out; }
    size_t n = fread(out, 1, sizeof(out) - 1, f);
    fclose(f);
    out[n] = '\0';
    return out;
}

#define CHECK_EQ(got, expect) do {                                    \
    const char *g_ = (got);                                            \
    checks++;                                                          \
    if (strcmp(g_, expect) != 0) {                                     \
        failures++;                                                    \
        printf("  FAIL %s:%d\n    expected: %s\n    got:      %s\n",   \
               __func__, __LINE__, expect, g_);                        \
    }                                                                  \
} while (0)

static void test_ass_override_blocks(void)
{
    /* The reported bug: an ASS source converted to SRT keeps {\...} blocks,
     * and every brace was being drawn on screen as literal text. */
    CHECK_EQ(sanitize(
        "1\n"
        "00:00:20,000 --> 00:00:24,400\n"
        "{\\an8}{\\i1}Hello there{\\i0}\n"
        "\n"),
        "1\n"
        "00:00:20,000 --> 00:00:24,400\n"
        "Hello there\n"
        "\n");

    /* Positioning and colour codes mid-line. */
    CHECK_EQ(sanitize("{\\pos(400,570)}Watch out{\\c&H0000FF&} behind you\n"),
                      "Watch out behind you\n");
}

static void test_line_of_only_codes_is_dropped(void)
{
    /* A line that was nothing but override codes becomes empty — and an
     * empty line is the cue separator in SubRip, so emitting it would cut
     * the cue in half and lose the dialogue. */
    CHECK_EQ(sanitize(
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "{\\an8}\n"
        "Real dialogue\n"
        "\n"),
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "Real dialogue\n"
        "\n");
}

static void test_ass_escapes(void)
{
    CHECK_EQ(sanitize("First\\NSecond\n"), "First\nSecond\n");
    CHECK_EQ(sanitize("Hard\\hspace\n"),   "Hard space\n");
}

static void test_html_markup(void)
{
    CHECK_EQ(sanitize("<i>Italic</i> and <b>bold</b>\n"), "Italic and bold\n");
    CHECK_EQ(sanitize("<font color=\"#ffffff\">Coloured</font>\n"), "Coloured\n");
    CHECK_EQ(sanitize("Line one<br>Line two\n"), "Line oneLine two\n");

    /* Allowlisted on purpose: a bare '<' in dialogue must survive. A
     * strip-to-'>' rule would eat the rest of the line here. */
    CHECK_EQ(sanitize("5 < 6 is true\n"), "5 < 6 is true\n");
    CHECK(subtitles_markup_len("<notatag>") == 0, "unknown tag not consumed");
    CHECK(subtitles_markup_len("<i>") == 3, "<i> length");
    CHECK(subtitles_markup_len("</font color>") == 13, "</font ...> runs to '>'");
}

static void test_timing_lines_survive(void)
{
    /* "-->" contains '>' and the timing line must pass through byte for byte,
     * or every cue loses its timing. */
    const char *out = sanitize(
        "42\n"
        "01:02:03,456 --> 01:02:05,789\n"
        "Text\n"
        "\n");
    CHECK(strstr(out, "01:02:03,456 --> 01:02:05,789") != NULL,
          "timing line intact, got: %s", out);
    CHECK(strstr(out, "42\n") == out, "cue number intact");
}

static void test_non_ascii_folding(void)
{
    /* Previously every byte >= 0x80 was blanked, so an accented word turned
     * into gaps. Folding keeps it readable on an ASCII-only font. */
    CHECK_EQ(sanitize("Caf\xC3\xA9 na\xC3\xAF ve\n"), "Cafe nai ve\n");
    /* U+266A MUSIC NOTE — common in subtitles to mark background music. It
     * has no ASCII equivalent, so it becomes a single '?'. */
    CHECK_EQ(sanitize("\xE2\x99\xAA\n"), "?\n");
}

static void test_structure_preserved(void)
{
    /* A full two-cue file end to end. */
    CHECK_EQ(sanitize(
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "{\\i1}First cue{\\i0}\n"
        "\n"
        "2\n"
        "00:00:03,000 --> 00:00:04,000\n"
        "Second cue\n"
        "\n"),
        "1\n"
        "00:00:01,000 --> 00:00:02,000\n"
        "First cue\n"
        "\n"
        "2\n"
        "00:00:03,000 --> 00:00:04,000\n"
        "Second cue\n"
        "\n");
}

static void test_crlf_input(void)
{
    /* Plenty of subtitle files are CRLF; a stray \r would draw as a glyph. */
    CHECK_EQ(sanitize("1\r\n00:00:01,000 --> 00:00:02,000\r\nText\r\n\r\n"),
                      "1\n00:00:01,000 --> 00:00:02,000\nText\n\n");
}

static void test_degenerate_input(void)
{
    /* Unterminated brace must not swallow the file — it's bounded to the
     * line, so the text after the newline still comes through. */
    const char *out = sanitize("{\\an8 unterminated\nSurvivor\n");
    CHECK(strstr(out, "Survivor") != NULL, "text after a stray '{' survives, got: %s", out);

    CHECK_EQ(sanitize(""), "");

    /* A missing file is a no-op, not a crash. */
    remove(TMP);
    subtitles_sanitize_srt(TMP);
    checks++;
}

int main(void)
{
    test_ass_override_blocks();
    test_line_of_only_codes_is_dropped();
    test_ass_escapes();
    test_html_markup();
    test_timing_lines_survive();
    test_non_ascii_folding();
    test_structure_preserved();
    test_crlf_input();
    test_degenerate_input();

    remove(TMP);
    printf("subtitles: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
