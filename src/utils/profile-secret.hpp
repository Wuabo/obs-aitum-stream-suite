#pragma once

#include <obs-data.h>
#include <QString>

/* Read a value stored by StoreProfileSecret. Older profiles keep the raw string. */
QString RevealProfileSecret(const char *stored);

/* Replace the field with a value only the current Windows user can read. */
void StoreProfileSecret(obs_data_t *object, const char *key, const QString &plain);

/* Seal a legacy plaintext field. Returns true when the stored value changed. */
bool UpgradeProfileSecret(obs_data_t *object, const char *key);
