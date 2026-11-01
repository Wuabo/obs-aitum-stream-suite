#include "profile-secret.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include <QByteArray>
#include <cstring>

static const char kPrefix[] = "dpapi:";

QString RevealProfileSecret(const char *stored)
{
	if (!stored || !stored[0])
		return {};
	const QByteArray raw(stored);
	if (!raw.startsWith(kPrefix))
		return QString::fromUtf8(raw);
#ifdef _WIN32
	const QByteArray blob = QByteArray::fromBase64(raw.mid(int(strlen(kPrefix))));
	if (blob.isEmpty())
		return {};
	DATA_BLOB in{};
	in.cbData = DWORD(blob.size());
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(blob.constData()));
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) || !out.pbData)
		return {};
	const QString plain = QString::fromUtf8(reinterpret_cast<const char *>(out.pbData), int(out.cbData));
	LocalFree(out.pbData);
	return plain;
#else
	return QString::fromUtf8(raw);
#endif
}

void StoreProfileSecret(obs_data_t *object, const char *key, const QString &plain)
{
	if (!object || !key)
		return;
	if (plain.isEmpty()) {
		obs_data_set_string(object, key, "");
		return;
	}
#ifdef _WIN32
	const QByteArray utf8 = plain.toUtf8();
	DATA_BLOB in{};
	in.cbData = DWORD(utf8.size());
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(utf8.constData()));
	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"Aitum Stream Suite", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out) ||
	    !out.pbData) {
		obs_data_set_string(object, key, plain.toUtf8().constData());
		return;
	}
	QByteArray sealed(kPrefix);
	sealed += QByteArray(reinterpret_cast<const char *>(out.pbData), int(out.cbData)).toBase64();
	LocalFree(out.pbData);
	obs_data_set_string(object, key, sealed.constData());
#else
	obs_data_set_string(object, key, plain.toUtf8().constData());
#endif
}

bool UpgradeProfileSecret(obs_data_t *object, const char *key)
{
	if (!object || !key)
		return false;
	const char *stored = obs_data_get_string(object, key);
	if (!stored || !stored[0] || strncmp(stored, kPrefix, strlen(kPrefix)) == 0)
		return false;
	StoreProfileSecret(object, key, QString::fromUtf8(stored));
	const char *now = obs_data_get_string(object, key);
	return now && strncmp(now, kPrefix, strlen(kPrefix)) == 0;
}
