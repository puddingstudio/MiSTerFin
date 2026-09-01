#ifndef PAUSE_UI_H
#define PAUSE_UI_H

#define PAUSE_UI_TIMEOUT_SECONDS 3.0

/* Video pause-overlay visibility and timeout. */
typedef struct {
    int visible;
    double hide_at;
} PauseUi;

/* Hides the controls and cancels any active deadline. */
void pause_ui_hide(PauseUi *ui);
/* Returns nonzero when hidden controls became visible, so the waking input
 * can be consumed instead of also performing its normal action. */
int  pause_ui_reveal(PauseUi *ui, double now);
/* Returns nonzero only on the visible-to-hidden deadline transition. */
int  pause_ui_expire(PauseUi *ui, double now);

#endif
