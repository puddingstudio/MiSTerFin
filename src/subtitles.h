#pragma once

/* Subtitle text clean-up, split out from main.c so it can be tested directly:
 * it's pure text processing with no framebuffer or player involvement, and
 * the failure mode (override codes drawn on screen as literal text) is only
 * visible during playback, which is exactly the thing that can't be exercised
 * off-hardware. */

/* Rewrites a downloaded .srt in place so mplayer's classic bitmap renderer
 * can display it as plain text:
 *
 *  - ASS/SSA inline override blocks ({\an8}, {\pos(..)}, {\i1}, ...) removed
 *  - \N and \n turned into real line breaks, \h into a space
 *  - <i>/<b>/<u>/<br>/<font ...> markup removed
 *  - text folded to ASCII (see jf_text_to_display)
 *  - lines left empty by the above dropped, so a cue isn't split in two by
 *    an accidental blank line
 *
 * Cue numbering and "00:00:20,000 --> 00:00:24,400" timing lines pass through
 * untouched. Silently does nothing if the file is missing, empty, or larger
 * than a sane subtitle track. */
void subtitles_sanitize_srt(const char *path);

/* Length of a recognised inline markup tag at `p`, or 0 if there isn't one.
 * Exposed for testing — the allowlist behaviour matters (a blanket
 * '<' to '>' rule would eat dialogue containing a bare angle bracket). */
int subtitles_markup_len(const char *p);
