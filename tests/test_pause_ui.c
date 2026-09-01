/* Video pause-overlay state.
 *
 * Every pause stays clean until directional input asks for the controls.
 * These transitions are easy to regress because playback, seeking, and the
 * track submenu all share the same underlying mplayer pause command. */

#include <stdio.h>
#include <stdlib.h>

#include "pause_ui.h"

static int checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { \
    printf("  FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); exit(1); } } while (0)

int main(void)
{
    PauseUi ui = {.visible = 1, .hide_at = 99.0};
    pause_ui_hide(&ui);
    CHECK(!ui.visible, "a new playback starts with the overlay hidden");
    CHECK(ui.hide_at == 0.0, "a new playback clears any old deadline");

    pause_ui_hide(&ui);
    CHECK(!ui.visible, "the first pause stays clean");
    CHECK(ui.hide_at == 0.0, "pausing alone does not start a deadline");

    CHECK(pause_ui_reveal(&ui, 20.0), "the first direction wakes hidden controls");
    CHECK(ui.visible, "directional input reveals the overlay");
    CHECK(ui.hide_at == 23.0, "a reveal starts a new three-second deadline");
    CHECK(!pause_ui_reveal(&ui, 22.0), "direction on visible controls is not another wake");
    CHECK(ui.hide_at == 25.0, "more directional input extends the deadline");
    CHECK(!pause_ui_expire(&ui, 24.0), "the extended deadline is honored");
    CHECK(pause_ui_expire(&ui, 25.0), "the overlay expires at its deadline");
    CHECK(!ui.visible, "expiration restores the clean pause screen");
    CHECK(!pause_ui_expire(&ui, 26.0), "an already hidden overlay does not expire twice");

    CHECK(pause_ui_reveal(&ui, 30.0), "controls can be woken again");
    pause_ui_hide(&ui);
    CHECK(!ui.visible, "resuming hides the overlay");
    CHECK(ui.hide_at == 0.0, "resuming cancels the deadline");
    pause_ui_hide(&ui);
    CHECK(!ui.visible, "a later pause stays clean");
    CHECK(pause_ui_reveal(&ui, 31.0), "direction wakes controls after a later pause");
    CHECK(ui.visible, "directional input still reveals controls on a later pause");

    printf("pause ui: %d checks, 0 failures\n", checks);
    return 0;
}
