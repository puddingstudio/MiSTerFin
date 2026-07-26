#include "subtitles.h"
#include "jellyfin.h"   /* jf_text_to_display */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Returns the length of a recognised inline markup tag at p, or 0.
 *
 * Deliberately an allowlist rather than "skip from '<' to '>'": subtitle
 * dialogue really does contain bare angle brackets, and a blanket rule would
 * silently eat the rest of a line like "5 < 6 is true". */
int subtitles_markup_len(const char *p)
{
    static const char *const tags[] = {
        "<i>", "</i>", "<b>", "</b>", "<u>", "</u>",
        "<br>", "<br/>", "<br />", NULL
    };
    for (int i = 0; tags[i]; i++) {
        size_t n = strlen(tags[i]);
        if (strncasecmp(p, tags[i], n) == 0) return (int)n;
    }
    /* <font color="#ffffff"> / </font> — attributes vary, so run to the '>'. */
    if (!strncasecmp(p, "<font", 5) || !strncasecmp(p, "</font", 6)) {
        const char *end = strchr(p, '>');
        if (end) return (int)(end - p + 1);
    }
    return 0;
}

/* Cleans up a downloaded .srt so mplayer's classic bitmap renderer can show
 * it as plain readable text.
 *
 * Two separate problems get handled here.
 *
 * Formatting codes: asking Jellyfin for Stream.srt converts the CONTAINER to
 * SubRip, but not the dialogue text itself — an ASS/SSA source keeps its
 * inline override blocks, so lines arrive looking like
 * "{\an8}{\i1}Hello there{\i0}" and every one of those braces gets drawn
 * on screen. \N (ASS hard line break) and \h (hard space) survive too, as do
 * the <i>/<b>/<font> tags SubRip itself permits. None of that means anything
 * to this renderer, so it is stripped rather than displayed. Confirmed on
 * real hardware against a real server as the reason ASS subtitles looked
 * like code.
 *
 * Non-ASCII: the font (src/font8x8.h) covers 0x00-0x7F only. Text is folded
 * through the same transliteration used for titles, so "café" reads as
 * "cafe" rather than as replacement blobs — a real improvement on the old
 * behaviour here, which blanked every byte >= 0x80 and turned any accented
 * word into gaps. */
void subtitles_sanitize_srt(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0 || len > 4 * 1024 * 1024) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char *raw = malloc((size_t)len + 1);
    if (!raw) { fclose(f); return; }
    size_t got = fread(raw, 1, (size_t)len, f);
    fclose(f);
    raw[got] = '\0';

    /* Pass 1 — drop markup and override blocks, turn ASS escapes into real
     * characters. Output is never longer than the input, so one buffer of the
     * same size is always enough. */
    char *stripped = malloc(got + 1);
    if (!stripped) { free(raw); return; }

    size_t o = 0;
    /* Whether a line ends up empty has to be decided against the INPUT line,
     * not the output: a line that was nothing but "{\an8}" strips to nothing,
     * and emitting its newline anyway leaves a blank line — which is the cue
     * separator in SubRip, so the cue would be cut in half and its dialogue
     * lost. Tracking the line start lets such a line be rolled back entirely,
     * while a line that was blank to begin with is a real separator and is
     * kept. */
    size_t line_start  = 0;   /* output offset where the current line began */
    int    line_stripped = 0; /* something was removed from this line */
    int    line_visible  = 0; /* something other than whitespace survived */

    for (size_t i = 0; i < got; ) {
        char c = raw[i];

        if (c == '\n') {
            /* Only lines that LOST content are candidates for removal. A line
             * that was already blank — including the lone '\r' of a CRLF
             * separator — is a real cue boundary and must survive. */
            if (line_stripped && !line_visible) {
                o = line_start;          /* drop the line, newline included */
            } else {
                stripped[o++] = '\n';
                line_start = o;
            }
            line_stripped = line_visible = 0;
            i++;
            continue;
        }

        if (c == '{') {
            /* Bounded to the current line: an unmatched '{' shouldn't be
             * able to swallow the rest of the file. */
            size_t j = i + 1;
            while (j < got && raw[j] != '}' && raw[j] != '\n') j++;
            if (j < got && raw[j] == '}') { line_stripped = 1; i = j + 1; continue; }
        }
        if (c == '<') {
            int n = subtitles_markup_len(raw + i);
            if (n) { line_stripped = 1; i += (size_t)n; continue; }
        }
        if (c == '\\' && i + 1 < got) {
            char esc = raw[i + 1];
            if (esc == 'N' || esc == 'n') {
                /* An ASS hard break starts a new line, so the bookkeeping
                 * has to roll over with it. */
                stripped[o++] = '\n';
                line_start = o;
                line_stripped = line_visible = 0;
                i += 2;
                continue;
            }
            if (esc == 'h') { stripped[o++] = ' '; i += 2; continue; }
        }

        if (c != ' ' && c != '\t' && c != '\r') line_visible = 1;
        stripped[o++] = c;
        i++;
    }
    /* Same decision for a trailing line with no terminating newline. */
    if (line_stripped && !line_visible) o = line_start;

    stripped[o] = '\0';
    free(raw);

    /* Pass 2 — fold each line to ASCII independently, so the line structure
     * SubRip depends on survives (jf_text_to_display flattens newlines to
     * spaces, which would otherwise merge a cue's timing and text lines). */
    FILE *out = fopen(path, "wb");
    if (!out) { free(stripped); return; }

    char *line = stripped;
    while (line < stripped + o) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\r') line[--line_len] = '\0';

        /* Folding can't empty a line that wasn't already empty — an
         * untranslatable character becomes '?', not nothing — so pass 1's
         * decision about which lines survive still stands. */
        char folded[1024];
        jf_text_to_display(line, folded, sizeof(folded));
        fprintf(out, "%s\n", folded);

        if (!nl) break;
        line = nl + 1;
    }

    fclose(out);
    free(stripped);
}
