#include "pause_ui.h"

void pause_ui_hide(PauseUi *ui)
{
    ui->visible = 0;
    ui->hide_at = 0.0;
}

int pause_ui_reveal(PauseUi *ui, double now)
{
    int was_hidden = !ui->visible;
    ui->visible = 1;
    ui->hide_at = now + PAUSE_UI_TIMEOUT_SECONDS;
    return was_hidden;
}

int pause_ui_expire(PauseUi *ui, double now)
{
    if (!ui->visible || now < ui->hide_at) return 0;
    ui->visible = 0;
    ui->hide_at = 0.0;
    return 1;
}
