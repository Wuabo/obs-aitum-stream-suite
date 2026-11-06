#pragma once

class QWidget;

/* Profile countdowns: absolute end time, pause, and a text source. */
void countdowns_register_source();
QWidget *countdowns_create_page(QWidget *parent);
void countdowns_reload();
void countdowns_shutdown();
void countdowns_set_visible(bool visible);
