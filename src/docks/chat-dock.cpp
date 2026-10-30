#include "chat-dock.hpp"
#include "stream-info-dock.hpp"
#include <src/utils/icon.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QBuffer>
#include <QColor>
#include <QCursor>
#include <QDateTime>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QFont>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QHideEvent>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSvgRenderer>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>
#include <memory>
#include <util/bmem.h>

namespace {

/* Simple left-to-right wrapping row for username + text + animated emote labels. */
class ChatFlowBody final : public QWidget {
public:
	explicit ChatFlowBody(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("chatMsgBody"));
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
	}

	QLabel *addHtml(const QString &html)
	{
		auto *lab = new QLabel(this);
		lab->setTextFormat(Qt::RichText);
		lab->setTextInteractionFlags(Qt::TextBrowserInteraction);
		lab->setOpenExternalLinks(true);
		lab->setWordWrap(false);
		lab->setStyleSheet(QStringLiteral("background: transparent; color: #e6e6e6; border: none; padding: 0; margin: 0;"));
		lab->setText(html);
		lab->adjustSize();
		segs.append({lab, lab->sizeHint().width(), lab->sizeHint().height()});
		return lab;
	}

	QLabel *addPlain(const QString &text)
	{
		auto *lab = new QLabel(this);
		lab->setTextFormat(Qt::PlainText);
		lab->setWordWrap(false);
		QFont font(QStringLiteral("Segoe UI"));
		font.setPixelSize(13);
		lab->setFont(font);
		lab->setStyleSheet(QStringLiteral(
			"background: transparent; color: #e6e6e6; border: none; padding: 0; margin: 0;"));
		lab->setText(text);
		const int w = qMax(lab->fontMetrics().horizontalAdvance(text) + 1, 4);
		const int h = qMax(lab->fontMetrics().height(), 16);
		lab->setFixedSize(w, h);
		segs.append({lab, w, h});
		return lab;
	}

	QLabel *addEmoteSlot(int size = 18)
	{
		auto *lab = new QLabel(this);
		lab->setFixedSize(size, size);
		lab->setScaledContents(true);
		lab->setAlignment(Qt::AlignCenter);
		lab->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
		segs.append({lab, size, size});
		return lab;
	}

	bool hasHeightForWidth() const override { return true; }

	int heightForWidth(int w) const override
	{
		return layoutHeight(qMax(40, w));
	}

	QSize sizeHint() const override
	{
		const int w = width() > 0 ? width() : 200;
		return QSize(w, layoutHeight(w));
	}

	QSize minimumSizeHint() const override { return QSize(40, 16); }

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		applyLayout(width());
	}

private:
	struct Seg {
		QWidget *w = nullptr;
		int wHint = 0;
		int hHint = 0;
	};
	QVector<Seg> segs;

	int layoutHeight(int w) const
	{
		int x = 0;
		int y = 0;
		int rowH = 0;
		for (const Seg &s : segs) {
			const int sw = s.wHint;
			const int sh = s.hHint;
			if (x > 0 && x + sw > w) {
				x = 0;
				y += rowH + 2;
				rowH = 0;
			}
			x += sw + 2;
			rowH = qMax(rowH, sh);
		}
		return qMax(16, y + rowH);
	}

	void applyLayout(int w)
	{
		int x = 0;
		int y = 0;
		int rowH = 0;
		for (const Seg &s : segs) {
			if (!s.w)
				continue;
			const int sw = s.wHint;
			const int sh = s.hHint;
			if (x > 0 && x + sw > w) {
				x = 0;
				y += rowH + 2;
				rowH = 0;
			}
			s.w->setGeometry(x, y, sw, sh);
			x += sw + 2;
			rowH = qMax(rowH, sh);
		}
	}
};

/* Painted control. A QToolButton picked up the OBS theme and drew an empty circle. */
class ChatActionsButton final : public QWidget {
public:
	std::function<void()> onClick;

	explicit ChatActionsButton(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName(QStringLiteral("chatMsgActions"));
		setFixedSize(18, 18);
		setCursor(Qt::PointingHandCursor);
		setAttribute(Qt::WA_Hover, true);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);
		if (underMouse()) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(46, 56, 74));
			painter.drawRoundedRect(rect(), 4, 4);
		}
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(230, 235, 242));
		const qreal cx = width() / 2.0;
		painter.drawEllipse(QPointF(cx, 3.5), 1.25, 1.25);
		painter.drawEllipse(QPointF(cx, 9.0), 1.25, 1.25);
		painter.drawEllipse(QPointF(cx, 14.5), 1.25, 1.25);
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && rect().contains(event->pos()) && onClick)
			onClick();
	}

	void enterEvent(QEnterEvent *event) override
	{
		QWidget::enterEvent(event);
		update();
	}

	void leaveEvent(QEvent *event) override
	{
		QWidget::leaveEvent(event);
		update();
	}
};

struct EmojiRun {
	QString text;
	bool emoji = false;
	QString url;
};

bool EmojiStartCp(char32_t cp)
{
	return (cp >= 0x231A && cp <= 0x231B) || (cp >= 0x23E9 && cp <= 0x23FA) || (cp >= 0x25AA && cp <= 0x27BF) ||
	       (cp >= 0x2B05 && cp <= 0x2B55) || (cp >= 0x1F1E6 && cp <= 0x1F1FF) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
	       cp == 0x2764 || cp == 0x2763 || cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299 ||
	       cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 || cp == 0x2122 || cp == 0x2139 ||
	       (cp >= 0x2194 && cp <= 0x21AA) || (cp >= 0x2934 && cp <= 0x2935) || (cp >= 0x2B05 && cp <= 0x2B07);
}

bool EmojiContinueCp(char32_t cp)
{
	return cp == 0xFE0F || cp == 0x200D || cp == 0x20E3 || (cp >= 0x1F3FB && cp <= 0x1F3FF) || EmojiStartCp(cp);
}

char32_t ReadCp(const QString &text, int i, int *step)
{
	if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
		*step = 2;
		return QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
	}
	*step = 1;
	return text.at(i).unicode();
}

QVector<EmojiRun> SplitEmojiRuns(const QString &text)
{
	QVector<EmojiRun> runs;
	QString plain;
	auto flush = [&] {
		if (plain.isEmpty())
			return;
		runs.append({plain, false, {}});
		plain.clear();
	};
	int i = 0;
	while (i < text.size()) {
		int step = 1;
		const char32_t cp = ReadCp(text, i, &step);
		if (!EmojiStartCp(cp)) {
			plain += text.mid(i, step);
			i += step;
			continue;
		}
		flush();
		QVector<uint> cluster;
		cluster.append(cp);
		int j = i + step;
		while (j < text.size()) {
			int nstep = 1;
			const char32_t ncp = ReadCp(text, j, &nstep);
			const bool zwj = !cluster.isEmpty() && cluster.last() == 0x200D;
			if (!EmojiContinueCp(ncp) && !zwj)
				break;
			if (zwj && !EmojiStartCp(ncp) && ncp != 0xFE0F)
				break;
			cluster.append(ncp);
			j += nstep;
		}
		QStringList parts;
		for (uint c : cluster) {
			if (c == 0xFE0F)
				continue;
			parts.append(QString::number(c, 16));
		}
		EmojiRun run;
		run.emoji = true;
		run.text = text.mid(i, j - i);
		run.url = QStringLiteral("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/%1.png")
				  .arg(parts.join(QLatin1Char('-')));
		runs.append(run);
		i = j;
	}
	flush();
	return runs;
}

QPixmap RenderEmojiPixmap(const QString &grapheme, int size);

} // namespace
#include <util/base.h>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <cmath>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

namespace {

/* Qt's raster painter drops color emoji, so the slot was an empty box. DirectWrite
 * with ENABLE_COLOR_FONT paints Segoe UI Emoji into a real image. */
QPixmap RenderEmojiPixmap(const QString &grapheme, int size)
{
	if (grapheme.isEmpty() || size <= 0)
		return {};
	static QHash<QString, QPixmap> cache;
	const QString key = QString::number(size) + QLatin1Char('|') + grapheme;
	if (cache.contains(key))
		return cache.value(key);
#ifndef _WIN32
	Q_UNUSED(cache);
	return {};
#else
	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	const bool balanceCom = (com == S_OK || com == S_FALSE);
	QPixmap result;
	ID2D1Factory *factory = nullptr;
	IDWriteFactory *write = nullptr;
	IWICImagingFactory *wic = nullptr;
	IWICBitmap *bmp = nullptr;
	ID2D1RenderTarget *rt = nullptr;
	IDWriteTextFormat *format = nullptr;
	ID2D1SolidColorBrush *brush = nullptr;
	IWICBitmapLock *lock = nullptr;
	const UINT px = UINT(qMax(size * 2, 32));
	if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory)) &&
	    SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
					  reinterpret_cast<IUnknown **>(&write))) &&
	    SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
	    SUCCEEDED(wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp)) &&
	    SUCCEEDED(factory->CreateWicBitmapRenderTarget(
		    bmp,
		    D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
						 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
		    &rt)) &&
	    SUCCEEDED(write->CreateTextFormat(L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_REGULAR,
					      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(px) * 0.72f,
					      L"en-us", &format))) {
		format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->BeginDraw();
		rt->Clear(D2D1::ColorF(0, 0, 0, 0));
		if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush))) {
			const D2D1_RECT_F layout = D2D1::RectF(0, 0, float(px), float(px));
			rt->DrawText(reinterpret_cast<const wchar_t *>(grapheme.utf16()), UINT32(grapheme.size()), format,
				     layout, brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
		}
		rt->EndDraw();
		WICRect rect{0, 0, INT(px), INT(px)};
		if (SUCCEEDED(bmp->Lock(&rect, WICBitmapLockRead, &lock)) && lock) {
			UINT stride = 0;
			BYTE *data = nullptr;
			UINT bytes = 0;
			lock->GetStride(&stride);
			lock->GetDataPointer(&bytes, &data);
			if (data && stride >= px * 4) {
				QImage img(int(px), int(px), QImage::Format_ARGB32_Premultiplied);
				bool any = false;
				for (UINT y = 0; y < px; ++y) {
					memcpy(img.scanLine(int(y)), data + y * stride, px * 4);
					const auto *row = reinterpret_cast<const QRgb *>(img.scanLine(int(y)));
					for (UINT x = 0; x < px && !any; ++x)
						any = qAlpha(row[x]) > 16;
				}
				if (any)
					result = QPixmap::fromImage(img).scaled(size, size, Qt::KeepAspectRatio,
										 Qt::SmoothTransformation);
			}
		}
	}
	if (lock)
		lock->Release();
	if (brush)
		brush->Release();
	if (format)
		format->Release();
	if (rt)
		rt->Release();
	if (bmp)
		bmp->Release();
	if (wic)
		wic->Release();
	if (write)
		write->Release();
	if (factory)
		factory->Release();
	if (balanceCom)
		CoUninitialize();
	if (!result.isNull()) {
		if (cache.size() > 400)
			cache.clear();
		cache.insert(key, result);
	}
	return result;
#endif
}

} // namespace

extern obs_data_t *current_profile_config;

namespace {

struct HttpResult {
	int status = 0;
	QByteArray body;
	QString error;
};

size_t CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *out = static_cast<QByteArray *>(userdata);
	out->append(ptr, static_cast<qsizetype>(size * nmemb));
	return size * nmemb;
}

HttpResult CurlPerform(CURL *curl)
{
	HttpResult result;
	char error[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aitum-stream-suite");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef _WIN32
#ifdef CURLSSLOPT_REVOKE_BEST_EFFORT
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
#elif defined(CURLSSLOPT_NO_REVOKE)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);
#endif
#endif
	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	result.status = static_cast<int>(status);
	if (code != CURLE_OK)
		result.error = QString::fromUtf8(error[0] ? error : curl_easy_strerror(code));
	return result;
}

void RunAsync(QObject *context, std::function<HttpResult()> work,
	      std::function<void(int, const QByteArray &, const QString &)> done)
{
	QPointer<QObject> alive(context);
	std::thread([alive, work = std::move(work), done = std::move(done)] {
		const HttpResult result = work();
		if (!alive)
			return;
		QMetaObject::invokeMethod(
			alive.data(),
			[alive, done, result] {
				if (alive)
					done(result.status, result.body, result.error);
			},
			Qt::QueuedConnection);
	}).detach();
}

QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

bool YouTubeBodyLooksLikeQuota(const QByteArray &body)
{
	const QByteArray lower = body.toLower();
	return lower.contains("quota") || lower.contains("\"reason\": \"quotaexceeded\"") ||
	       lower.contains("\"reason\":\"quotaexceeded\"");
}

/* liveBroadcasts.snippet uses liveChatId; videos.liveStreamingDetails uses activeLiveChatId. */
QString YouTubeBroadcastChatId(const QJsonObject &snippet)
{
	QString id = snippet.value(QStringLiteral("liveChatId")).toString();
	if (id.isEmpty())
		id = snippet.value(QStringLiteral("activeLiveChatId")).toString();
	return id;
}

QString YouTubeVideoActiveChatId(const QJsonObject &liveStreamingDetails)
{
	return liveStreamingDetails.value(QStringLiteral("activeLiveChatId")).toString();
}

bool YouTubeBroadcastLifeDead(const QString &life)
{
	return life == QLatin1String("complete") || life == QLatin1String("revoked") ||
	       life == QLatin1String("abandoned");
}

qint64 YouTubePublishedMs(const QJsonObject &snippet)
{
	const QString published = snippet.value(QStringLiteral("publishedAt")).toString();
	if (published.isEmpty())
		return 0;
	QDateTime dt = QDateTime::fromString(published, Qt::ISODateWithMs);
	if (!dt.isValid())
		dt = QDateTime::fromString(published, Qt::ISODate);
	if (!dt.isValid())
		return 0;
	return dt.toMSecsSinceEpoch();
}

QString EscapeHtml(const QString &text)
{
	QString out = text;
	out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
	out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
	out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
	out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
	return out;
}

QString Linkify(const QString &escaped)
{
	QString out;
	out.reserve(escaped.size() + 32);
	int i = 0;
	while (i < escaped.size()) {
		const int http = escaped.indexOf(QStringLiteral("http"), i);
		if (http < 0) {
			out += escaped.mid(i);
			break;
		}
		out += escaped.mid(i, http - i);
		int end = http;
		while (end < escaped.size() && !escaped.at(end).isSpace() && escaped.at(end) != QLatin1Char('<'))
			++end;
		const QString url = escaped.mid(http, end - http);
		out += QStringLiteral("<a href=\"%1\" style=\"color:#6cb6ff;\">%1</a>").arg(url);
		i = end;
	}
	return out;
}

QJsonObject FirstDataObject(const QByteArray &body)
{
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (doc.isArray() && !doc.array().isEmpty())
		return doc.array().at(0).toObject();
	const QJsonObject root = doc.object();
	const QJsonValue data = root.value("data");
	if (data.isArray()) {
		if (data.toArray().isEmpty())
			return {};
		return data.toArray().at(0).toObject();
	}
	if (data.isObject())
		return data.toObject();
	return {};
}

QString JsonId(const QJsonValue &value)
{
	if (value.isDouble())
		return QString::number(value.toInteger());
	return value.toString();
}

void ApplyCurlSsl(CURL *curl)
{
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aitum-stream-suite");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef _WIN32
#ifdef CURLSSLOPT_REVOKE_BEST_EFFORT
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
#elif defined(CURLSSLOPT_NO_REVOKE)
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);
#endif
#endif
}

bool CurlWsSendText(CURL *curl, const QByteArray &payload)
{
	size_t sent = 0;
	size_t left = static_cast<size_t>(payload.size());
	const char *ptr = payload.constData();
	while (left > 0) {
		sent = 0;
		const CURLcode rc = curl_ws_send(curl, ptr, left, &sent, 0, CURLWS_TEXT);
		if (rc == CURLE_AGAIN)
			continue;
		if (rc != CURLE_OK)
			return false;
		ptr += sent;
		left -= sent;
	}
	return true;
}

bool CurlWsWaitReadable(CURL *curl, int timeoutMs)
{
	curl_socket_t sock = CURL_SOCKET_BAD;
	if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD)
		return false;
#ifdef _WIN32
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	return select(0, &fds, nullptr, nullptr, &tv) >= 0;
#else
	pollfd pfd{};
	pfd.fd = sock;
	pfd.events = POLLIN;
	return poll(&pfd, 1, timeoutMs) >= 0;
#endif
}

} // namespace

const char *ChatDock::PlatformKey(Platform platform)
{
	switch (platform) {
	case YouTube:
		return "youtube";
	case Kick:
		return "kick";
	case Twitch:
	default:
		return "twitch";
	}
}

QString ChatDock::PlatformLabel(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QStringLiteral("YouTube");
	case Kick:
		return QStringLiteral("Kick");
	case Twitch:
	default:
		return QStringLiteral("Twitch");
	}
}

QIcon ChatDock::PlatformIcon(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QIcon(QStringLiteral(":/aitum/media/youtube.png"));
	case Kick:
		return QIcon(QStringLiteral(":/aitum/media/kick.png"));
	case Twitch:
	default:
		return QIcon(QStringLiteral(":/aitum/media/twitch.png"));
	}
}

QColor ChatDock::PlatformAccent(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QColor(QStringLiteral("#FF5555"));
	case Kick:
		return QColor(QStringLiteral("#53FC18"));
	case Twitch:
	default:
		return QColor(QStringLiteral("#9146FF"));
	}
}

QColor ChatDock::PlatformChipAccent(Platform platform)
{
	switch (platform) {
	case YouTube:
		return QColor(QStringLiteral("#CC0000"));
	case Kick:
		return QColor(QStringLiteral("#1B7A2E"));
	case Twitch:
	default:
		return QColor(QStringLiteral("#9146FF"));
	}
}

ChatDock::ChatDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteChat"));
	curl_global_init(CURL_GLOBAL_DEFAULT);

	viewerTimer = new QTimer(this);
	viewerTimer->setInterval(5000);
	connect(viewerTimer, &QTimer::timeout, this, &ChatDock::RefreshViewers);

	youtubeTimer = new QTimer(this);
	youtubeTimer->setSingleShot(true);
	connect(youtubeTimer, &QTimer::timeout, this, &ChatDock::PollYouTubeChat);

	youtubeDiscoverTimer = new QTimer(this);
	/* Slow on purpose — each live chat poll costs quota. */
	youtubeDiscoverTimer->setInterval(30000);
	connect(youtubeDiscoverTimer, &QTimer::timeout, this, [this] {
		if (!connectionsActive || !availablePlatforms.contains(YouTube))
			return;
		if (ytLiveChatId.isEmpty()) {
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			const bool streaming = obs_frontend_streaming_active();
			EnsureYouTubeWaitLiveTimer();
			/* While streaming (ready→live lag), rediscover often; otherwise every 2 min. */
			const qint64 fullEvery = streaming ? 12000 : 120000;
			if (!ytVideoId.isEmpty()) {
				ResolveYouTubeLiveChatId(ytVideoId, [this, now, fullEvery](bool ok) {
					if (!ok && now - youtubeLastFullDiscoverMs >= fullEvery)
						StartYouTubeChat();
				});
				return;
			}
			if (now - youtubeLastFullDiscoverMs >= fullEvery)
				StartYouTubeChat();
			return;
		}
		if (youtubeWaitLiveTimer)
			youtubeWaitLiveTimer->stop();
		/* Keep polling alive if the single-shot timer was lost; never restart while in flight. */
		if (!youtubePollInFlight && !youtubeTimer->isActive())
			PollYouTubeChat();
	});

	youtubeWaitLiveTimer = new QTimer(this);
	youtubeWaitLiveTimer->setInterval(3000);
	connect(youtubeWaitLiveTimer, &QTimer::timeout, this, &ChatDock::TickYouTubeWaitLive);

	kickReconnectTimer = new QTimer(this);
	kickReconnectTimer->setSingleShot(true);
	connect(kickReconnectTimer, &QTimer::timeout, this, [this] {
		if (connectionsActive)
			ResolveKickChatroom();
	});

	twitchReconnectTimer = new QTimer(this);
	twitchReconnectTimer->setSingleShot(true);
	connect(twitchReconnectTimer, &QTimer::timeout, this, [this] {
		if (connectionsActive)
			ConnectTwitchIrc();
	});

	BuildUi();
	RefreshAvailablePlatforms();
}

ChatDock::~ChatDock()
{
	StopConnections();
}

void ChatDock::BuildUi()
{
	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	viewerBar = new QFrame(this);
	viewerBar->setObjectName(QStringLiteral("chatViewerBar"));
	auto *barLayout = new QHBoxLayout(viewerBar);
	barLayout->setContentsMargins(8, 6, 8, 6);
	barLayout->setSpacing(6);

	for (int i = 0; i < 3; ++i) {
		viewerChips[i] = new QFrame(viewerBar);
		viewerChips[i]->setObjectName(QStringLiteral("chatViewerChip"));
		auto *chipLayout = new QHBoxLayout(viewerChips[i]);
		chipLayout->setContentsMargins(8, 4, 10, 4);
		chipLayout->setSpacing(6);
		auto *icon = new QLabel(viewerChips[i]);
		icon->setFixedSize(16, 16);
		icon->setScaledContents(true);
		icon->setPixmap(PlatformIcon(Platform(i)).pixmap(16, 16));
		viewerLabels[i] = new QLabel(QStringLiteral("—"), viewerChips[i]);
		viewerLabels[i]->setStyleSheet(QStringLiteral("color: white; font-weight: 600;"));
		chipLayout->addWidget(icon);
		chipLayout->addWidget(viewerLabels[i]);
		const QColor accent = PlatformChipAccent(Platform(i));
		viewerChips[i]->setStyleSheet(
			QStringLiteral("QFrame#chatViewerChip { background: %1; border-radius: 4px; }")
				.arg(accent.name()));
		viewerChips[i]->setVisible(false);
		barLayout->addWidget(viewerChips[i]);
	}
	barLayout->addStretch(1);
	clearSystemButton = new QPushButton(Tr("ChatDockClearSystem"), viewerBar);
	clearSystemButton->setCursor(Qt::PointingHandCursor);
	clearSystemButton->setFlat(true);
	clearSystemButton->setStyleSheet(QStringLiteral(
		"QPushButton { color: #b0b0b0; background: transparent; border: 1px solid #3a3a3a;"
		" border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
		"QPushButton:hover { color: #eee; border-color: #666; background: #222; }"
		"QPushButton:pressed { background: #1a1a1a; }"));
	connect(clearSystemButton, &QPushButton::clicked, this, &ChatDock::ClearSystemMessages);
	barLayout->addWidget(clearSystemButton);
	root->addWidget(viewerBar);

	messageList = new QListWidget(this);
	messageList->setObjectName(QStringLiteral("chatMessageList"));
	messageList->setSelectionMode(QAbstractItemView::NoSelection);
	messageList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	messageList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	messageList->setUniformItemSizes(false);
	messageList->setSpacing(6);
	messageList->setStyleSheet(QStringLiteral(
		"QListWidget#chatMessageList { background: #141414; border: none; color: #eee; outline: none; }"
		"QListWidget#chatMessageList::item { padding: 0px; margin: 0px; border: none;"
		" background: transparent; }"
		"QListWidget#chatMessageList::item:selected { background: transparent; }"
		"QListWidget#chatMessageList::item:hover { background: transparent; }"));
	messageList->viewport()->installEventFilter(this);
	root->addWidget(messageList, 1);

	auto *inputRow = new QFrame(this);
	auto *inputLayout = new QHBoxLayout(inputRow);
	inputLayout->setContentsMargins(8, 6, 8, 2);
	inputLayout->setSpacing(6);
	sendPlatformIcon = new QLabel(inputRow);
	sendPlatformIcon->setFixedSize(22, 22);
	sendPlatformIcon->setScaledContents(true);
	sendPlatformIcon->setCursor(Qt::PointingHandCursor);
	sendPlatformIcon->installEventFilter(this);
	connect(sendPlatformIcon, &QLabel::linkActivated, this, [this](const QString &) { CycleSendPlatform(); });
	messageEdit = new QLineEdit(inputRow);
	messageEdit->setPlaceholderText(Tr("ChatDockMessagePlaceholder"));
	connect(messageEdit, &QLineEdit::returnPressed, this, &ChatDock::SendChatMessage);
	inputLayout->addWidget(sendPlatformIcon);
	inputLayout->addWidget(messageEdit, 1);
	root->addWidget(inputRow);

	sendStatus = new QLabel(this);
	sendStatus->setStyleSheet(QStringLiteral("color: #9a9a9a; padding: 0 10px 8px 10px;"));
	root->addWidget(sendStatus);
	UpdateSendStatus();
}

bool ChatDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == sendPlatformIcon && event->type() == QEvent::MouseButtonRelease) {
		CycleSendPlatform();
		return true;
	}
	if (messageList && watched == messageList->viewport() && event->type() == QEvent::Resize)
		RelayoutMessageItems();
	return QFrame::eventFilter(watched, event);
}

bool ChatDock::DockIsOpen() const
{
	if (auto *dw = qobject_cast<QDockWidget *>(parentWidget()))
		return dw->isVisible();
	return isVisible();
}

void ChatDock::showEvent(QShowEvent *event)
{
	QFrame::showEvent(event);
	RefreshAvailablePlatforms();
	if (!connectionsActive)
		StartConnections();
	else
		EnsurePlatformConnections();
	RefreshViewers();
	/* If OBS is already live, STREAMING_STARTED already fired — rediscover YouTube chat now. */
	if (obs_frontend_streaming_active())
		QTimer::singleShot(1500, this, &ChatDock::OnStreamingStarted);
	QTimer::singleShot(0, this, &ChatDock::RelayoutMessageItems);
}

void ChatDock::hideEvent(QHideEvent *event)
{
	QFrame::hideEvent(event);
	/* Keep sockets alive while the dock exists. Stopping on hide caused
	 * connect/disconnect spam when OBS toggled visibility. */
}

void ChatDock::ReloadAccounts()
{
	for (int i = 0; i < 3; ++i)
		LoadAccount(Platform(i));
	if (!DockIsOpen() && !connectionsActive)
		return;
	/* Full restart so a newly linked YouTube/Kick account connects immediately. */
	StartConnections();
	RefreshViewers();
}

void ChatDock::SyncAccountsFromConfig()
{
	QString previous[3];
	for (int i = 0; i < 3; ++i)
		previous[i] = accounts[i].accessToken;
	for (int i = 0; i < 3; ++i)
		LoadAccount(Platform(i));
	for (int i = 0; i < 3; ++i) {
		if (previous[i] == accounts[i].accessToken)
			continue;
		if (i == Twitch) {
			twitchConnectStarted = false;
			twitchConnectedAnnounced = false;
			twitchNotLinkedAnnounced = false;
			DisconnectTwitchIrc();
		} else if (i == YouTube) {
			youtubeConnectStarted = false;
			youtubeConnectedAnnounced = false;
			youtubeWaitingAnnounced = false;
			youtubePollInFlight = false;
			youtubePageTokenFails = 0;
			youtubeNotLinkedAnnounced = false;
			ytLiveChatId.clear();
			ytVideoId.clear();
			ytNextPage.clear();
		} else {
			kickConnectStarted = false;
			kickConnectedAnnounced = false;
			kickNotLinkedAnnounced = false;
			DisconnectKickChat();
			kickChatroomId.clear();
			kickSlug.clear();
		}
	}
	if (!connectionsActive) {
		if (DockIsOpen())
			StartConnections();
		return;
	}
	EnsurePlatformConnections();
}

void ChatDock::RefreshAvailablePlatforms()
{
	availablePlatforms.clear();
	if (obs_service_t *service = obs_frontend_get_streaming_service()) {
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		const StreamPlatformKind kind = DetectStreamPlatformFromEndpoint(QString::fromUtf8(url ? url : ""));
		if (kind == StreamPlatformKind::Twitch)
			availablePlatforms.insert(int(Twitch));
		else if (kind == StreamPlatformKind::YouTube)
			availablePlatforms.insert(int(YouTube));
		else if (kind == StreamPlatformKind::Kick)
			availablePlatforms.insert(int(Kick));
	}
	if (current_profile_config) {
		obs_data_array_t *outputs = obs_data_get_array(current_profile_config, "outputs");
		if (outputs) {
			const size_t count = obs_data_array_count(outputs);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *item = obs_data_array_item(outputs, i);
				const char *type = obs_data_get_string(item, "type");
				if (type && *type && strcmp(type, "stream") != 0) {
					obs_data_release(item);
					continue;
				}
				QString server = QString::fromUtf8(obs_data_get_string(item, "stream_server"));
				if (server.isEmpty())
					server = QString::fromUtf8(obs_data_get_string(item, "server"));
				obs_data_release(item);
				const StreamPlatformKind kind = DetectStreamPlatformFromEndpoint(server);
				if (kind == StreamPlatformKind::Twitch)
					availablePlatforms.insert(int(Twitch));
				else if (kind == StreamPlatformKind::YouTube)
					availablePlatforms.insert(int(YouTube));
				else if (kind == StreamPlatformKind::Kick)
					availablePlatforms.insert(int(Kick));
			}
			obs_data_array_release(outputs);
		}
	}

	for (int i = 0; i < 3; ++i)
		LoadAccount(Platform(i));

	if (!availablePlatforms.contains(sendPlatform) && !availablePlatforms.isEmpty())
		sendPlatform = *availablePlatforms.constBegin();
	RebuildViewerBar();
	UpdateSendStatus();
	if (connectionsActive)
		EnsurePlatformConnections();
}

void ChatDock::RebuildViewerBar()
{
	for (int i = 0; i < 3; ++i) {
		const bool show = availablePlatforms.contains(i);
		viewerChips[i]->setVisible(show);
		if (!show)
			viewerLabels[i]->setText(QStringLiteral("—"));
	}
}

void ChatDock::CycleSendPlatform()
{
	if (availablePlatforms.isEmpty())
		return;
	QList<int> order = {int(Twitch), int(YouTube), int(Kick)};
	int idx = order.indexOf(sendPlatform);
	for (int step = 1; step <= order.size(); ++step) {
		const int next = order[(idx + step) % order.size()];
		if (availablePlatforms.contains(next) && !accounts[next].accessToken.isEmpty()) {
			sendPlatform = next;
			break;
		}
		if (availablePlatforms.contains(next)) {
			sendPlatform = next;
			break;
		}
	}
	UpdateSendStatus();
}

void ChatDock::UpdateSendStatus()
{
	sendPlatformIcon->setPixmap(PlatformIcon(Platform(sendPlatform)).pixmap(22, 22));
	sendStatus->setText(Tr("ChatDockSendingTo").arg(PlatformLabel(Platform(sendPlatform))));
}

void ChatDock::LoadAccount(Platform platform)
{
	Account &acc = accounts[platform];
	acc = {};
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, PlatformKey(platform)) : nullptr;
	if (one) {
		acc.clientId = QString::fromUtf8(obs_data_get_string(one, "client_id"));
		acc.clientSecret = QString::fromUtf8(obs_data_get_string(one, "client_secret"));
		acc.accessToken = QString::fromUtf8(obs_data_get_string(one, "access_token"));
		acc.refreshToken = QString::fromUtf8(obs_data_get_string(one, "refresh_token"));
		acc.expiresAt = obs_data_get_int(one, "expires_at");
		acc.userId = QString::fromUtf8(obs_data_get_string(one, "user_id"));
		acc.userName = QString::fromUtf8(obs_data_get_string(one, "user_name"));
		acc.login = acc.userName.toLower();
		if (platform == YouTube) {
			/* Never restore the last video/chat. A saved id is the previous live and
			 * its history shows up again the next time OBS opens. */
			obs_data_erase(one, "youtube_video_id");
			obs_data_erase(one, "youtube_live_chat_id");
			obs_data_set_obj(root, PlatformKey(YouTube), one);
			obs_data_set_obj(current_profile_config, "stream_accounts", root);
		}
	}
	obs_data_release(one);
	obs_data_release(root);
}

void ChatDock::EnsureToken(Platform platform, std::function<void(bool)> done)
{
	Account &acc = accounts[platform];
	if (acc.accessToken.isEmpty()) {
		done(false);
		return;
	}
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	if (acc.expiresAt <= 0 || acc.expiresAt > now + 60) {
		done(true);
		return;
	}
	if (acc.refreshToken.isEmpty() || acc.clientId.isEmpty()) {
		done(false);
		return;
	}
	QUrl url;
	QUrlQuery query;
	query.addQueryItem("grant_type", "refresh_token");
	query.addQueryItem("refresh_token", acc.refreshToken);
	query.addQueryItem("client_id", acc.clientId);
	if (platform == Twitch) {
		url = QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"));
	} else if (platform == YouTube) {
		url = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
		query.addQueryItem("client_secret", acc.clientSecret);
	} else {
		url = QUrl(QStringLiteral("https://id.kick.com/oauth/token"));
		query.addQueryItem("client_secret", acc.clientSecret);
	}
	Http(url.toString(), {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/x-www-form-urlencoded")}},
	     query.query(QUrl::FullyEncoded).toUtf8(), "POST",
	     [this, platform, done](int status, const QByteArray &body, const QString &) {
		     Account &acc = accounts[platform];
		     const QJsonObject json = QJsonDocument::fromJson(body).object();
		     if (status < 200 || status >= 300 || !json.contains("access_token")) {
			     done(false);
			     return;
		     }
		     acc.accessToken = json.value("access_token").toString();
		     if (json.contains("refresh_token"))
			     acc.refreshToken = json.value("refresh_token").toString();
		     const int expiresIn = json.value("expires_in").toInt(0);
		     acc.expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
		     if (current_profile_config) {
			     obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
			     obs_data_t *one = root ? obs_data_get_obj(root, PlatformKey(platform)) : nullptr;
			     if (one) {
				     obs_data_set_string(one, "access_token", acc.accessToken.toUtf8().constData());
				     obs_data_set_string(one, "refresh_token", acc.refreshToken.toUtf8().constData());
				     obs_data_set_int(one, "expires_at", acc.expiresAt);
				     obs_data_set_obj(root, PlatformKey(platform), one);
				     obs_data_set_obj(current_profile_config, "stream_accounts", root);
			     }
			     obs_data_release(one);
			     obs_data_release(root);
		     }
		     done(true);
	     });
}

void ChatDock::Http(const QString &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body,
		    const char *verb, std::function<void(int, const QByteArray &, const QString &)> done)
{
	const QByteArray urlBytes = url.toUtf8();
	const QByteArray verbBytes(verb);
	const QByteArray payload = body;
	const auto hdrs = headers;
	RunAsync(
		this,
		[urlBytes, verbBytes, payload, hdrs] {
			HttpResult result;
			CURL *curl = curl_easy_init();
			if (!curl) {
				result.error = QStringLiteral("curl_easy_init failed");
				return result;
			}
			curl_slist *list = nullptr;
			for (const auto &header : hdrs) {
				const QByteArray line = header.first + ": " + header.second;
				list = curl_slist_append(list, line.constData());
			}
			curl_easy_setopt(curl, CURLOPT_URL, urlBytes.constData());
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
			/* Emoji CDNs (jsDelivr) answer with a redirect. Without this the image is dropped
			 * and the chat shows an empty slot. */
			if (verbBytes == "GET") {
				curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
				curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
			}
			if (verbBytes != "GET") {
				curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verbBytes.constData());
				/* DELETE liveChat/messages and liveChat/bans must not send a body. */
				if (!payload.isEmpty()) {
					curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.constData());
					curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
				}
			}
			result = CurlPerform(curl);
			curl_slist_free_all(list);
			curl_easy_cleanup(curl);
			return result;
		},
		std::move(done));
}

void ChatDock::TryStartPlatform(Platform platform)
{
	if (!availablePlatforms.contains(platform))
		return;

	bool &started = platform == YouTube ? youtubeConnectStarted
					    : (platform == Kick ? kickConnectStarted : twitchConnectStarted);
	bool &notLinked = platform == YouTube ? youtubeNotLinkedAnnounced
					      : (platform == Kick ? kickNotLinkedAnnounced : twitchNotLinkedAnnounced);

	if (accounts[platform].accessToken.isEmpty()) {
		started = false;
		if (!notLinked) {
			notLinked = true;
			AppendSystemMessage(Tr("ChatDockNotLinked").arg(PlatformLabel(platform)), platform);
		}
		return;
	}

	notLinked = false;
	if (started)
		return;
	started = true;
	AppendSystemMessage(Tr("ChatDockConnecting").arg(PlatformLabel(platform)), platform);
	if (platform == Twitch) {
		ConnectTwitchIrc();
	} else if (platform == YouTube) {
		StartYouTubeChat();
		if (!youtubeDiscoverTimer->isActive())
			youtubeDiscoverTimer->start();
	} else {
		ResolveKickChatroom();
	}
}

void ChatDock::EnsurePlatformConnections()
{
	if (!connectionsActive)
		return;
	for (int i = 0; i < 3; ++i)
		LoadAccount(Platform(i));
	TryStartPlatform(Twitch);
	TryStartPlatform(YouTube);
	TryStartPlatform(Kick);
	if (!viewerTimer->isActive())
		viewerTimer->start();
}

void ChatDock::StartConnections()
{
	/* Preserve YouTube ids across restart — StopConnections clears them, LoadAccount restores from config. */
	StopConnections();
	connectionsActive = true;
	EnsurePlatformConnections();
	RefreshViewers();
	if (!ytLiveChatId.isEmpty() && !youtubePollInFlight)
		PollYouTubeChat();
	if (obs_frontend_streaming_active())
		QTimer::singleShot(2000, this, &ChatDock::OnStreamingStarted);
}

void ChatDock::StopConnections()
{
	viewerTimer->stop();
	youtubeTimer->stop();
	youtubeDiscoverTimer->stop();
	if (youtubeWaitLiveTimer)
		youtubeWaitLiveTimer->stop();
	kickReconnectTimer->stop();
	twitchReconnectTimer->stop();
	DisconnectTwitchIrc();
	DisconnectKickChat();
	ytLiveChatId.clear();
	ytVideoId.clear();
	ytNextPage.clear();
	ytContinuation.clear();
	ytInnertubeClientVersion.clear();
	ytInnertubeApiKey.clear();
	ytInnertubeVisitor.clear();
	ytDeletePageTokens.clear();
	youtubeIgnoreBeforeMs = 0;
	youtubeCatchingUp = true;
	youtubeCatchupPages = 0;
	ytPendingDeletes.clear();
	ytBanIds.clear();
	kickSlug.clear();
	kickChatroomId.clear();
	kickChannelId.clear();
	kickRealtimeClientId.clear();
	connectionsActive = false;
	twitchConnectedAnnounced = false;
	kickConnectedAnnounced = false;
	youtubeConnectedAnnounced = false;
	youtubeWaitingAnnounced = false;
	youtubeQuotaAnnounced = false;
	youtubePollInFlight = false;
	youtubePageTokenFails = 0;
	youtubeLastFullDiscoverMs = 0;
	youtubeLastViewerFetchMs = 0;
	twitchConnectStarted = false;
	youtubeConnectStarted = false;
	kickConnectStarted = false;
	twitchNotLinkedAnnounced = false;
	youtubeNotLinkedAnnounced = false;
	kickNotLinkedAnnounced = false;
}

void ChatDock::RefreshViewers()
{
	if (availablePlatforms.contains(Twitch))
		FetchTwitchViewers();
	if (availablePlatforms.contains(YouTube))
		FetchYouTubeViewers();
	if (availablePlatforms.contains(Kick))
		FetchKickViewers();
}

void ChatDock::SetViewerCount(Platform platform, int count, bool live)
{
	/* Each chip owns its own QLabel — Twitch/YouTube/Kick never share a counter. */
	QLabel *label = viewerLabels[int(platform)];
	if (!label)
		return;
	if (!live)
		label->setText(QStringLiteral("—"));
	else
		label->setText(QString::number(qMax(0, count)));
}

void ChatDock::FetchTwitchViewers()
{
	EnsureToken(Twitch, [this](bool ok) {
		if (!ok) {
			SetViewerCount(Twitch, 0, false);
			return;
		}
		const Account &acc = accounts[Twitch];
		if (acc.clientId.isEmpty()) {
			SetViewerCount(Twitch, 0, false);
			return;
		}
		auto requestStreams = [this](const QString &userId) {
			if (userId.isEmpty()) {
				SetViewerCount(Twitch, 0, false);
				return;
			}
			const Account &a = accounts[Twitch];
			const QString url = QStringLiteral("https://api.twitch.tv/helix/streams?user_id=") + userId;
			Http(url,
			     {{QByteArrayLiteral("Client-Id"), a.clientId.toUtf8()},
			      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + a.accessToken.toUtf8()}},
			     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
				     if (status < 200 || status >= 300) {
					     SetViewerCount(Twitch, 0, false);
					     return;
				     }
				     const QJsonArray data = QJsonDocument::fromJson(body).object().value("data").toArray();
				     if (data.isEmpty()) {
					     SetViewerCount(Twitch, 0, false);
					     return;
				     }
				     SetViewerCount(Twitch, data.at(0).toObject().value("viewer_count").toInt(), true);
			     });
		};
		if (!acc.userId.isEmpty()) {
			requestStreams(acc.userId);
			return;
		}
		Http(QStringLiteral("https://api.twitch.tv/helix/users"),
		     {{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
		      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}},
		     {}, "GET", [this, requestStreams](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300) {
				     SetViewerCount(Twitch, 0, false);
				     return;
			     }
			     const QJsonArray data = QJsonDocument::fromJson(body).object().value("data").toArray();
			     if (data.isEmpty()) {
				     SetViewerCount(Twitch, 0, false);
				     return;
			     }
			     accounts[Twitch].userId = data.at(0).toObject().value("id").toString();
			     requestStreams(accounts[Twitch].userId);
		     });
	});
}

void ChatDock::FetchYouTubeViewers()
{
	/* Cap viewer polls — each videos.list costs quota; 5s was burning the daily budget. */
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - youtubeLastViewerFetchMs < 20000 && youtubeLastViewerFetchMs > 0)
		return;
	youtubeLastViewerFetchMs = now;
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok) {
			SetViewerCount(YouTube, 0, false);
			return;
		}
		auto ytAuth = [this] {
			return QList<QPair<QByteArray, QByteArray>>{
				{QByteArrayLiteral("Authorization"),
				 QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}};
		};
		auto applyVideoId = [this, ytAuth](const QString &videoId) {
			if (videoId.isEmpty()) {
				SetViewerCount(YouTube, 0, false);
				return;
			}
			if (ytVideoId.isEmpty())
				ytVideoId = videoId;
			const QString url =
				QStringLiteral("https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=") +
				videoId;
			Http(url, ytAuth(), {}, "GET", [this, videoId](int status, const QByteArray &body, const QString &) {
				if (status < 200 || status >= 300) {
					SetViewerCount(YouTube, 0, false);
					return;
				}
				const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
				if (items.isEmpty()) {
					SetViewerCount(YouTube, 0, false);
					return;
				}
				const QJsonObject details =
					items.at(0).toObject().value("liveStreamingDetails").toObject();
				const QString chatId = YouTubeVideoActiveChatId(details);
				if (!chatId.isEmpty() &&
				    BindYouTubeChat(chatId, videoId) && connectionsActive && !youtubePollInFlight)
					PollYouTubeChat();
				const QString viewers = details.value("concurrentViewers").toString();
				if (viewers.isEmpty())
					SetViewerCount(YouTube, 0, false);
				else
					SetViewerCount(YouTube, viewers.toInt(), true);
			});
		};
		if (!ytVideoId.isEmpty()) {
			applyVideoId(ytVideoId);
			return;
		}
		/* Discover an active broadcast so the YouTube chip can update without waiting for chat. */
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "id,snippet,status");
		query.addQueryItem("broadcastStatus", "active");
		query.addQueryItem("broadcastType", "all");
		query.addQueryItem("maxResults", "5");
		url.setQuery(query);
		Http(url.toString(), ytAuth(), {}, "GET",
		     [this, applyVideoId](int status, const QByteArray &body, const QString &) {
			     if (status >= 200 && status < 300) {
				     const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
				     if (!items.isEmpty()) {
					     const QJsonObject item = items.at(0).toObject();
					     const QString id = item.value("id").toString();
					     const QString life =
						     item.value("status").toObject().value("lifeCycleStatus").toString();
					     if (YouTubeBroadcastLifeDead(life)) {
						     SetViewerCount(YouTube, 0, false);
						     return;
					     }
					     const QString chatId =
						     YouTubeBroadcastChatId(item.value("snippet").toObject());
					     if (!chatId.isEmpty() && BindYouTubeChat(chatId, id) && connectionsActive &&
						 !youtubePollInFlight)
						     PollYouTubeChat();
					     applyVideoId(id);
					     return;
				     }
			     }
			     /* Viewers only. Chat discovery uses liveBroadcasts, not search.list. */
			     SetViewerCount(YouTube, 0, false);
		     });
	});
}

void ChatDock::FetchKickViewers()
{
	EnsureToken(Kick, [this](bool ok) {
		if (!ok) {
			SetViewerCount(Kick, 0, false);
			return;
		}
		/* Kick only: never write Twitch/YouTube labels from this path. */
		auto setKickOffline = [this] { SetViewerCount(Kick, 0, false); };
		auto setKickLive = [this](int count) { SetViewerCount(Kick, qMax(0, count), true); };

		auto jsonIsLive = [](const QJsonValue &value) -> bool {
			if (value.isBool())
				return value.toBool();
			if (value.isDouble())
				return value.toInt() != 0;
			if (value.isString()) {
				const QString s = value.toString().trimmed().toLower();
				return s == QLatin1String("true") || s == QLatin1String("1");
			}
			return false;
		};
		auto jsonViewerCount = [](const QJsonObject &obj) -> int {
			const QJsonValue v = obj.value(QStringLiteral("viewer_count"));
			if (v.isUndefined() || v.isNull()) {
				const QJsonValue alt = obj.value(QStringLiteral("viewers"));
				if (alt.isUndefined() || alt.isNull())
					return -1;
				if (alt.isString())
					return alt.toString().toInt();
				return alt.toInt(-1);
			}
			if (v.isString())
				return v.toString().toInt();
			return v.toInt(-1);
		};

		auto fromPublicSlug = [this, setKickOffline, setKickLive, jsonIsLive, jsonViewerCount] {
			if (kickSlug.isEmpty()) {
				setKickOffline();
				return;
			}
			const QList<QPair<QByteArray, QByteArray>> browserHeaders = {
				{QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")},
				{QByteArrayLiteral("Accept-Language"), QByteArrayLiteral("en-US,en;q=0.9")},
				{QByteArrayLiteral("Origin"), QByteArrayLiteral("https://kick.com")},
				{QByteArrayLiteral("Referer"), QByteArrayLiteral("https://kick.com/")},
				{QByteArrayLiteral("User-Agent"),
				 QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
						   "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")},
			};
			Http(QStringLiteral("https://kick.com/api/v2/channels/") +
				     QString::fromUtf8(QUrl::toPercentEncoding(kickSlug)),
			     browserHeaders, {}, "GET",
			     [setKickOffline, setKickLive, jsonIsLive, jsonViewerCount](int status, const QByteArray &body,
											const QString &) {
				     if (status < 200 || status >= 300) {
					     setKickOffline();
					     return;
				     }
				     const QJsonObject root = QJsonDocument::fromJson(body).object();
				     const QJsonValue liveVal = root.value(QStringLiteral("livestream"));
				     /* Offline channels return livestream: null. */
				     if (!liveVal.isObject()) {
					     setKickOffline();
					     return;
				     }
				     const QJsonObject live = liveVal.toObject();
				     if (live.contains(QStringLiteral("is_live")) &&
					 !jsonIsLive(live.value(QStringLiteral("is_live")))) {
					     setKickOffline();
					     return;
				     }
				     const int count = jsonViewerCount(live);
				     if (count < 0)
					     setKickOffline();
				     else
					     setKickLive(count);
			     });
		};

		auto fromChannels = [this, setKickOffline, setKickLive, jsonIsLive, jsonViewerCount, fromPublicSlug] {
			const Account &a = accounts[Kick];
			Http(QStringLiteral("https://api.kick.com/public/v1/channels"),
			     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + a.accessToken.toUtf8()},
			      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
			     {}, "GET",
			     [this, setKickOffline, setKickLive, jsonIsLive, jsonViewerCount,
			      fromPublicSlug](int status, const QByteArray &body, const QString &) {
				     if (status < 200 || status >= 300) {
					     fromPublicSlug();
					     return;
				     }
				     const QJsonObject channel = FirstDataObject(body);
				     if (kickSlug.isEmpty())
					     kickSlug = channel.value(QStringLiteral("slug")).toString();
				     const QJsonObject stream = channel.value(QStringLiteral("stream")).toObject();
				     /* Require an explicit live flag — missing is_live must NOT default to true. */
				     if (stream.isEmpty() || !jsonIsLive(stream.value(QStringLiteral("is_live")))) {
					     setKickOffline();
					     return;
				     }
				     const int count = jsonViewerCount(stream);
				     if (count < 0)
					     setKickOffline();
				     else
					     setKickLive(count);
			     });
		};

		const Account &acc = accounts[Kick];
		if (acc.userId.isEmpty()) {
			fromChannels();
			return;
		}
		QUrl url(QStringLiteral("https://api.kick.com/public/v1/users/livestreams"));
		QUrlQuery query;
		query.addQueryItem("user_id", acc.userId);
		url.setQuery(query);
		Http(url.toString(),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     {}, "GET",
		     [setKickOffline, setKickLive, jsonViewerCount, fromChannels](int status, const QByteArray &body,
										  const QString &) {
			     if (status < 200 || status >= 300) {
				     fromChannels();
				     return;
			     }
			     /* 200 + empty data ⇒ offline. Do not fall back to channel stream leftovers. */
			     const QJsonObject live = FirstDataObject(body);
			     if (live.isEmpty()) {
				     setKickOffline();
				     return;
			     }
			     const int count = jsonViewerCount(live);
			     if (count < 0)
				     setKickOffline();
			     else
				     setKickLive(count);
		     });
	});
}

void ChatDock::ConnectTwitchIrc()
{
	DisconnectTwitchIrc();
	if (!connectionsActive)
		return;
	EnsureToken(Twitch, [this](bool ok) {
		if (!ok || !connectionsActive) {
			twitchConnectStarted = false;
			AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(Twitch)), Twitch);
			return;
		}
		const Account &acc = accounts[Twitch];
		Http(QStringLiteral("https://api.twitch.tv/helix/users"),
		     {{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
		      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}},
		     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (!connectionsActive)
				     return;
			     if (status < 200 || status >= 300) {
				     AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(Twitch)), Twitch);
				     if (!twitchReconnectTimer->isActive())
					     twitchReconnectTimer->start(8000);
				     return;
			     }
			     const QJsonArray data = QJsonDocument::fromJson(body).object().value("data").toArray();
			     if (data.isEmpty())
				     return;
			     const QJsonObject user = data.at(0).toObject();
			     twitchLogin = user.value("login").toString().toLower();
			     accounts[Twitch].userId = user.value("id").toString();
			     accounts[Twitch].login = twitchLogin;
			     accounts[Twitch].userName = user.value("display_name").toString(twitchLogin);
			     /* Prefetch badge CDN URLs before chat traffic so we never fall back to letter pills. */
			     FetchTwitchBadgeSets();

			     const QByteArray token = accounts[Twitch].accessToken.toUtf8();
			     const QByteArray nick = twitchLogin.toUtf8();
			     twitchJoined = false;
			     twitchBuffer.clear();
			     twitchWsGen++;
			     const int gen = twitchWsGen;
			     auto session = std::make_shared<ChatCurlWsSession>();
			     twitchWs = session;
			     QPointer<ChatDock> alive(this);
			     std::thread([alive, session, gen, token, nick] {
				     CURL *curl = curl_easy_init();
				     if (!curl)
					     return;
				     ApplyCurlSsl(curl);
				     curl_easy_setopt(curl, CURLOPT_URL, "wss://irc-ws.chat.twitch.tv:443");
				     curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
				     const CURLcode connectRc = curl_easy_perform(curl);
				     if (connectRc != CURLE_OK) {
					     blog(LOG_WARNING, "[Stream Suite] Twitch WS connect failed: %s",
						  curl_easy_strerror(connectRc));
					     curl_easy_cleanup(curl);
					     if (alive) {
						     QMetaObject::invokeMethod(
							     alive, [alive] {
								     if (!alive || !alive->connectionsActive)
									     return;
								     if (!alive->twitchReconnectTimer->isActive())
									     alive->twitchReconnectTimer->start(8000);
							     },
							     Qt::QueuedConnection);
					     }
					     return;
				     }

				     CurlWsSendText(curl, "PASS oauth:" + token + "\r\n");
				     CurlWsSendText(curl, "NICK " + nick + "\r\n");
				     CurlWsSendText(curl, "CAP REQ :twitch.tv/tags twitch.tv/commands twitch.tv/echo-message\r\n");

				     QByteArray lineBuf;
				     while (!session->stop.load()) {
					     for (const QByteArray &out : session->takeOutbound()) {
						     if (!CurlWsSendText(curl, out)) {
							     session->stop = true;
							     break;
						     }
					     }
					     char buffer[4096];
					     size_t n = 0;
					     const curl_ws_frame *meta = nullptr;
					     const CURLcode rc = curl_ws_recv(curl, buffer, sizeof(buffer), &n, &meta);
					     if (rc == CURLE_AGAIN) {
						     CurlWsWaitReadable(curl, 200);
						     continue;
					     }
					     if (rc != CURLE_OK || !meta)
						     break;
					     if (meta->flags & CURLWS_CLOSE)
						     break;
					     if (meta->flags & CURLWS_PING) {
						     size_t sent = 0;
						     curl_ws_send(curl, buffer, n, &sent, 0, CURLWS_PONG);
						     continue;
					     }
					     if (!(meta->flags & CURLWS_TEXT) || n == 0)
						     continue;
					     lineBuf.append(buffer, static_cast<int>(n));
					     while (true) {
						     int idx = lineBuf.indexOf("\r\n");
						     int skip = 2;
						     if (idx < 0) {
							     idx = lineBuf.indexOf('\n');
							     skip = 1;
						     }
						     if (idx < 0)
							     break;
						     const QString line = QString::fromUtf8(lineBuf.left(idx)).trimmed();
						     lineBuf.remove(0, idx + skip);
						     if (line.isEmpty() || !alive)
							     continue;
						     QMetaObject::invokeMethod(
							     alive,
							     [alive, gen, line] {
								     if (!alive || alive->twitchWsGen != gen)
									     return;
								     alive->HandleTwitchLine(line);
							     },
							     Qt::QueuedConnection);
					     }
				     }
				     curl_easy_cleanup(curl);
				     if (alive) {
					     QMetaObject::invokeMethod(
						     alive,
						     [alive, gen] {
							     if (!alive || alive->twitchWsGen != gen || !alive->connectionsActive)
								     return;
							     alive->twitchJoined = false;
							     if (!alive->twitchReconnectTimer->isActive())
								     alive->twitchReconnectTimer->start(8000);
						     },
						     Qt::QueuedConnection);
				     }
			     }).detach();
		     });
	});
}

void ChatDock::DisconnectTwitchIrc()
{
	twitchWsGen++;
	if (twitchWs) {
		twitchWs->stop = true;
		twitchWs.reset();
	}
	twitchJoined = false;
	twitchBuffer.clear();
}

void ChatDock::HandleTwitchLine(const QString &line)
{
	if (line.startsWith(QStringLiteral("PING"))) {
		const QByteArray payload = line.section(QLatin1Char(':'), 1).toUtf8();
		const QByteArray pong =
			"PONG :" + (payload.isEmpty() ? QByteArrayLiteral("tmi.twitch.tv") : payload) + "\r\n";
		if (twitchWs)
			twitchWs->enqueue(pong);
		return;
	}
	if (line.contains(QStringLiteral("Login authentication failed"), Qt::CaseInsensitive) ||
	    line.contains(QStringLiteral("Improperly formatted auth"), Qt::CaseInsensitive)) {
		AppendSystemMessage(Tr("ChatDockTwitchReauth"), Twitch);
		blog(LOG_WARNING, "[Stream Suite] Twitch IRC auth failed — reconnect with chat:read chat:edit");
		return;
	}
	if ((line.contains(QStringLiteral(" 001 ")) || line.contains(QStringLiteral("GLOBALUSERSTATE"))) && !twitchJoined &&
	    !twitchLogin.isEmpty()) {
		twitchJoined = true;
		if (twitchWs)
			twitchWs->enqueue("JOIN #" + twitchLogin.toUtf8() + "\r\n");
		if (!twitchConnectedAnnounced) {
			twitchConnectedAnnounced = true;
			AppendSystemMessage(Tr("ChatDockConnected").arg(PlatformLabel(Twitch)), Twitch);
			FetchTwitchBadgeSets();
		}
		return;
	}

	QString tags;
	QString rest = line;
	if (rest.startsWith(QLatin1Char('@'))) {
		const int sp = rest.indexOf(QLatin1Char(' '));
		tags = rest.mid(1, sp - 1);
		rest = rest.mid(sp + 1);
	}
	auto tagValue = [&tags](const QString &key) -> QString {
		for (const QString &part : tags.split(QLatin1Char(';'))) {
			if (part.section(QLatin1Char('='), 0, 0) == key)
				return part.section(QLatin1Char('='), 1);
		}
		return {};
	};

	/* Single message delete. */
	if (rest.contains(QStringLiteral("CLEARMSG"))) {
		const QString msgId = tagValue(QStringLiteral("target-msg-id"));
		if (!msgId.isEmpty())
			MarkMessageDeleted(msgId, Tr("ChatDockMessageRemoved"));
		return;
	}
	/* Clear all chat, or all messages from one user (ban/timeout). */
	if (rest.contains(QStringLiteral("CLEARCHAT"))) {
		const int colon = rest.lastIndexOf(QStringLiteral(" :"));
		const QString targetUser = colon >= 0 ? rest.mid(colon + 2).trimmed() : QString();
		if (targetUser.isEmpty()) {
			MarkPlatformMessagesDeleted(Twitch, Tr("ChatDockChatCleared"));
			AppendSystemMessage(Tr("ChatDockChatCleared"), Twitch);
		} else {
			const QString tip = Tr("ChatDockMessageRemovedUser").arg(targetUser);
			MarkMessagesDeletedByUser(Twitch, targetUser, tip);
			AppendSystemMessage(tip, Twitch);
		}
		return;
	}

	if (!rest.contains(QStringLiteral("PRIVMSG")))
		return;

	const int excl = rest.indexOf(QLatin1Char('!'));
	const int priv = rest.indexOf(QStringLiteral("PRIVMSG"));
	const int colon = rest.indexOf(QStringLiteral(" :"), priv >= 0 ? priv : 0);
	if (colon < 0)
		return;
	ChatMessage msg;
	msg.platform = Twitch;
	msg.text = rest.mid(colon + 2);
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.user = excl > 0 ? rest.left(excl).section(QLatin1Char(':'), -1) : QStringLiteral("twitch");
	msg.color = QStringLiteral("#BF94FF");
	QString badgesTag;
	QString emotesTag;
	for (const QString &part : tags.split(QLatin1Char(';'))) {
		const QString key = part.section(QLatin1Char('='), 0, 0);
		const QString val = part.section(QLatin1Char('='), 1);
		if (key == QLatin1String("display-name") && !val.isEmpty())
			msg.user = val;
		else if (key == QLatin1String("color") && !val.isEmpty())
			msg.color = val;
		else if (key == QLatin1String("id"))
			msg.id = val;
		else if (key == QLatin1String("user-id") && !val.isEmpty())
			msg.actorId = val;
		else if (key == QLatin1String("badges"))
			badgesTag = val;
		else if (key == QLatin1String("emotes"))
			emotesTag = val;
		else if (key == QLatin1String("login") && !val.isEmpty())
			msg.userLogin = val.toLower();
	}
	if (msg.userLogin.isEmpty())
		msg.userLogin = (excl > 0 ? rest.left(excl).section(QLatin1Char(':'), -1) : msg.user).toLower();
	msg.badges = TwitchBadgesFromTags(badgesTag);
	msg.emotes = TwitchEmotesFromTag(emotesTag);
	if (msg.id.isEmpty())
		msg.id = QStringLiteral("tw-%1-%2").arg(msg.ts).arg(msg.user);
	const bool deleteAfter = TakeTwitchEcho(msg);
	AppendMessage(msg);
	if (deleteAfter && !msg.id.startsWith(QLatin1String("local-tw-")) && !msg.id.startsWith(QLatin1String("tw-")))
		DeleteTwitchMessage(msg.id);
}

void ChatDock::SendTwitchMessage(const QString &text)
{
	if (!twitchWs || !twitchJoined || twitchLogin.isEmpty())
		return;
	twitchWs->enqueue("PRIVMSG #" + twitchLogin.toUtf8() + " :" + text.toUtf8() + "\r\n");
	ChatMessage msg;
	msg.platform = Twitch;
	msg.user = accounts[Twitch].userName.isEmpty() ? twitchLogin : accounts[Twitch].userName;
	msg.userLogin = twitchLogin;
	msg.actorId = accounts[Twitch].userId;
	msg.echo = true;
	msg.color = QStringLiteral("#9146FF");
	msg.text = text;
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.id = QStringLiteral("local-tw-%1").arg(msg.ts);
	AppendMessage(msg);
}

void ChatDock::ReportYouTubeHttpError(int status, const QByteArray &body)
{
	if (YouTubeBodyLooksLikeQuota(body)) {
		if (!youtubeQuotaAnnounced) {
			youtubeQuotaAnnounced = true;
			AppendSystemMessage(Tr("ChatDockYouTubeQuota"), YouTube);
		}
		return;
	}
	if (status == 401) {
		youtubeConnectStarted = false;
		AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(YouTube)), YouTube);
		return;
	}
	if (status >= 400 && !youtubeConnectedAnnounced) {
		const QJsonObject err = QJsonDocument::fromJson(body).object().value("error").toObject();
		const QString msg = err.value("message").toString();
		if (!msg.isEmpty())
			AppendSystemMessage(Tr("ChatDockYouTubeApiError").arg(msg.left(120)), YouTube);
		else if (status == 403)
			AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(YouTube)), YouTube);
	}
}

void ChatDock::DebugYouTube(const QString &text)
{
	/* Chat stays clean — diagnostics only go to the OBS log (Help → Log Files). */
	blog(LOG_INFO, "[Stream Suite] YT-debug: %s", text.toUtf8().constData());
}

void ChatDock::EnsureYouTubeWaitLiveTimer()
{
	if (!youtubeWaitLiveTimer)
		return;
	if (!connectionsActive || youtubeConnectedAnnounced || ytVideoId.isEmpty() || !ytLiveChatId.isEmpty()) {
		youtubeWaitLiveTimer->stop();
		return;
	}
	if (!youtubeWaitLiveTimer->isActive())
		youtubeWaitLiveTimer->start();
}

void ChatDock::TickYouTubeWaitLive()
{
	if (!connectionsActive || youtubeConnectedAnnounced || ytVideoId.isEmpty()) {
		if (youtubeWaitLiveTimer)
			youtubeWaitLiveTimer->stop();
		return;
	}
	if (!ytLiveChatId.isEmpty()) {
		if (youtubeWaitLiveTimer)
			youtubeWaitLiveTimer->stop();
		if (!youtubePollInFlight && !youtubeTimer->isActive())
			PollYouTubeChat();
		return;
	}
	/* Cheap poll: videos.list for activeLiveChatId on the known ready/live video. */
	ResolveYouTubeLiveChatId(ytVideoId, [this](bool ok) {
		if (ok) {
			if (youtubeWaitLiveTimer)
				youtubeWaitLiveTimer->stop();
		} else if (obs_frontend_streaming_active()) {
			EnsureYouTubeWaitLiveTimer();
		} else if (youtubeWaitLiveTimer) {
			youtubeWaitLiveTimer->stop();
		}
	});
}

void ChatDock::OnStreamingStarted()
{
	if (!connectionsActive || !availablePlatforms.contains(YouTube))
		return;
	/* Encoder just went up: drop any chat id (may be a ready-broadcast stub that 404s) and rediscover.
	 * Keep ytVideoId if we have one — activeLiveChatId often appears a few seconds later. */
	/* New stream: drop the previous broadcast so its chat history is not polled again. */
	youtubeWaitingAnnounced = false;
	youtubeQuotaAnnounced = false;
	youtubeConnectedAnnounced = false;
	ytLiveChatId.clear();
	ytVideoId.clear();
	ytNextPage.clear();
	ytContinuation.clear();
	ytInnertubeClientVersion.clear();
	ytInnertubeApiKey.clear();
	ytInnertubeVisitor.clear();
	ytDeletePageTokens.clear();
	youtubeCatchingUp = true;
	youtubeCatchupPages = 0;
	ytPendingDeletes.clear();
	ytBanIds.clear();
	youtubeIgnoreBeforeMs = QDateTime::currentMSecsSinceEpoch() - 15000;
	ClearYouTubeMessages();
	DebugYouTube(QStringLiteral("streaming-started fresh discover"));
	StartYouTubeChat();
	EnsureYouTubeWaitLiveTimer();
	if (!youtubeDiscoverTimer->isActive())
		youtubeDiscoverTimer->start();
}

bool ChatDock::BindYouTubeChat(const QString &chatId, const QString &videoId)
{
	const QString previousVideo = ytVideoId;
	if (!videoId.isEmpty())
		ytVideoId = videoId;
	if (chatId.isEmpty() || chatId == ytLiveChatId)
		return false;
	/* Stream Info sends snippet.liveChatId and then activeLiveChatId for one video.
	 * Treating that as a new chat wiped the lines and the live cursor. */
	const bool sameVideo = !ytLiveChatId.isEmpty() && !previousVideo.isEmpty() &&
			       (videoId.isEmpty() || videoId == previousVideo);
	if (sameVideo) {
		ytLiveChatId = chatId;
		blog(LOG_INFO, "[Stream Suite] YouTube chat id updated for the same video (%d chars)", chatId.size());
		return false;
	}
	const bool switching = !ytLiveChatId.isEmpty();
	ytLiveChatId = chatId;
	ytNextPage.clear();
	ytContinuation.clear();
	ytInnertubeClientVersion.clear();
	ytInnertubeApiKey.clear();
	ytInnertubeVisitor.clear();
	ytDeletePageTokens.clear();
	youtubePageTokenFails = 0;
	youtubeCatchingUp = true;
	youtubeCatchupPages = 0;
	ytPendingDeletes.clear();
	ytBanIds.clear();
	youtubeIgnoreBeforeMs = QDateTime::currentMSecsSinceEpoch() - 15000;
	youtubeConnectedAnnounced = false;
	if (youtubeTimer)
		youtubeTimer->stop();
	if (switching)
		ClearYouTubeMessages();
	blog(LOG_INFO, "[Stream Suite] YouTube chat bound, history before this session is ignored");
	return true;
}

void ChatDock::ClearYouTubeMessages()
{
	if (!messageList)
		return;
	for (int i = messageList->count() - 1; i >= 0; --i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || item->data(Qt::UserRole + 4).toInt() != int(YouTube))
			continue;
		const QString id = item->data(Qt::UserRole + 1).toString();
		if (!id.isEmpty())
			seenMessageIds.remove(id);
		for (const QString &alias : item->data(Qt::UserRole + 6).toStringList()) {
			if (!alias.isEmpty())
				seenMessageIds.remove(alias);
		}
		delete messageList->takeItem(i);
	}
}

void ChatDock::PersistYouTubeIds()
{
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, PlatformKey(YouTube)) : nullptr;
	if (one) {
		/* Session-only. Writing these made the next OBS launch reopen the previous live. */
		obs_data_erase(one, "youtube_video_id");
		obs_data_erase(one, "youtube_live_chat_id");
		obs_data_set_obj(root, PlatformKey(YouTube), one);
		obs_data_set_obj(current_profile_config, "stream_accounts", root);
	}
	obs_data_release(one);
	obs_data_release(root);
}

void ChatDock::NotifyYouTubeVideo(const QString &videoId, const QString &liveChatId)
{
	if (videoId.isEmpty() && liveChatId.isEmpty())
		return;
	DebugYouTube(QStringLiteral("notify video=%1 chat=%2 conn=%3")
			     .arg(videoId.isEmpty() ? QStringLiteral("-") : videoId.left(11),
				  liveChatId.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes(%1)").arg(liveChatId.size()),
				  connectionsActive ? QStringLiteral("on") : QStringLiteral("off")));
	if (!videoId.isEmpty())
		ytVideoId = videoId;
	if (!liveChatId.isEmpty()) {
		const bool changed = BindYouTubeChat(liveChatId, videoId);
		youtubeWaitingAnnounced = false;
		youtubeQuotaAnnounced = false;
		PersistYouTubeIds();
		blog(LOG_INFO, "[Stream Suite] YouTube liveChatId from Stream Info (%d chars)", liveChatId.size());
		if (connectionsActive && !youtubePollInFlight && (changed || !youtubeTimer->isActive())) {
			DebugYouTube(QStringLiteral("notify → PollYouTubeChat"));
			PollYouTubeChat();
		} else if (!connectionsActive && DockIsOpen()) {
			DebugYouTube(QStringLiteral("notify → StartConnections"));
			StartConnections();
		} else {
			DebugYouTube(QStringLiteral("notify: chat id stored (poll deferred)"));
		}
		if (!youtubeDiscoverTimer->isActive() && connectionsActive)
			youtubeDiscoverTimer->start();
		return;
	}
	PersistYouTubeIds();
	if (!connectionsActive || !ytLiveChatId.isEmpty())
		return;
	if (!youtubeDiscoverTimer->isActive())
		youtubeDiscoverTimer->start();
	if (!ytVideoId.isEmpty()) {
		EnsureYouTubeWaitLiveTimer();
		ResolveYouTubeLiveChatId(ytVideoId);
	}
}

void ChatDock::ResolveYouTubeLiveChatId(const QString &videoId, std::function<void(bool)> done)
{
	if (videoId.isEmpty()) {
		if (done)
			done(false);
		return;
	}
	ytVideoId = videoId;
	if (!ytLiveChatId.isEmpty()) {
		if (!youtubePollInFlight && !youtubeTimer->isActive())
			PollYouTubeChat();
		if (done)
			done(true);
		return;
	}
	EnsureToken(YouTube, [this, videoId, done](bool ok) {
		if (!ok || !ytLiveChatId.isEmpty()) {
			if (done)
				done(!ytLiveChatId.isEmpty());
			return;
		}
		auto authHdr = [this] {
			return QList<QPair<QByteArray, QByteArray>>{
				{QByteArrayLiteral("Authorization"),
				 QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}};
		};
		auto finishOk = [this, done] {
			youtubeWaitingAnnounced = false;
			if (youtubeWaitLiveTimer)
				youtubeWaitLiveTimer->stop();
			PersistYouTubeIds();
			if (!youtubePollInFlight && !youtubeTimer->isActive())
				PollYouTubeChat();
			if (done)
				done(true);
		};
		auto tryVideos = [this, authHdr, videoId, finishOk, done] {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"));
			QUrlQuery query;
			query.addQueryItem("part", "liveStreamingDetails,snippet");
			query.addQueryItem("id", videoId);
			url.setQuery(query);
			Http(QString::fromUtf8(url.toEncoded()), authHdr(), {}, "GET",
			     [this, finishOk, done, videoId](int st, const QByteArray &payload, const QString &) {
				     if (!ytLiveChatId.isEmpty()) {
					     finishOk();
					     return;
				     }
				     if (st >= 200 && st < 300) {
					     const QJsonArray vids =
						     QJsonDocument::fromJson(payload).object().value("items").toArray();
					     if (!vids.isEmpty()) {
						     const QJsonObject details = vids.at(0)
											.toObject()
											.value("liveStreamingDetails")
											.toObject();
						     const QString chatId = YouTubeVideoActiveChatId(details);
						     if (!details.value(QStringLiteral("actualEndTime")).toString().isEmpty()) {
							     DebugYouTube(QStringLiteral("videos.list: live já terminou"));
							     if (ytVideoId == videoId) {
								     ytVideoId.clear();
								     PersistYouTubeIds();
							     }
							     if (done)
								     done(false);
							     return;
						     }
						     if (!chatId.isEmpty()) {
							     DebugYouTube(QStringLiteral("videos.list → chat id OK"));
							     BindYouTubeChat(chatId, videoId);
							     finishOk();
							     return;
						     }
						     DebugYouTube(QStringLiteral("videos.list: sem activeLiveChatId ainda"));
						     blog(LOG_INFO,
							  "[Stream Suite] YouTube video has no activeLiveChatId yet "
							  "(stream may still be starting)");
						     EnsureYouTubeWaitLiveTimer();
					     } else {
						     DebugYouTube(QStringLiteral("videos.list: items vazio"));
					     }
				     } else {
					     DebugYouTube(QStringLiteral("videos.list HTTP %1").arg(st));
					     blog(LOG_WARNING, "[Stream Suite] YouTube videos.list HTTP %d", st);
					     ReportYouTubeHttpError(st, payload);
				     }
				     if (done)
					     done(false);
			     });
		};
		/* liveBroadcasts.snippet.liveChatId is available for most scheduled/live events. */
		QUrl burl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery bq;
		bq.addQueryItem("part", "snippet,status");
		bq.addQueryItem("id", videoId);
		burl.setQuery(bq);
		Http(QString::fromUtf8(burl.toEncoded()), authHdr(), {}, "GET",
		     [this, videoId, tryVideos, finishOk, done](int code, const QByteArray &body, const QString &) {
			     if (!ytLiveChatId.isEmpty()) {
				     finishOk();
				     return;
			     }
			     if (code >= 200 && code < 300) {
				     const QJsonArray items =
					     QJsonDocument::fromJson(body).object().value("items").toArray();
				     if (!items.isEmpty()) {
					     const QJsonObject snip = items.at(0).toObject().value("snippet").toObject();
					     const QString chatId = YouTubeBroadcastChatId(snip);
					     const QString life = items.at(0)
									 .toObject()
									 .value("status")
									 .toObject()
									 .value("lifeCycleStatus")
									 .toString();
					     DebugYouTube(QStringLiteral("broadcast status=%1 chatField=%2")
								  .arg(life.isEmpty() ? QStringLiteral("?") : life,
								       chatId.isEmpty() ? QStringLiteral("empty")
											: QStringLiteral("ok")));
					     /* Ended streams keep a dead liveChatId — never poll it (404 loop). */
					     if (YouTubeBroadcastLifeDead(life)) {
						     DebugYouTube(QStringLiteral("broadcast %1 → discard video")
									  .arg(life));
						     if (ytVideoId == videoId) {
							     ytVideoId.clear();
							     PersistYouTubeIds();
						     }
						     if (done)
							     done(false);
						     return;
					     }
					     /* snippet.liveChatId exists from ready onward. Poll it; 404 just means
					      * chat is not open yet and we retry the same id. */
					     if (!chatId.isEmpty()) {
						     BindYouTubeChat(chatId, videoId);
						     finishOk();
						     return;
					     }
					     if (!videoId.isEmpty())
						     ytVideoId = videoId;
					     EnsureYouTubeWaitLiveTimer();
				     } else {
					     DebugYouTube(QStringLiteral("liveBroadcasts by id: items vazio"));
				     }
				     tryVideos();
				     return;
			     }
			     DebugYouTube(QStringLiteral("liveBroadcasts by id HTTP %1").arg(code));
			     ReportYouTubeHttpError(code, body);
			     if (YouTubeBodyLooksLikeQuota(body)) {
				     if (done)
					     done(false);
				     return;
			     }
			     tryVideos();
		     });
	});
}

void ChatDock::StartYouTubeChat()
{
	youtubeLastFullDiscoverMs = QDateTime::currentMSecsSinceEpoch();
	DebugYouTube(QStringLiteral("StartYouTubeChat video=%1")
			     .arg(ytVideoId.isEmpty() ? QStringLiteral("-") : ytVideoId.left(11)));
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok) {
			youtubeConnectStarted = false;
			DebugYouTube(QStringLiteral("token FAILED"));
			AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(YouTube)), YouTube);
			return;
		}
		if (!ytLiveChatId.isEmpty()) {
			DebugYouTube(QStringLiteral("já tenho chat id → poll"));
			youtubeWaitingAnnounced = false;
			if (!youtubePollInFlight && !youtubeTimer->isActive())
				PollYouTubeChat();
			return;
		}
		auto authHdr = [this] {
			return QList<QPair<QByteArray, QByteArray>>{
				{QByteArrayLiteral("Authorization"),
				 QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}};
		};
		auto markWaiting = [this] {
			DebugYouTube(QStringLiteral("discover esgotado → Waiting"));
			if (!youtubeWaitingAnnounced && !youtubeConnectedAnnounced) {
				youtubeWaitingAnnounced = true;
				AppendSystemMessage(Tr("ChatDockYouTubeWaiting"), YouTube);
			}
		};
		auto beginPoll = [this] {
			if (ytLiveChatId.isEmpty()) {
				EnsureYouTubeWaitLiveTimer();
				return;
			}
			if (youtubeWaitLiveTimer)
				youtubeWaitLiveTimer->stop();
			youtubeWaitingAnnounced = false;
			if (!youtubePollInFlight && !youtubeTimer->isActive())
				PollYouTubeChat();
		};
		/* Resolve liveChatId. Never trust hintChatId from complete broadcasts (404 loop). */
		auto resolveVideoChat = [this, beginPoll](const QString &videoId, const QString &hintChatId,
							  const QString &life, std::function<void()> next) {
			if (!ytLiveChatId.isEmpty()) {
				beginPoll();
				return;
			}
			if (YouTubeBroadcastLifeDead(life)) {
				DebugYouTube(QStringLiteral("skip %1 (%2)").arg(videoId.left(11), life));
				if (ytVideoId == videoId) {
					ytVideoId.clear();
					PersistYouTubeIds();
				}
				if (next)
					next();
				return;
			}
			if (!hintChatId.isEmpty()) {
				BindYouTubeChat(hintChatId, videoId);
				DebugYouTube(QStringLiteral("trust chat hint (%1)").arg(life.isEmpty() ? QStringLiteral("?") : life));
				beginPoll();
				return;
			}
			if (videoId.isEmpty()) {
				if (next)
					next();
				return;
			}
			/* Remember ready broadcasts so later discover/videos.list can find activeLiveChatId. */
			if (!videoId.isEmpty())
				ytVideoId = videoId;
			ResolveYouTubeLiveChatId(videoId, [next](bool ok) {
				if (!ok && next)
					next();
			});
		};
		auto pickBroadcastIds = [](const QJsonArray &items) {
			struct Cand {
				QString id;
				QString chatId;
				QString life;
				int score = 0;
			};
			QVector<Cand> out;
			for (const QJsonValue &row : items) {
				const QJsonObject item = row.toObject();
				Cand c;
				c.id = item.value("id").toString();
				c.life = item.value("status").toObject().value("lifeCycleStatus").toString();
				if (YouTubeBroadcastLifeDead(c.life) || c.id.isEmpty())
					continue;
				c.chatId = YouTubeBroadcastChatId(item.value("snippet").toObject());
				if (c.life == QLatin1String("live") || c.life == QLatin1String("liveStarting"))
					c.score = 50;
				else if (c.life == QLatin1String("testing") || c.life == QLatin1String("testStarting"))
					c.score = 40;
				else if (c.life == QLatin1String("ready"))
					c.score = 20;
				else
					c.score = 5;
				if (!c.chatId.isEmpty())
					c.score += 100;
				out.append(c);
			}
			std::sort(out.begin(), out.end(), [](const Cand &a, const Cand &b) { return a.score > b.score; });
			return out;
		};
		auto tryBroadcastList = [this, authHdr, resolveVideoChat, pickBroadcastIds](const QUrlQuery &extra,
											     std::function<void()> next) {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
			QUrlQuery query = extra;
			query.addQueryItem("part", "snippet,contentDetails,status");
			query.addQueryItem("broadcastType", "all");
			query.addQueryItem("maxResults", "25");
			url.setQuery(query);
			Http(QString::fromUtf8(url.toEncoded()), authHdr(), {}, "GET",
			     [this, resolveVideoChat, pickBroadcastIds, next](int code, const QByteArray &body, const QString &) {
				     if (!ytLiveChatId.isEmpty()) {
					     if (!youtubePollInFlight && !youtubeTimer->isActive())
						     PollYouTubeChat();
					     return;
				     }
				     if (code < 200 || code >= 300) {
					     DebugYouTube(QStringLiteral("liveBroadcasts list HTTP %1").arg(code));
					     blog(LOG_WARNING, "[Stream Suite] YouTube liveBroadcasts HTTP %d", code);
					     if (next)
						     next();
					     return;
				     }
				     const auto cands =
					     pickBroadcastIds(QJsonDocument::fromJson(body).object().value("items").toArray());
				     DebugYouTube(QStringLiteral("liveBroadcasts list → %1 candidatos")
							  .arg(cands.size()));
				     /* Try candidates in score order until one yields a liveChatId. */
				     auto tryAt = std::make_shared<std::function<void(int)>>();
				     *tryAt = [this, cands, resolveVideoChat, next, tryAt](int idx) {
					     if (idx >= cands.size()) {
						     if (next)
							     next();
						     return;
					     }
					     DebugYouTube(QStringLiteral("try broadcast[%1] id=%2 life=%3 chatHint=%4")
								  .arg(idx)
								  .arg(cands.at(idx).id.left(11), cands.at(idx).life,
								       cands.at(idx).chatId.isEmpty() ? QStringLiteral("no")
												      : QStringLiteral("yes")));
					     resolveVideoChat(cands.at(idx).id, cands.at(idx).chatId, cands.at(idx).life,
							      [tryAt, idx] { (*tryAt)(idx + 1); });
				     };
				     (*tryAt)(0);
			     });
		};
		auto afterChannel = [this, tryBroadcastList, markWaiting, resolveVideoChat] {
			if (!ytLiveChatId.isEmpty()) {
				if (!youtubePollInFlight && !youtubeTimer->isActive())
					PollYouTubeChat();
				return;
			}
			auto rest = [tryBroadcastList, markWaiting] {
				/* Active broadcast first, then the channel's own list. search.list is not used:
				 * it is capped near 100 calls/day and was returning HTTP 429. */
				QUrlQuery activeQ;
				activeQ.addQueryItem("broadcastStatus", "active");
				tryBroadcastList(activeQ, [tryBroadcastList, markWaiting] {
					QUrlQuery mineQ;
					mineQ.addQueryItem("mine", "true");
					mineQ.addQueryItem("maxResults", "50");
					tryBroadcastList(mineQ, markWaiting);
				});
			};
			if (!ytVideoId.isEmpty())
				resolveVideoChat(ytVideoId, QString(), QString(), rest);
			else
				rest();
		};

		/* Display name for messages we send. liveBroadcasts does not need the channel id. */
		if (accounts[YouTube].userId.isEmpty() || accounts[YouTube].userName.isEmpty()) {
			QUrl ch(QStringLiteral("https://www.googleapis.com/youtube/v3/channels"));
			QUrlQuery cq;
			cq.addQueryItem("part", "snippet");
			cq.addQueryItem("mine", "true");
			ch.setQuery(cq);
			Http(QString::fromUtf8(ch.toEncoded()), authHdr(), {}, "GET",
			     [this, afterChannel](int code, const QByteArray &payload, const QString &) {
				     if (code >= 200 && code < 300) {
					     const QJsonArray items =
						     QJsonDocument::fromJson(payload).object().value("items").toArray();
					     if (!items.isEmpty()) {
						     accounts[YouTube].userId = items.at(0).toObject().value("id").toString();
						     accounts[YouTube].userName = items.at(0)
											 .toObject()
											 .value("snippet")
											 .toObject()
											 .value("title")
											 .toString();
					     }
				     }
				     afterChannel();
			     });
			return;
		}
		afterChannel();
	});
}

void ChatDock::TryYouTubeGoLive()
{
	if (ytVideoId.isEmpty())
		return;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - youtubeLastTransitionMs < 12000)
		return;
	youtubeLastTransitionMs = now;
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok || ytVideoId.isEmpty())
			return;
		auto authHdr = [this] {
			return QList<QPair<QByteArray, QByteArray>>{
				{QByteArrayLiteral("Authorization"),
				 QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}};
		};
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "status,contentDetails");
		query.addQueryItem("id", ytVideoId);
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()), authHdr(), {}, "GET",
		     [this, authHdr](int code, const QByteArray &body, const QString &) {
			     if (code < 200 || code >= 300) {
				     DebugYouTube(QStringLiteral("go-live status HTTP %1").arg(code));
				     return;
			     }
			     const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
			     if (items.isEmpty())
				     return;
			     const QJsonObject item = items.at(0).toObject();
			     const QString life = item.value("status").toObject().value("lifeCycleStatus").toString();
			     DebugYouTube(QStringLiteral("go-live check status=%1").arg(life.isEmpty() ? QStringLiteral("?") : life));
			     if (life == QLatin1String("live") || life == QLatin1String("liveStarting")) {
				     if (!ytLiveChatId.isEmpty() && !youtubePollInFlight && !youtubeTimer->isActive())
					     PollYouTubeChat();
				     return;
			     }
			     if (YouTubeBroadcastLifeDead(life))
				     return;
			     const bool monitor = item.value("contentDetails")
							  .toObject()
							  .value("monitorStream")
							  .toObject()
							  .value("enableMonitorStream")
							  .toBool();
			     const QString target = (life == QLatin1String("ready") && monitor) ? QStringLiteral("testing")
												: QStringLiteral("live");
			     QUrl turl(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts/transition"));
			     QUrlQuery tq;
			     tq.addQueryItem("broadcastStatus", target);
			     tq.addQueryItem("id", ytVideoId);
			     tq.addQueryItem("part", "status");
			     turl.setQuery(tq);
			     DebugYouTube(QStringLiteral("transition → %1").arg(target));
			     auto headers = authHdr();
			     headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
			     Http(QString::fromUtf8(turl.toEncoded()), headers, QByteArray("{}"), "POST",
				  [this, target](int st, const QByteArray &payload, const QString &) {
					  const QString msg = QJsonDocument::fromJson(payload)
								      .object()
								      .value("error")
								      .toObject()
								      .value("message")
								      .toString();
					  if (st >= 200 && st < 300) {
						  DebugYouTube(QStringLiteral("transition %1 OK").arg(target));
						  if (!youtubePollInFlight && !ytLiveChatId.isEmpty())
							  QTimer::singleShot(2000, this, &ChatDock::PollYouTubeChat);
					  } else {
						  DebugYouTube(QStringLiteral("transition %1 HTTP %2: %3")
								       .arg(target)
								       .arg(st)
								       .arg(msg.isEmpty() ? QString::fromUtf8(payload.left(120))
											  : msg.left(140)));
					  }
				  });
		     });
	});
}

bool ChatDock::HandleYouTubeControlItem(const QJsonObject &obj)
{
	const QJsonObject snip = obj.value(QStringLiteral("snippet")).toObject();
	const QString type = snip.value(QStringLiteral("type")).toString();
	const bool deleted = type == QLatin1String("tombstone") || type == QLatin1String("messageDeletedEvent") ||
			     type == QLatin1String("messageRetractedEvent") ||
			     type.contains(QLatin1String("eleted"), Qt::CaseInsensitive) ||
			     type.contains(QLatin1String("etract"), Qt::CaseInsensitive);
	if (type == QLatin1String("userBannedEvent")) {
		const QJsonObject banned = snip.value(QStringLiteral("userBannedDetails"))
						 .toObject()
						 .value(QStringLiteral("bannedUserDetails"))
						 .toObject();
		const QString channelId = banned.value(QStringLiteral("channelId")).toString();
		const QString name = banned.value(QStringLiteral("displayName")).toString();
		const QString tip = Tr("ChatDockMessageRemovedUser").arg(name.isEmpty() ? channelId : name);
		if (!channelId.isEmpty())
			MarkMessagesDeletedByUser(YouTube, channelId, tip);
		if (!name.isEmpty())
			MarkMessagesDeletedByUser(YouTube, name, tip);
		return true;
	}
	if (!deleted)
		return false;
	QStringList ids;
	auto addId = [&ids](const QString &id) {
		const QString trimmed = id.trimmed();
		if (!trimmed.isEmpty() && !ids.contains(trimmed))
			ids.append(trimmed);
	};
	/* Tombstones reuse the original message id. Delete/retract events use a different id
	 * and point at the original via *MessageId. */
	if (type == QLatin1String("tombstone"))
		addId(obj.value(QStringLiteral("id")).toString());
	const QJsonValue delVal = snip.value(QStringLiteral("messageDeletedDetails"));
	if (delVal.isObject())
		addId(delVal.toObject().value(QStringLiteral("deletedMessageId")).toString());
	else if (delVal.isString())
		addId(delVal.toString());
	const QJsonValue retVal = snip.value(QStringLiteral("messageRetractedDetails"));
	if (retVal.isObject())
		addId(retVal.toObject().value(QStringLiteral("retractedMessageId")).toString());
	else if (retVal.isString())
		addId(retVal.toString());
	addId(snip.value(QStringLiteral("deletedMessageId")).toString());
	addId(snip.value(QStringLiteral("retractedMessageId")).toString());
	blog(LOG_INFO, "[Stream Suite] YouTube delete type=%s id=%s", type.toUtf8().constData(),
	     ids.join(QLatin1Char(',')).toUtf8().constData());
	for (const QString &id : ids)
		MarkMessageDeleted(id, Tr("ChatDockMessageRemoved"));
	return true;
}

void ChatDock::RefreshYouTubeDeletes()
{
	/* The live cursor only returns new lines. A deleted message stays on the page that
	 * first delivered it, rewritten as a tombstone. Re-read that page and do not append. */
	if (ytLiveChatId.isEmpty() || ytDeletePageTokens.isEmpty() || !connectionsActive)
		return;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (youtubeLastDeleteSyncMs > 0 && now - youtubeLastDeleteSyncMs < 8000)
		return;
	youtubeLastDeleteSyncMs = now;
	const QString token = ytDeletePageTokens.last();
	const QString chatId = ytLiveChatId;
	EnsureToken(YouTube, [this, token, chatId](bool ok) {
		if (!ok || ytLiveChatId != chatId)
			return;
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("liveChatId"), chatId);
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("200"));
		if (token != QLatin1String("*"))
			query.addQueryItem(QStringLiteral("pageToken"), token);
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}},
		     {}, "GET", [this, chatId](int status, const QByteArray &body, const QString &) {
			     if (ytLiveChatId != chatId || status < 200 || status >= 300)
				     return;
			     const QJsonArray items =
				     QJsonDocument::fromJson(body).object().value(QStringLiteral("items")).toArray();
			     for (const QJsonValue &row : items)
				     HandleYouTubeControlItem(row.toObject());
		     });
	});
}

static QString YouTubeCaptureNear(const QByteArray &html, const char *marker, const QRegularExpression &re)
{
	const int at = html.indexOf(marker);
	if (at < 0)
		return {};
	const auto match = re.match(QString::fromUtf8(html.mid(at, 280)));
	return match.hasMatch() ? match.captured(1) : QString();
}

static QString YouTubeContinuationNear(const QByteArray &html, const char *marker)
{
	const int at = html.indexOf(marker);
	if (at < 0)
		return {};
	const QString window = QString::fromUtf8(html.mid(at, 16000));
	const QRegularExpression re(QStringLiteral("\"continuation\"\\s*:\\s*\"([^\"]{20,})\""));
	const auto match = re.match(window);
	if (!match.hasMatch())
		return {};
	QString token = match.captured(1);
	token.replace(QStringLiteral("\\u003d"), QStringLiteral("="));
	token.replace(QStringLiteral("\\u0026"), QStringLiteral("&"));
	token.replace(QStringLiteral("\\/"), QStringLiteral("/"));
	return token;
}

static QString YouTubePageContinuation(const QByteArray &html, QString *clientVersion, QString *apiKey, QString *visitor)
{
	if (clientVersion) {
		*clientVersion = YouTubeCaptureNear(html, "INNERTUBE_CLIENT_VERSION",
						    QRegularExpression(QStringLiteral(":\\s*\"([0-9][0-9.]*)\"")));
	}
	if (apiKey) {
		*apiKey = YouTubeCaptureNear(html, "INNERTUBE_API_KEY",
					     QRegularExpression(QStringLiteral(":\\s*\"(AIza[^\"\\\\]+)\"")));
	}
	if (visitor) {
		*visitor = YouTubeCaptureNear(html, "VISITOR_DATA",
					      QRegularExpression(QStringLiteral(":\\s*\"([^\"\\\\]+)\"")));
	}
	const int liveAt = html.indexOf("liveChatRenderer");
	const QByteArray slice = liveAt >= 0 ? html.mid(liveAt, 60000) : html;
	QString token = YouTubeContinuationNear(slice, "invalidationContinuationData");
	if (token.isEmpty())
		token = YouTubeContinuationNear(slice, "timedContinuationData");
	if (token.isEmpty())
		token = YouTubeContinuationNear(slice, "reloadContinuationData");
	return token;
}

static QJsonObject YouTubeNextContinuation(const QJsonArray &continuations, int *timeoutMs)
{
	const QStringList keys = {QStringLiteral("invalidationContinuationData"), QStringLiteral("timedContinuationData"),
				  QStringLiteral("reloadContinuationData")};
	for (const QString &key : keys) {
		for (const QJsonValue &value : continuations) {
			const QJsonObject data = value.toObject().value(key).toObject();
			if (data.value(QStringLiteral("continuation")).toString().isEmpty())
				continue;
			if (timeoutMs)
				*timeoutMs = data.value(QStringLiteral("timeoutMs")).toInt(1000);
			return data;
		}
	}
	return {};
}

static QString YouTubeRichText(const QJsonObject &node)
{
	const QString simple = node.value(QStringLiteral("simpleText")).toString();
	if (!simple.isEmpty())
		return simple;
	QString text;
	const QJsonArray runs = node.value(QStringLiteral("runs")).toArray();
	for (const QJsonValue &run : runs)
		text += run.toObject().value(QStringLiteral("text")).toString();
	return text;
}

static QString YouTubeThumbUrl(const QJsonObject &image)
{
	const QJsonArray thumbs = image.value(QStringLiteral("thumbnails")).toArray();
	auto pick = [&](bool allowSvg) {
		QString best;
		int bestDist = 100000;
		for (const QJsonValue &value : thumbs) {
			const QJsonObject thumb = value.toObject();
			QString url = thumb.value(QStringLiteral("url")).toString();
			if (url.startsWith(QLatin1String("//")))
				url.prepend(QLatin1String("https:"));
			if (url.isEmpty())
				continue;
			if (!allowSvg && url.contains(QLatin1String(".svg"), Qt::CaseInsensitive))
				continue;
			const int width = thumb.value(QStringLiteral("width")).toInt(64);
			const int dist = qAbs(width - 64);
			if (best.isEmpty() || dist < bestDist) {
				best = url;
				bestDist = dist;
			}
		}
		return best;
	};
	const QString raster = pick(false);
	return raster.isEmpty() ? pick(true) : raster;
}

void ChatDock::IngestYouTubeLiveAction(const QJsonObject &action)
{
	if (action.contains(QStringLiteral("replayChatItemAction"))) {
		const QJsonArray nested = action.value(QStringLiteral("replayChatItemAction"))
						  .toObject()
						  .value(QStringLiteral("actions"))
						  .toArray();
		for (const QJsonValue &value : nested)
			IngestYouTubeLiveAction(value.toObject());
		return;
	}
	if (action.contains(QStringLiteral("markChatItemsByAuthorAsDeletedAction"))) {
		const QString channel = action.value(QStringLiteral("markChatItemsByAuthorAsDeletedAction"))
						.toObject()
						.value(QStringLiteral("externalChannelId"))
						.toString();
		if (!channel.isEmpty()) {
			blog(LOG_INFO, "[Stream Suite] YouTube delete author=%s", channel.toUtf8().constData());
			MarkMessagesDeletedByUser(YouTube, channel, Tr("ChatDockMessageRemoved"));
		}
		return;
	}
	if (action.contains(QStringLiteral("replaceChatItemAction"))) {
		const QJsonObject replaced = action.value(QStringLiteral("replaceChatItemAction")).toObject();
		const QString id = replaced.value(QStringLiteral("targetItemId")).toString();
		const QJsonObject item = replaced.value(QStringLiteral("replacementItem")).toObject();
		QString text = YouTubeRichText(item.value(QStringLiteral("liveChatTextMessageRenderer"))
						       .toObject()
						       .value(QStringLiteral("message"))
						       .toObject());
		if (text.isEmpty()) {
			text = YouTubeRichText(item.value(QStringLiteral("liveChatPaidMessageRenderer"))
						       .toObject()
						       .value(QStringLiteral("message"))
						       .toObject());
		}
		const QString low = text.toLower();
		if (!id.isEmpty() && (low.contains(QLatin1String("delet")) || low.contains(QLatin1String("remov")) ||
				      low.contains(QLatin1String("elimin")) || low.contains(QLatin1String("retract")))) {
			blog(LOG_INFO, "[Stream Suite] YouTube delete type=replace id=%s", id.left(48).toUtf8().constData());
			MarkMessageDeleted(id, Tr("ChatDockMessageRemoved"));
		}
		return;
	}
	const QStringList keys = action.keys();
	for (const QString &key : keys) {
		if (!key.contains(QLatin1String("eleted")) && key != QLatin1String("removeChatItemAction"))
			continue;
		const QString id = action.value(key).toObject().value(QStringLiteral("targetItemId")).toString();
		if (id.isEmpty())
			continue;
		blog(LOG_INFO, "[Stream Suite] YouTube delete type=%s id=%s", key.toUtf8().constData(),
		     id.left(48).toUtf8().constData());
		MarkMessageDeleted(id, Tr("ChatDockMessageRemoved"));
		return;
	}

	const QJsonObject item = action.value(QStringLiteral("addChatItemAction")).toObject().value(QStringLiteral("item")).toObject();
	if (item.isEmpty())
		return;
	QJsonObject renderer = item.value(QStringLiteral("liveChatTextMessageRenderer")).toObject();
	if (renderer.isEmpty())
		renderer = item.value(QStringLiteral("liveChatPaidMessageRenderer")).toObject();
	if (renderer.isEmpty())
		return;

	qint64 usec = 0;
	const QJsonValue stamp = renderer.value(QStringLiteral("timestampUsec"));
	if (stamp.isString())
		usec = stamp.toString().toLongLong();
	else
		usec = static_cast<qint64>(stamp.toDouble());
	qint64 published = usec;
	if (usec > 100000000000000LL)
		published = usec / 1000;
	if (published > 0 && youtubeIgnoreBeforeMs > 0 && published < youtubeIgnoreBeforeMs)
		return;

	ChatMessage msg;
	msg.platform = YouTube;
	msg.id = renderer.value(QStringLiteral("id")).toString();
	if (msg.id.isEmpty())
		return;
	msg.user = YouTubeRichText(renderer.value(QStringLiteral("authorName")).toObject());
	if (msg.user.isEmpty())
		msg.user = QStringLiteral("YouTube");
	msg.color = QStringLiteral("#FF5555");
	msg.ts = published > 0 ? published : QDateTime::currentMSecsSinceEpoch();
	msg.userLogin = renderer.value(QStringLiteral("authorExternalChannelId")).toString();

	auto appendText = [&](const QString &text) {
		if (text.isEmpty())
			return;
		msg.text += text;
		if (!msg.runs.isEmpty() && msg.runs.last().imageUrl.isEmpty()) {
			msg.runs.last().text += text;
			return;
		}
		ChatRun run;
		run.text = text;
		msg.runs.append(run);
	};
	const QString amount = YouTubeRichText(renderer.value(QStringLiteral("purchaseAmountText")).toObject());
	if (!amount.isEmpty())
		appendText(amount + QStringLiteral(" "));
	const QJsonArray runs = renderer.value(QStringLiteral("message")).toObject().value(QStringLiteral("runs")).toArray();
	for (const QJsonValue &value : runs) {
		const QJsonObject run = value.toObject();
		if (run.contains(QStringLiteral("text"))) {
			appendText(run.value(QStringLiteral("text")).toString());
			continue;
		}
		const QJsonObject emoji = run.value(QStringLiteral("emoji")).toObject();
		if (emoji.isEmpty())
			continue;
		QString alt = emoji.value(QStringLiteral("emojiId")).toString();
		const bool custom = emoji.value(QStringLiteral("isCustomEmoji")).toBool() ||
				    alt.startsWith(QLatin1String("UC")) || alt.size() > 12;
		if (custom) {
			const QJsonArray shortcuts = emoji.value(QStringLiteral("shortcuts")).toArray();
			alt = shortcuts.isEmpty() ? QString() : shortcuts.at(0).toString();
		}
		const QString url = YouTubeThumbUrl(emoji.value(QStringLiteral("image")).toObject());
		if (url.isEmpty()) {
			appendText(alt);
			continue;
		}
		static bool loggedEmoji = false;
		if (!loggedEmoji) {
			loggedEmoji = true;
			blog(LOG_INFO, "[Stream Suite] YouTube emoji image %s", url.left(140).toUtf8().constData());
		}
		msg.text += alt;
		ChatRun image;
		image.text = alt;
		image.imageUrl = url;
		msg.runs.append(image);
	}
	if (msg.text.isEmpty() && msg.runs.isEmpty())
		return;

	const QJsonArray badges = renderer.value(QStringLiteral("authorBadges")).toArray();
	for (const QJsonValue &value : badges) {
		const QJsonObject badge = value.toObject().value(QStringLiteral("liveChatAuthorBadgeRenderer")).toObject();
		const QString icon = badge.value(QStringLiteral("icon")).toObject().value(QStringLiteral("iconType")).toString();
		const QString tip = badge.value(QStringLiteral("tooltip")).toString();
		if (icon == QLatin1String("OWNER"))
			msg.badges.append({QStringLiteral("owner"), QStringLiteral("yt-role:owner"),
					   tip.isEmpty() ? QStringLiteral("Owner") : tip});
		else if (icon == QLatin1String("MODERATOR"))
			msg.badges.append({QStringLiteral("mod"), QStringLiteral("yt-role:moderator"),
					   tip.isEmpty() ? QStringLiteral("Moderator") : tip});
		else if (icon == QLatin1String("VERIFIED"))
			msg.badges.append({QStringLiteral("verified"), QStringLiteral("yt-role:verified"),
					   tip.isEmpty() ? QStringLiteral("Verified") : tip});
		else {
			const QString custom = YouTubeThumbUrl(badge.value(QStringLiteral("customThumbnail")).toObject());
			if (!custom.isEmpty())
				msg.badges.append({QStringLiteral("member"), custom, tip.isEmpty() ? QStringLiteral("Member") : tip});
			else if (icon == QLatin1String("MEMBER") || tip.contains(QLatin1String("member"), Qt::CaseInsensitive))
				msg.badges.append({QStringLiteral("member"), QStringLiteral("yt-role:member"),
						   tip.isEmpty() ? QStringLiteral("Member") : tip});
		}
	}
	if (!AdoptYouTubeMessage(msg))
		AppendMessage(msg);
}

void ChatDock::PollYouTubeInnertube()
{
	if (ytVideoId.isEmpty() || youtubePollInFlight)
		return;
	youtubePollInFlight = true;
	const QString videoId = ytVideoId;
	const QByteArray userAgent = QByteArrayLiteral(
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
		"Chrome/131.0.0.0 Safari/537.36");
	const QByteArray cookie = QByteArrayLiteral("SOCS=CAI; CONSENT=YES+cb");

	auto schedule = [this](int ms) {
		if (connectionsActive && !ytVideoId.isEmpty() && youtubeTimer)
			youtubeTimer->start(qBound(250, ms, 4000));
	};

	std::function<void(const QString &)> post;
	post = [this, videoId, userAgent, cookie, schedule](const QString &continuation) {
		QString version = ytInnertubeClientVersion;
		if (version.isEmpty())
			version = QStringLiteral("2.20261030.01.00");
		QString key = ytInnertubeApiKey;
		if (key.isEmpty())
			key = QStringLiteral("AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8");
		QJsonObject client;
		client.insert(QStringLiteral("clientName"), QStringLiteral("WEB"));
		client.insert(QStringLiteral("clientVersion"), version);
		client.insert(QStringLiteral("hl"), QStringLiteral("en"));
		client.insert(QStringLiteral("gl"), QStringLiteral("US"));
		if (!ytInnertubeVisitor.isEmpty())
			client.insert(QStringLiteral("visitorData"), ytInnertubeVisitor);
		QJsonObject context;
		context.insert(QStringLiteral("client"), client);
		QJsonObject payload;
		payload.insert(QStringLiteral("context"), context);
		payload.insert(QStringLiteral("continuation"), continuation);
		const QString url = QStringLiteral("https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?prettyPrint=false&key=") +
				    key;
		const QByteArray referer =
			QByteArrayLiteral("https://www.youtube.com/live_chat?is_popout=1&v=") + videoId.toUtf8();
		const qint64 started = QDateTime::currentMSecsSinceEpoch();
		Http(url,
		     {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
		      {QByteArrayLiteral("User-Agent"), userAgent},
		      {QByteArrayLiteral("Origin"), QByteArrayLiteral("https://www.youtube.com")},
		      {QByteArrayLiteral("Referer"), referer},
		      {QByteArrayLiteral("Cookie"), cookie},
		      {QByteArrayLiteral("X-YouTube-Client-Name"), QByteArrayLiteral("1")},
		      {QByteArrayLiteral("X-YouTube-Client-Version"), version.toUtf8()}},
		     QJsonDocument(payload).toJson(QJsonDocument::Compact), "POST",
		     [this, videoId, started, schedule](int status, const QByteArray &body, const QString &error) {
			     youtubePollInFlight = false;
			     if (videoId != ytVideoId)
				     return;
			     if (status < 200 || status >= 300) {
				     ytContinuation.clear();
				     DebugYouTube(QStringLiteral("innertube HTTP %1 (%2 bytes)")
							  .arg(status)
							  .arg(body.size()));
				     blog(LOG_WARNING, "[Stream Suite] YouTube site chat HTTP %d: %s", status,
					  error.isEmpty() ? body.left(180).constData() : error.toUtf8().constData());
				     schedule(4000);
				     return;
			     }
			     const QJsonObject live = QJsonDocument::fromJson(body)
							       .object()
							       .value(QStringLiteral("continuationContents"))
							       .toObject()
							       .value(QStringLiteral("liveChatContinuation"))
							       .toObject();
			     if (live.isEmpty()) {
				     ytContinuation.clear();
				     DebugYouTube(QStringLiteral("innertube sem liveChatContinuation (%1 bytes) %2")
							  .arg(body.size())
							  .arg(QString::fromUtf8(body.left(140))));
				     schedule(4000);
				     return;
			     }
			     int timeoutMs = 1000;
			     const QString token = YouTubeNextContinuation(live.value(QStringLiteral("continuations")).toArray(),
									    &timeoutMs)
							    .value(QStringLiteral("continuation"))
							    .toString();
			     if (!token.isEmpty())
				     ytContinuation = token;
			     else
				     ytContinuation.clear();
			     if (!youtubeConnectedAnnounced) {
				     youtubeConnectedAnnounced = true;
				     youtubeWaitingAnnounced = false;
				     const int actions = live.value(QStringLiteral("actions")).toArray().size();
				     blog(LOG_INFO, "[Stream Suite] YouTube site chat HTTP 200 (%d actions)", actions);
				     DebugYouTube(QStringLiteral("innertube HTTP 200 → Connected"));
				     AppendSystemMessage(Tr("ChatDockConnected").arg(PlatformLabel(YouTube)), YouTube);
			     }
			     if (youtubeIgnoreBeforeMs <= 0)
				     youtubeIgnoreBeforeMs = QDateTime::currentMSecsSinceEpoch() - 15000;
			     const QJsonArray actions = live.value(QStringLiteral("actions")).toArray();
			     for (const QJsonValue &value : actions)
				     IngestYouTubeLiveAction(value.toObject());
			     const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - started;
			     /* Invalidation requests stay open until a new line arrives. Do not wait again. */
			     const int wait = elapsed >= 1500 ? 250 : qBound(800, timeoutMs > 0 ? timeoutMs : 1000, 3000);
			     if (ytContinuation.isEmpty())
				     schedule(3000);
			     else
				     schedule(wait);
		     });
	};

	if (!ytContinuation.isEmpty() && !ytInnertubeClientVersion.isEmpty()) {
		post(ytContinuation);
		return;
	}

	const QString page = QStringLiteral("https://www.youtube.com/live_chat?is_popout=1&v=") + videoId;
	Http(page,
	     {{QByteArrayLiteral("User-Agent"), userAgent},
	      {QByteArrayLiteral("Accept-Language"), QByteArrayLiteral("en-US,en;q=0.9")},
	      {QByteArrayLiteral("Cookie"), cookie}},
	     {}, "GET", [this, videoId, post, schedule](int status, const QByteArray &body, const QString &) {
		     if (videoId != ytVideoId) {
			     youtubePollInFlight = false;
			     return;
		     }
		     QString version;
		     QString key;
		     QString visitor;
		     const QString token = YouTubePageContinuation(body, &version, &key, &visitor);
		     if (!version.isEmpty())
			     ytInnertubeClientVersion = version;
		     if (!key.isEmpty())
			     ytInnertubeApiKey = key;
		     if (!visitor.isEmpty())
			     ytInnertubeVisitor = visitor;
		     if (ytInnertubeClientVersion.isEmpty())
			     ytInnertubeClientVersion = QStringLiteral("2.20261030.01.00");
		     if (token.isEmpty()) {
			     youtubePollInFlight = false;
			     DebugYouTube(QStringLiteral("live_chat sem continuation HTTP %1 (%2 bytes)")
						  .arg(status)
						  .arg(body.size()));
			     blog(LOG_WARNING, "[Stream Suite] YouTube live_chat page HTTP %d, no continuation (%d bytes)",
				  status, int(body.size()));
			     TryYouTubeGoLive();
			     schedule(5000);
			     return;
		     }
		     ytContinuation = token;
		     post(token);
	     });
}

void ChatDock::PollYouTubeChat()
{
	if (youtubePollInFlight)
		return;
	/* liveChat/messages has neither emoji images nor a delete event.
	 * The site feed (get_live_chat) is what the YouTube chat window reads. */
	if (!ytVideoId.isEmpty()) {
		PollYouTubeInnertube();
		return;
	}
	if (ytLiveChatId.isEmpty())
		return;
	youtubePollInFlight = true;
	if (!youtubeConnectedAnnounced)
		DebugYouTube(QStringLiteral("poll liveChatMessages…"));
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok || ytLiveChatId.isEmpty()) {
			youtubePollInFlight = false;
			if (!ok) {
				DebugYouTube(QStringLiteral("poll: token FAILED"));
				blog(LOG_WARNING, "[Stream Suite] YouTube chat poll: token failed");
				if (connectionsActive && !ytLiveChatId.isEmpty())
					youtubeTimer->start(8000);
			}
			return;
		}
		const Account &acc = accounts[YouTube];
		/* Discovery path is liveChat/messages — liveChatMessages 404s with an empty body. */
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem("liveChatId", ytLiveChatId);
		query.addQueryItem("part", "snippet,authorDetails");
		/* Big pages while skipping history, then small live pages. */
		query.addQueryItem("maxResults", youtubeCatchingUp ? QStringLiteral("2000") : QStringLiteral("200"));
		if (!ytNextPage.isEmpty())
			query.addQueryItem("pageToken", ytNextPage);
		url.setQuery(query);
		const QString requestUrl = QString::fromUtf8(url.toEncoded());
		const QString usedToken = ytNextPage;
		const QString polledChat = ytLiveChatId;
		Http(requestUrl,
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {}, "GET",
		     [this, usedToken, polledChat](int status, const QByteArray &body, const QString &error) {
			     youtubePollInFlight = false;
			     if (polledChat != ytLiveChatId) {
				     if (connectionsActive && !ytLiveChatId.isEmpty())
					     QTimer::singleShot(0, this, &ChatDock::PollYouTubeChat);
				     return;
			     }
			     int interval = 5000;
			     if (status >= 200 && status < 300) {
				     youtubePageTokenFails = 0;
				     const QJsonObject root = QJsonDocument::fromJson(body).object();
				     if (root.contains(QStringLiteral("offlineAt"))) {
					     SetViewerCount(YouTube, 0, false);
					     ytLiveChatId.clear();
					     ytNextPage.clear();
					     youtubeConnectedAnnounced = false;
					     youtubeCatchingUp = true;
					     ClearYouTubeMessages();
					     PersistYouTubeIds();
					     DebugYouTube(QStringLiteral("poll: offlineAt → chat fechou"));
					     blog(LOG_INFO, "[Stream Suite] YouTube live chat went offline");
					     if (connectionsActive && !youtubeDiscoverTimer->isActive())
						     youtubeDiscoverTimer->start();
					     if (!youtubeWaitingAnnounced) {
						     youtubeWaitingAnnounced = true;
						     AppendSystemMessage(Tr("ChatDockYouTubeWaiting"), YouTube);
					     }
					     return;
				     }
				     const QString next = root.value("nextPageToken").toString();
				     if (!next.isEmpty())
					     ytNextPage = next;
				     /* Honor YouTube's interval (often 5–10s). Floor at 5s to protect quota (5 units/call). */
				     interval = qMax(5000, root.value("pollingIntervalMillis").toInt(10000));
				     /* Only announce Connected once per session after a successful poll. */
				     if (!youtubeConnectedAnnounced) {
					     youtubeConnectedAnnounced = true;
					     youtubeWaitingAnnounced = false;
					     DebugYouTube(QStringLiteral("poll HTTP 200 → Connected"));
					     AppendSystemMessage(Tr("ChatDockConnected").arg(PlatformLabel(YouTube)), YouTube);
				     }
				     if (youtubeIgnoreBeforeMs <= 0)
					     youtubeIgnoreBeforeMs = QDateTime::currentMSecsSinceEpoch() - 15000;
				     const QJsonArray items = root.value("items").toArray();
				     qint64 newestPub = 0;
				     bool keptMessage = false;
				     for (const QJsonValue &row : items) {
					     const QJsonObject obj = row.toObject();
					     if (HandleYouTubeControlItem(obj))
						     continue;
					     const QJsonObject snip = obj.value("snippet").toObject();
					     const qint64 published = YouTubePublishedMs(snip);
					     if (published > newestPub)
						     newestPub = published;
					     /* Same liveChatId keeps the previous stream's lines. Drop them. */
					     if (published > 0 && published < youtubeIgnoreBeforeMs)
						     continue;
					     const QString type = snip.value("type").toString();
					     /* Prefer typed text, then displayMessage (covers Super Chat / stickers / etc.). */
					     QString text =
						     snip.value("textMessageDetails").toObject().value("messageText").toString();
					     if (text.isEmpty())
						     text = snip.value("superChatDetails")
								    .toObject()
								    .value("userComment")
								    .toString();
					     if (text.isEmpty())
						     text = snip.value("displayMessage").toString();
					     if (text.isEmpty())
						     continue;
					     /* Skip pure system/fan-funding noise with no useful chat line when type is known-bad. */
					     if (type == QLatin1String("fanFundingEvent") ||
						 type == QLatin1String("pollEvent") ||
						 type == QLatin1String("chatEndedEvent") ||
						 type == QLatin1String("sponsorOnlyModeStartedEvent") ||
						 type == QLatin1String("sponsorOnlyModeEndedEvent") ||
						 type == QLatin1String("userBannedEvent"))
						     continue;
					     const QJsonObject author = obj.value("authorDetails").toObject();
					     ChatMessage msg;
					     msg.platform = YouTube;
					     msg.id = obj.value("id").toString();
					     msg.user = author.value("displayName").toString();
					     if (msg.user.isEmpty())
						     msg.user = QStringLiteral("YouTube");
					     msg.color = QStringLiteral("#FF5555");
					     msg.text = text;
					     msg.ts = QDateTime::currentMSecsSinceEpoch();
					     msg.userLogin = author.value("channelId").toString();
					     if (msg.userLogin.isEmpty())
						     msg.userLogin = snip.value("authorChannelId").toString();
					     if (author.value("isChatOwner").toBool())
						     msg.badges.append({QStringLiteral("owner"), QStringLiteral("yt-role:owner"),
									QStringLiteral("Owner")});
					     if (author.value("isChatModerator").toBool())
						     msg.badges.append({QStringLiteral("mod"), QStringLiteral("yt-role:moderator"),
									QStringLiteral("Moderator")});
					     if (author.value("isChatSponsor").toBool())
						     msg.badges.append({QStringLiteral("member"), QStringLiteral("yt-role:member"),
									QStringLiteral("Member")});
					     if (author.value("isVerified").toBool())
						     msg.badges.append({QStringLiteral("verified"),
									QStringLiteral("yt-role:verified"),
									QStringLiteral("Verified")});
					     /* Reuse the local echo so a later tombstone hits the same row.
					      * Insert id and list id are often different. */
					     if (!AdoptYouTubeMessage(msg))
						     AppendMessage(msg);
					     keptMessage = true;
				     }
				     if (keptMessage) {
					     const QString pageKey = usedToken.isEmpty() ? QStringLiteral("*") : usedToken;
					     ytDeletePageTokens.removeAll(pageKey);
					     ytDeletePageTokens.append(pageKey);
					     while (ytDeletePageTokens.size() > 2)
						     ytDeletePageTokens.removeFirst();
				     }
				     RefreshYouTubeDeletes();
				     /* A full old page means we are still walking the previous live.
				      * Skip ahead quickly, then sit on YouTube's own poll interval. */
				     const bool moreHistory = youtubeCatchingUp && !next.isEmpty() && next != usedToken &&
							      newestPub > 0 && newestPub + 1500 < youtubeIgnoreBeforeMs &&
							      youtubeCatchupPages < 40;
				     if (moreHistory) {
					     ++youtubeCatchupPages;
					     interval = 1000;
				     } else {
					     youtubeCatchingUp = false;
				     }
			     } else {
				     const QString errMsg = QJsonDocument::fromJson(body)
								   .object()
								   .value("error")
								   .toObject()
								   .value("message")
								   .toString();
				     DebugYouTube(QStringLiteral("poll HTTP %1 (%2 bytes): %3")
							  .arg(status)
							  .arg(body.size())
							  .arg(errMsg.isEmpty() ? QString::fromUtf8(body.left(80))
										: errMsg.left(100)));
				     blog(LOG_WARNING, "[Stream Suite] YouTube chat poll HTTP %d: %s", status,
					  error.isEmpty() ? body.left(200).constData() : error.toUtf8().constData());
				     if (status == 401) {
					     ReportYouTubeHttpError(status, body);
					     youtubeConnectStarted = false;
					     return;
				     }
				     if (status == 403) {
					     ReportYouTubeHttpError(status, body);
					     if (YouTubeBodyLooksLikeQuota(body))
						     return;
					     /* Invalid/forbidden chat id — drop and rediscover instead of stalling forever. */
					     ytLiveChatId.clear();
					     ytNextPage.clear();
					     youtubeConnectedAnnounced = false;
					     youtubeConnectStarted = false;
					     if (connectionsActive)
						     QTimer::singleShot(3000, this, &ChatDock::StartYouTubeChat);
					     return;
				     }
				     if (status == 400) {
					     /* Stale/single-use pageToken — drop and keep polling; do not rediscover. */
					     if (!ytNextPage.isEmpty()) {
						     ytNextPage.clear();
						     youtubePageTokenFails++;
					     } else {
						     /* Bad liveChatId often returns 400 — rediscover. */
						     ytLiveChatId.clear();
						     youtubeConnectedAnnounced = false;
						     youtubeConnectStarted = false;
						     if (connectionsActive)
							     QTimer::singleShot(2000, this, &ChatDock::StartYouTubeChat);
						     return;
					     }
					     interval = youtubePageTokenFails > 3 ? 10000 : 3000;
				     } else if (status == 404) {
					     /* Chat stays closed while the broadcast is still "ready". Custom RTMP
					      * outputs do not call liveBroadcasts.transition, so do that ourselves. */
					     ytNextPage.clear();
					     youtubePageTokenFails++;
					     DebugYouTube(QStringLiteral("poll 404 #%1 — retry same chat id")
								  .arg(youtubePageTokenFails));
					     if (!youtubeWaitingAnnounced && !youtubeConnectedAnnounced) {
						     youtubeWaitingAnnounced = true;
						     AppendSystemMessage(Tr("ChatDockYouTubeWaiting"), YouTube);
					     }
					     if (youtubePageTokenFails >= 1)
						     TryYouTubeGoLive();
					     if (connectionsActive && !ytLiveChatId.isEmpty())
						     youtubeTimer->start(8000);
					     return;
				     } else {
					     interval = 8000;
				     }
			     }
			     if (connectionsActive && !ytLiveChatId.isEmpty())
				     youtubeTimer->start(interval);
		     });
	});
}

static bool ItemHasChatId(const QListWidgetItem *item, const QString &want);
static void AddChatIdAlias(QListWidgetItem *item, const QString &id);

void ChatDock::SendYouTubeMessage(const QString &text)
{
	if (ytLiveChatId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockSendFailed").arg(PlatformLabel(YouTube), Tr("ChatDockYouTubeWaiting")), YouTube);
		return;
	}
	/* Echo immediately (like Twitch); poll may also deliver the server copy with a real id. */
	ChatMessage local;
	local.platform = YouTube;
	local.user = accounts[YouTube].userName.isEmpty() ? QStringLiteral("You") : accounts[YouTube].userName;
	local.color = QStringLiteral("#FF5555");
	local.text = text;
	local.ts = QDateTime::currentMSecsSinceEpoch();
	local.id = QStringLiteral("local-yt-%1").arg(local.ts);
	local.userLogin = accounts[YouTube].userId;
	local.badges.append({QStringLiteral("owner"), QStringLiteral("yt-role:owner"), QStringLiteral("Owner")});
	AppendMessage(local);

	EnsureToken(YouTube, [this, text, localId = local.id](bool ok) {
		if (!ok || ytLiveChatId.isEmpty()) {
			AppendSystemMessage(Tr("ChatDockSendFailed").arg(PlatformLabel(YouTube), QStringLiteral("auth")), YouTube);
			return;
		}
		const Account &acc = accounts[YouTube];
		QJsonObject snip;
		snip.insert("liveChatId", ytLiveChatId);
		snip.insert("type", "textMessageEvent");
		QJsonObject details;
		details.insert("messageText", text);
		snip.insert("textMessageDetails", details);
		QJsonObject body;
		body.insert("snippet", snip);
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery q;
		q.addQueryItem("part", "snippet");
		url.setQuery(q);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
		     [this, localId](int status, const QByteArray &resp, const QString &error) {
			     if (status >= 200 && status < 300) {
				     /* Prefer the real YouTube message id so later tombstones match. */
				     const QString realId = QJsonDocument::fromJson(resp).object().value("id").toString();
				     if (!realId.isEmpty() && messageList) {
					     for (int i = 0; i < messageList->count(); ++i) {
						     QListWidgetItem *item = messageList->item(i);
						     if (item && ItemHasChatId(item, localId)) {
							     AddChatIdAlias(item, realId);
							     seenMessageIds.insert(realId);
							     if (ytPendingDeletes.contains(realId))
								     MarkMessageDeleted(realId, Tr("ChatDockMessageRemoved"));
							     break;
						     }
					     }
				     }
				     return;
			     }
			     blog(LOG_WARNING, "[Stream Suite] YouTube send HTTP %d: %s", status,
				  error.isEmpty() ? resp.left(200).constData() : error.toUtf8().constData());
			     QString reason = QString::number(status);
			     const QJsonObject err = QJsonDocument::fromJson(resp).object().value("error").toObject();
			     if (!err.isEmpty())
				     reason = err.value("message").toString(reason);
			     AppendSystemMessage(Tr("ChatDockSendFailed").arg(PlatformLabel(YouTube), reason), YouTube);
		     });
	});
}

static QString YouTubeApiErrorText(const QByteArray &body, int status)
{
	const QJsonObject err = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toObject();
	const QJsonArray errors = err.value(QStringLiteral("errors")).toArray();
	if (!errors.isEmpty()) {
		const QString reason = errors.at(0).toObject().value(QStringLiteral("reason")).toString();
		if (!reason.isEmpty())
			return reason;
	}
	const QString message = err.value(QStringLiteral("message")).toString();
	return message.isEmpty() ? QString::number(status) : message;
}

static QStringList YouTubeDeleteIds(const QListWidgetItem *item)
{
	QStringList ids;
	auto add = [&](const QString &id) {
		if (id.isEmpty() || id.startsWith(QLatin1String("local-yt-")) || ids.contains(id))
			return;
		ids.append(id);
	};
	if (!item)
		return ids;
	add(item->data(Qt::UserRole + 1).toString());
	for (const QString &alias : item->data(Qt::UserRole + 6).toStringList())
		add(alias);
	/* Only ids minted by liveChat/messages start with LCC. The site chat id does not. */
	QStringList apiIds;
	for (const QString &id : ids) {
		if (id.startsWith(QLatin1String("LCC.")))
			apiIds.append(id);
	}
	return apiIds;
}

static QString NormYouTubeText(QString text)
{
	text.remove(QChar(0xFE0F));
	return text.trimmed();
}

static QString YouTubeListedText(const QJsonObject &snippet)
{
	QString text = snippet.value(QStringLiteral("textMessageDetails")).toObject().value(QStringLiteral("messageText")).toString();
	if (text.isEmpty())
		text = snippet.value(QStringLiteral("superChatDetails")).toObject().value(QStringLiteral("userComment")).toString();
	if (text.isEmpty())
		text = snippet.value(QStringLiteral("displayMessage")).toString();
	return text;
}

void ChatDock::ShowYouTubeMessageMenu(QListWidgetItem *item, QWidget *anchor)
{
	if (!item || !messageList)
		return;
	const QString channelId = item->data(Qt::UserRole + 8).toString();
	QString name = item->data(Qt::UserRole + 9).toString();
	if (name.isEmpty())
		name = channelId;
	const QString mine = accounts[YouTube].userId;
	const bool canModerate = !channelId.isEmpty() && channelId.compare(mine, Qt::CaseInsensitive) != 0;
	const bool alreadyDeleted = item->data(Qt::UserRole + 2).toBool();
	const QStringList deleteIds = YouTubeDeleteIds(item);
	const QString uiId = item->data(Qt::UserRole + 1).toString();
	const QString text = item->data(Qt::UserRole + 5).toString();
	const qint64 publishedMs = item->data(Qt::UserRole + 10).toLongLong();

	QMenu menu(this);
	QAction *removeAction = nullptr;
	if (!alreadyDeleted) {
		removeAction = menu.addAction(Tr("ChatDockDeleteMessage"));
	}
	QAction *banAction = nullptr;
	QAction *unbanAction = nullptr;
	if (canModerate) {
		banAction = menu.addAction(Tr("ChatDockBanUser"));
		unbanAction = menu.addAction(Tr("ChatDockUnbanUser"));
	}
	if (menu.actions().isEmpty())
		return;
	const QPoint pos = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos();
	QAction *chosen = menu.exec(pos);
	if (!chosen)
		return;
	if (chosen == removeAction) {
		if (!deleteIds.isEmpty())
			DeleteYouTubeMessage(deleteIds, 0, uiId, channelId, text, publishedMs);
		else
			FindYouTubeMessageToDelete(uiId, channelId, text, publishedMs, QString(), 6);
		return;
	}
	if (chosen == banAction) {
		if (QMessageBox::question(this, Tr("ChatDockBanUser"), Tr("ChatDockBanConfirm").arg(name)) != QMessageBox::Yes)
			return;
		BanYouTubeUser(channelId, name);
		return;
	}
	if (chosen == unbanAction) {
		if (QMessageBox::question(this, Tr("ChatDockUnbanUser"), Tr("ChatDockUnbanConfirm").arg(name)) !=
		    QMessageBox::Yes)
			return;
		UnbanYouTubeUser(channelId, name);
	}
}

void ChatDock::ShowMessageActions(QListWidgetItem *item, QWidget *anchor)
{
	if (!item)
		return;
	const auto platform = static_cast<Platform>(item->data(Qt::UserRole + 4).toInt());
	if (platform == YouTube) {
		ShowYouTubeMessageMenu(item, anchor);
		return;
	}

	const QString actorId = item->data(Qt::UserRole + 8).toString();
	const QString login = item->data(Qt::UserRole + 3).toString();
	QString name = item->data(Qt::UserRole + 9).toString();
	if (name.isEmpty())
		name = login.isEmpty() ? actorId : login;
	const QString messageId = item->data(Qt::UserRole + 1).toString();
	const bool alreadyDeleted = item->data(Qt::UserRole + 2).toBool();
	const bool ownTwitch = platform == Twitch && !login.isEmpty() &&
			       login.compare(twitchLogin, Qt::CaseInsensitive) == 0;
	const bool ownKick = platform == Kick && !actorId.isEmpty() && actorId == accounts[Kick].userId;
	const bool canModerate = platform == Twitch ? (!login.isEmpty() && !ownTwitch) : (!actorId.isEmpty() && !ownKick);

	QMenu menu(this);
	QAction *removeAction = nullptr;
	if (!alreadyDeleted)
		removeAction = menu.addAction(Tr("ChatDockDeleteMessage"));
	QAction *timeoutAction = nullptr;
	QAction *banAction = nullptr;
	QAction *unbanAction = nullptr;
	if (canModerate) {
		timeoutAction = menu.addAction(Tr("ChatDockTimeout"));
		banAction = menu.addAction(Tr("ChatDockBanUser"));
		unbanAction = menu.addAction(Tr("ChatDockUnbanUser"));
	}
	if (menu.actions().isEmpty())
		return;
	const QPoint pos = anchor ? anchor->mapToGlobal(QPoint(0, anchor->height())) : QCursor::pos();
	QAction *chosen = menu.exec(pos);
	if (!chosen)
		return;

	if (chosen == removeAction) {
		if (platform == Twitch) {
			if (messageId.startsWith(QLatin1String("local-tw-"))) {
				if (!twitchPendingDeletes.contains(messageId)) {
					twitchPendingDeletes.insert(messageId);
					AppendSystemMessage(Tr("ChatDockDeletePending").arg(PlatformLabel(Twitch)), Twitch);
				}
				return;
			}
			if (messageId.startsWith(QLatin1String("tw-"))) {
				AppendSystemMessage(Tr("ChatDockDeleteNoId"), Twitch);
				return;
			}
			DeleteTwitchMessage(messageId);
			return;
		}
		if (messageId.startsWith(QLatin1String("local-kick-"))) {
			AppendSystemMessage(Tr("ChatDockDeleteNoId"), Kick);
			return;
		}
		DeleteKickMessage(messageId, messageId);
		return;
	}
	if (chosen == timeoutAction) {
		if (platform == Twitch) {
			ModerateTwitchUser(actorId, login, name, 600);
		} else {
			ModerateKickUser(actorId, name, login, 10);
		}
		return;
	}
	if (chosen == banAction) {
		if (QMessageBox::question(this, Tr("ChatDockBanUser"), Tr("ChatDockBanConfirm").arg(name)) != QMessageBox::Yes)
			return;
		if (platform == Twitch) {
			ModerateTwitchUser(actorId, login, name, 0);
		} else {
			ModerateKickUser(actorId, name, login, 0);
		}
		return;
	}
	if (chosen == unbanAction) {
		if (QMessageBox::question(this, Tr("ChatDockUnbanUser"), Tr("ChatDockUnbanConfirm").arg(name)) !=
		    QMessageBox::Yes)
			return;
		if (platform == Twitch) {
			ModerateTwitchUser(actorId, login, name, -1);
		} else {
			ModerateKickUser(actorId, name, login, -1);
		}
	}
}

extern StreamInfoDock *stream_info_dock;

static QString TwitchApiError(const QByteArray &body, int status)
{
	const QString message = QJsonDocument::fromJson(body).object().value(QStringLiteral("message")).toString();
	return message.isEmpty() ? QString::number(status) : message;
}

void ChatDock::AskModerationReconnect(Platform platform)
{
	AppendSystemMessage(Tr("ChatDockModReconnect").arg(PlatformLabel(platform)), platform);
	if (stream_info_dock)
		stream_info_dock->RequestModerationAuth(int(platform));
}

void ChatDock::DeleteTwitchMessage(const QString &messageId)
{
	const QString userId = accounts[Twitch].userId;
	if (messageId.isEmpty() || userId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), QStringLiteral("id")), Twitch);
		return;
	}
	EnsureToken(Twitch, [this, messageId, userId](bool ok) {
		if (!ok) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), QStringLiteral("auth")), Twitch);
			return;
		}
		QUrl url(QStringLiteral("https://api.twitch.tv/helix/moderation/chat"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("broadcaster_id"), userId);
		query.addQueryItem(QStringLiteral("moderator_id"), userId);
		query.addQueryItem(QStringLiteral("message_id"), messageId);
		url.setQuery(query);
		const Account &acc = accounts[Twitch];
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
		      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}},
		     {}, "DELETE", [this, messageId](int status, const QByteArray &body, const QString &error) {
			     if (status == 204 || (status >= 200 && status < 300)) {
				     blog(LOG_INFO, "[Stream Suite] Twitch message deleted");
				     MarkMessageDeleted(messageId, Tr("ChatDockMessageRemoved"));
				     return;
			     }
			     if (status == 401 || status == 403) {
				     blog(LOG_WARNING, "[Stream Suite] Twitch delete HTTP %d: %s", status, body.left(180).constData());
				     AskModerationReconnect(Twitch);
				     return;
			     }
			     const QString reason = error.isEmpty() ? TwitchApiError(body, status) : error;
			     blog(LOG_WARNING, "[Stream Suite] Twitch delete HTTP %d: %s", status, reason.toUtf8().constData());
			     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), reason), Twitch);
		     });
	});
}

void ChatDock::ModerateTwitchUser(const QString &userId, const QString &login, const QString &displayName,
				  int timeoutSeconds)
{
	const bool numericId = !userId.isEmpty() &&
			       std::all_of(userId.cbegin(), userId.cend(), [](QChar ch) { return ch.isDigit(); });
	if (!numericId) {
		if (login.isEmpty()) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), QStringLiteral("id")), Twitch);
			return;
		}
		EnsureToken(Twitch, [this, login, displayName, timeoutSeconds](bool ok) {
			if (!ok) {
				AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), QStringLiteral("auth")),
						    Twitch);
				return;
			}
			QUrl url(QStringLiteral("https://api.twitch.tv/helix/users"));
			QUrlQuery query;
			query.addQueryItem(QStringLiteral("login"), login);
			url.setQuery(query);
			const Account &acc = accounts[Twitch];
			Http(QString::fromUtf8(url.toEncoded()),
			     {{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
			      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}},
			     {}, "GET", [this, login, displayName, timeoutSeconds](int status, const QByteArray &body, const QString &) {
				     const QJsonArray data = QJsonDocument::fromJson(body).object().value(QStringLiteral("data")).toArray();
				     if (status < 200 || status >= 300 || data.isEmpty()) {
					     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), QString::number(status)),
								 Twitch);
					     return;
				     }
				     const QString id = data.at(0).toObject().value(QStringLiteral("id")).toString();
				     ModerateTwitchUser(id, login, displayName, timeoutSeconds);
			     });
		});
		return;
	}

	const QString moderatorId = accounts[Twitch].userId;
	if (moderatorId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), QStringLiteral("id")), Twitch);
		return;
	}
	const bool unban = timeoutSeconds < 0;
	const QString actionLabel = unban ? Tr("ChatDockUnbanUser")
					  : (timeoutSeconds > 0 ? Tr("ChatDockTimeout") : Tr("ChatDockBanUser"));
	EnsureToken(Twitch, [this, userId, login, displayName, timeoutSeconds, moderatorId, unban, actionLabel](bool ok) {
		if (!ok) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(actionLabel, QStringLiteral("auth")), Twitch);
			return;
		}
		QUrl url(QStringLiteral("https://api.twitch.tv/helix/moderation/bans"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("broadcaster_id"), moderatorId);
		query.addQueryItem(QStringLiteral("moderator_id"), moderatorId);
		QByteArray payload;
		const char *verb = "POST";
		if (unban) {
			query.addQueryItem(QStringLiteral("user_id"), userId);
			verb = "DELETE";
		} else {
			QJsonObject data;
			data.insert(QStringLiteral("user_id"), userId);
			if (timeoutSeconds > 0)
				data.insert(QStringLiteral("duration"), timeoutSeconds);
			data.insert(QStringLiteral("reason"), timeoutSeconds > 0 ? QStringLiteral("Timeout") : QStringLiteral("Ban"));
			QJsonObject body;
			body.insert(QStringLiteral("data"), data);
			payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
		}
		url.setQuery(query);
		const Account &acc = accounts[Twitch];
		QList<QPair<QByteArray, QByteArray>> headers = {
			{QByteArrayLiteral("Client-Id"), acc.clientId.toUtf8()},
			{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}};
		if (!payload.isEmpty())
			headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
		Http(QString::fromUtf8(url.toEncoded()), headers, payload, verb,
		     [this, login, displayName, timeoutSeconds, unban, actionLabel](int status, const QByteArray &body,
										    const QString &error) {
			     if (status == 401 || status == 403) {
				     blog(LOG_WARNING, "[Stream Suite] Twitch moderation HTTP %d: %s", status, body.left(180).constData());
				     AskModerationReconnect(Twitch);
				     return;
			     }
			     if (status < 200 || status >= 300) {
				     const QString reason = error.isEmpty() ? TwitchApiError(body, status) : error;
				     blog(LOG_WARNING, "[Stream Suite] Twitch moderation HTTP %d: %s", status,
					  reason.toUtf8().constData());
				     AppendSystemMessage(Tr("ChatDockActionFailed").arg(actionLabel, reason), Twitch);
				     return;
			     }
			     blog(LOG_INFO, "[Stream Suite] Twitch moderation ok");
			     if (unban) {
				     AppendSystemMessage(Tr("ChatDockUserUnbanned").arg(displayName), Twitch);
				     return;
			     }
			     const QString tip = timeoutSeconds > 0 ? Tr("ChatDockUserTimedOut").arg(displayName)
								    : Tr("ChatDockMessageRemovedUser").arg(displayName);
			     MarkMessagesDeletedByUser(Twitch, login, tip);
			     AppendSystemMessage(timeoutSeconds > 0 ? Tr("ChatDockUserTimedOut").arg(displayName)
								    : Tr("ChatDockUserBanned").arg(displayName),
						 Twitch);
		     });
	});
}

void ChatDock::DeleteKickMessage(const QString &messageId, const QString &uiId)
{
	if (messageId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockDeleteNoId"), Kick);
		return;
	}
	EnsureToken(Kick, [this, messageId, uiId](bool ok) {
		if (!ok) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), QStringLiteral("auth")),
					    Kick);
			return;
		}
		const QString url = QStringLiteral("https://api.kick.com/public/v1/chat/") +
				    QString::fromUtf8(QUrl::toPercentEncoding(messageId));
		Http(url,
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[Kick].accessToken.toUtf8()},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     {}, "DELETE", [this, uiId](int status, const QByteArray &body, const QString &error) {
			     if (status == 204 || (status >= 200 && status < 300)) {
				     blog(LOG_INFO, "[Stream Suite] Kick message deleted");
				     MarkMessageDeleted(uiId, Tr("ChatDockMessageRemoved"));
				     return;
			     }
			     if (status == 401 || status == 403) {
				     blog(LOG_WARNING, "[Stream Suite] Kick delete HTTP %d: %s", status, body.left(180).constData());
				     AskModerationReconnect(Kick);
				     return;
			     }
			     const QString reason = error.isEmpty()
							    ? QJsonDocument::fromJson(body).object().value(QStringLiteral("message")).toString()
							    : error;
			     blog(LOG_WARNING, "[Stream Suite] Kick delete HTTP %d: %s", status,
				  reason.isEmpty() ? body.left(160).constData() : reason.toUtf8().constData());
			     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"),
										reason.isEmpty() ? QString::number(status) : reason),
						 Kick);
		     });
	});
}

void ChatDock::ModerateKickUser(const QString &userId, const QString &displayName, const QString &matchLogin,
				int timeoutMinutes)
{
	const QString broadcaster = kickChannelId.isEmpty() ? accounts[Kick].userId : kickChannelId;
	if (userId.isEmpty() || broadcaster.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), QStringLiteral("id")), Kick);
		return;
	}
	const bool unban = timeoutMinutes < 0;
	const QString actionLabel = unban ? Tr("ChatDockUnbanUser")
					  : (timeoutMinutes > 0 ? Tr("ChatDockTimeout") : Tr("ChatDockBanUser"));
	EnsureToken(Kick, [this, userId, displayName, matchLogin, timeoutMinutes, broadcaster, unban, actionLabel](bool ok) {
		if (!ok) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(actionLabel, QStringLiteral("auth")), Kick);
			return;
		}
		QJsonObject payload;
		payload.insert(QStringLiteral("broadcaster_user_id"), broadcaster.toLongLong());
		payload.insert(QStringLiteral("user_id"), userId.toLongLong());
		if (!unban && timeoutMinutes > 0) {
			payload.insert(QStringLiteral("duration"), timeoutMinutes);
			payload.insert(QStringLiteral("reason"), QStringLiteral("Timeout"));
		} else if (!unban) {
			payload.insert(QStringLiteral("reason"), QStringLiteral("Ban"));
		}
		Http(QStringLiteral("https://api.kick.com/public/v1/moderation/bans"),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[Kick].accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(payload).toJson(QJsonDocument::Compact), unban ? "DELETE" : "POST",
		     [this, userId, displayName, matchLogin, timeoutMinutes, unban, actionLabel](int status, const QByteArray &body,
										      const QString &error) {
			     if (status == 401 || status == 403) {
				     blog(LOG_WARNING, "[Stream Suite] Kick moderation HTTP %d: %s", status, body.left(180).constData());
				     AskModerationReconnect(Kick);
				     return;
			     }
			     if (status < 200 || status >= 300) {
				     const QString reason =
					     error.isEmpty() ? QJsonDocument::fromJson(body).object().value(QStringLiteral("message")).toString()
							     : error;
				     blog(LOG_WARNING, "[Stream Suite] Kick moderation HTTP %d: %s", status,
					  reason.isEmpty() ? body.left(160).constData() : reason.toUtf8().constData());
				     AppendSystemMessage(Tr("ChatDockActionFailed").arg(actionLabel, reason.isEmpty() ? QString::number(status)
												       : reason),
							 Kick);
				     return;
			     }
			     blog(LOG_INFO, "[Stream Suite] Kick moderation ok for %s", userId.toUtf8().constData());
			     if (unban) {
				     AppendSystemMessage(Tr("ChatDockUserUnbanned").arg(displayName), Kick);
				     return;
			     }
			     const QString tip = timeoutMinutes > 0 ? Tr("ChatDockUserTimedOut").arg(displayName)
								    : Tr("ChatDockMessageRemovedUser").arg(displayName);
			     if (!matchLogin.isEmpty())
				     MarkMessagesDeletedByUser(Kick, matchLogin, tip);
			     MarkMessagesDeletedByUser(Kick, userId, tip);
			     AppendSystemMessage(timeoutMinutes > 0 ? Tr("ChatDockUserTimedOut").arg(displayName)
								    : Tr("ChatDockUserBanned").arg(displayName),
						 Kick);
		     });
	});
}

void ChatDock::DeleteYouTubeMessage(const QStringList &ids, int index, const QString &uiId, const QString &channelId,
				    const QString &text, qint64 publishedMs)
{
	if (index < 0 || index >= ids.size()) {
		if (!channelId.isEmpty() || !text.isEmpty())
			FindYouTubeMessageToDelete(uiId, channelId, text, publishedMs, QString(), 6);
		else
			AppendSystemMessage(Tr("ChatDockDeleteNotFound"), YouTube);
		return;
	}
	const QString id = ids.at(index);
	EnsureToken(YouTube, [this, ids, index, id, uiId, channelId, text, publishedMs](bool ok) {
		if (!ok) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), QStringLiteral("auth")),
					    YouTube);
			return;
		}
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("id"), id);
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}},
		     {}, "DELETE",
		     [this, ids, index, id, uiId, channelId, text, publishedMs](int status, const QByteArray &body,
										 const QString &error) {
			     if (status == 204 || (status >= 200 && status < 300)) {
				     blog(LOG_INFO, "[Stream Suite] YouTube message deleted");
				     MarkMessageDeleted(uiId.isEmpty() ? id : uiId, Tr("ChatDockMessageRemoved"));
				     return;
			     }
			     if (status == 404 && index + 1 < ids.size()) {
				     DeleteYouTubeMessage(ids, index + 1, uiId, channelId, text, publishedMs);
				     return;
			     }
			     /* The site chat id is not a liveChatMessage id. Look the real one up. */
			     if (status == 404 && (!channelId.isEmpty() || !text.isEmpty())) {
				     FindYouTubeMessageToDelete(uiId, channelId, text, publishedMs, QString(), 6);
				     return;
			     }
			     const QString reason = error.isEmpty() ? YouTubeApiErrorText(body, status) : error;
			     blog(LOG_WARNING, "[Stream Suite] YouTube delete HTTP %d: %s", status,
				  reason.toUtf8().constData());
			     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), reason), YouTube);
		     });
	});
}

void ChatDock::FindYouTubeMessageToDelete(const QString &uiId, const QString &channelId, const QString &text,
					  qint64 publishedMs, const QString &pageToken, int pagesLeft)
{
	if (ytLiveChatId.isEmpty() || pagesLeft <= 0) {
		AppendSystemMessage(Tr("ChatDockDeleteNotFound"), YouTube);
		return;
	}
	EnsureToken(YouTube, [this, uiId, channelId, text, publishedMs, pageToken, pagesLeft](bool ok) {
		if (!ok || ytLiveChatId.isEmpty()) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), QStringLiteral("auth")),
					    YouTube);
			return;
		}
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/messages"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("liveChatId"), ytLiveChatId);
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet,authorDetails"));
		query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("2000"));
		if (!pageToken.isEmpty())
			query.addQueryItem(QStringLiteral("pageToken"), pageToken);
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}},
		     {}, "GET",
		     [this, uiId, channelId, text, publishedMs, pageToken, pagesLeft](int status, const QByteArray &body,
										      const QString &error) {
			     if (status < 200 || status >= 300) {
				     const QString reason = error.isEmpty() ? YouTubeApiErrorText(body, status) : error;
				     blog(LOG_WARNING, "[Stream Suite] YouTube delete lookup HTTP %d: %s", status,
					  reason.toUtf8().constData());
				     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockDeleteMessage"), reason),
							 YouTube);
				     return;
			     }
			     const QJsonObject root = QJsonDocument::fromJson(body).object();
			     const QJsonArray items = root.value(QStringLiteral("items")).toArray();
			     const QString wantText = NormYouTubeText(text);
			     QString bestId;
			     qint64 bestDelta = 0x7fffffffffffffffLL;
			     qint64 newest = 0;
			     for (const QJsonValue &value : items) {
				     const QJsonObject obj = value.toObject();
				     const QJsonObject snippet = obj.value(QStringLiteral("snippet")).toObject();
				     const qint64 published = YouTubePublishedMs(snippet);
				     if (published > newest)
					     newest = published;
				     const QString author = obj.value(QStringLiteral("authorDetails"))
								    .toObject()
								    .value(QStringLiteral("channelId"))
								    .toString();
				     const QString authorSnip = snippet.value(QStringLiteral("authorChannelId")).toString();
				     const QString who = author.isEmpty() ? authorSnip : author;
				     if (!channelId.isEmpty() && who.compare(channelId, Qt::CaseInsensitive) != 0)
					     continue;
				     if (!wantText.isEmpty() && NormYouTubeText(YouTubeListedText(snippet)) != wantText)
					     continue;
				     const qint64 delta = (publishedMs > 0 && published > 0) ? qAbs(published - publishedMs) : 0;
				     if (publishedMs > 0 && published > 0 && delta > 20000)
					     continue;
				     if (bestId.isEmpty() || delta < bestDelta) {
					     bestId = obj.value(QStringLiteral("id")).toString();
					     bestDelta = delta;
				     }
			     }
			     if (!bestId.isEmpty()) {
				     blog(LOG_INFO, "[Stream Suite] YouTube delete matched API id (%d chars)", bestId.size());
				     DeleteYouTubeMessage(QStringList{bestId}, 0, uiId, QString(), QString(), 0);
				     return;
			     }
			     const QString next = root.value(QStringLiteral("nextPageToken")).toString();
			     const bool olderPage = publishedMs > 0 && newest > 0 && newest + 2000 < publishedMs;
			     if (!next.isEmpty() && next != pageToken && pagesLeft > 1 && (publishedMs <= 0 || olderPage)) {
				     FindYouTubeMessageToDelete(uiId, channelId, text, publishedMs, next, pagesLeft - 1);
				     return;
			     }
			     blog(LOG_INFO, "[Stream Suite] YouTube delete lookup missed (%d items)", items.size());
			     AppendSystemMessage(Tr("ChatDockDeleteNotFound"), YouTube);
		     });
	});
}

void ChatDock::BanYouTubeUser(const QString &channelId, const QString &displayName)
{
	if (channelId.isEmpty() || ytLiveChatId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), Tr("ChatDockYouTubeWaiting")), YouTube);
		return;
	}
	EnsureToken(YouTube, [this, channelId, displayName](bool ok) {
		if (!ok || ytLiveChatId.isEmpty()) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), QStringLiteral("auth")), YouTube);
			return;
		}
		QJsonObject banned;
		banned.insert(QStringLiteral("channelId"), channelId);
		QJsonObject snippet;
		snippet.insert(QStringLiteral("liveChatId"), ytLiveChatId);
		snippet.insert(QStringLiteral("type"), QStringLiteral("permanent"));
		snippet.insert(QStringLiteral("bannedUserDetails"), banned);
		QJsonObject payload;
		payload.insert(QStringLiteral("snippet"), snippet);
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/bans"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(payload).toJson(QJsonDocument::Compact), "POST",
		     [this, channelId, displayName](int status, const QByteArray &body, const QString &error) {
			     if (status < 200 || status >= 300) {
				     const QString reason = error.isEmpty() ? YouTubeApiErrorText(body, status) : error;
				     blog(LOG_WARNING, "[Stream Suite] YouTube ban HTTP %d: %s", status,
					  reason.toUtf8().constData());
				     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockBanUser"), reason), YouTube);
				     return;
			     }
			     const QString banId = QJsonDocument::fromJson(body).object().value(QStringLiteral("id")).toString();
			     if (!banId.isEmpty())
				     ytBanIds.insert(channelId, banId);
			     blog(LOG_INFO, "[Stream Suite] YouTube banned %s", channelId.toUtf8().constData());
			     const QString tip = Tr("ChatDockMessageRemovedUser").arg(displayName);
			     MarkMessagesDeletedByUser(YouTube, channelId, tip);
			     AppendSystemMessage(Tr("ChatDockUserBanned").arg(displayName), YouTube);
		     });
	});
}

void ChatDock::UnbanYouTubeUser(const QString &channelId, const QString &displayName)
{
	if (channelId.isEmpty())
		return;
	const QString known = ytBanIds.value(channelId);
	if (!known.isEmpty()) {
		RemoveYouTubeBan(known, channelId, displayName, true);
		return;
	}
	if (ytLiveChatId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockUnbanUser"), Tr("ChatDockYouTubeWaiting")),
				    YouTube);
		return;
	}
	EnsureToken(YouTube, [this, channelId, displayName](bool ok) {
		if (!ok || ytLiveChatId.isEmpty()) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockUnbanUser"), QStringLiteral("auth")),
					    YouTube);
			return;
		}
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/bans"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("snippet"));
		query.addQueryItem(QStringLiteral("liveChatId"), ytLiveChatId);
		query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}},
		     {}, "GET", [this, channelId, displayName](int status, const QByteArray &body, const QString &error) {
			     if (status < 200 || status >= 300) {
				     const QString reason = error.isEmpty() ? YouTubeApiErrorText(body, status) : error;
				     blog(LOG_WARNING, "[Stream Suite] YouTube ban list HTTP %d: %s", status,
					  reason.toUtf8().constData());
				     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockUnbanUser"), reason),
							 YouTube);
				     return;
			     }
			     const QJsonArray items = QJsonDocument::fromJson(body).object().value(QStringLiteral("items")).toArray();
			     for (const QJsonValue &value : items) {
				     const QJsonObject ban = value.toObject();
				     const QString bannedChannel = ban.value(QStringLiteral("snippet"))
									   .toObject()
									   .value(QStringLiteral("bannedUserDetails"))
									   .toObject()
									   .value(QStringLiteral("channelId"))
									   .toString();
				     if (bannedChannel.compare(channelId, Qt::CaseInsensitive) != 0)
					     continue;
				     const QString banId = ban.value(QStringLiteral("id")).toString();
				     if (!banId.isEmpty()) {
					     ytBanIds.insert(channelId, banId);
					     RemoveYouTubeBan(banId, channelId, displayName, false);
					     return;
				     }
			     }
			     AppendSystemMessage(Tr("ChatDockBanNotFound").arg(displayName), YouTube);
		     });
	});
}

void ChatDock::RemoveYouTubeBan(const QString &banId, const QString &channelId, const QString &displayName,
				bool listIfMissing)
{
	if (banId.isEmpty())
		return;
	EnsureToken(YouTube, [this, banId, channelId, displayName, listIfMissing](bool ok) {
		if (!ok) {
			AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockUnbanUser"), QStringLiteral("auth")),
					    YouTube);
			return;
		}
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChat/bans"));
		QUrlQuery query;
		query.addQueryItem(QStringLiteral("id"), banId);
		url.setQuery(query);
		Http(QString::fromUtf8(url.toEncoded()),
		     {{QByteArrayLiteral("Authorization"),
		       QByteArrayLiteral("Bearer ") + accounts[YouTube].accessToken.toUtf8()}},
		     {}, "DELETE",
		     [this, banId, channelId, displayName, listIfMissing](int status, const QByteArray &body, const QString &error) {
			     if (status == 204 || (status >= 200 && status < 300)) {
				     ytBanIds.remove(channelId);
				     blog(LOG_INFO, "[Stream Suite] YouTube unbanned %s", channelId.toUtf8().constData());
				     AppendSystemMessage(Tr("ChatDockUserUnbanned").arg(displayName), YouTube);
				     return;
			     }
			     if (status == 404 && listIfMissing) {
				     ytBanIds.remove(channelId);
				     UnbanYouTubeUser(channelId, displayName);
				     return;
			     }
			     const QString reason = error.isEmpty() ? YouTubeApiErrorText(body, status) : error;
			     blog(LOG_WARNING, "[Stream Suite] YouTube unban HTTP %d: %s", status, reason.toUtf8().constData());
			     AppendSystemMessage(Tr("ChatDockActionFailed").arg(Tr("ChatDockUnbanUser"), reason), YouTube);
		     });
	});
}

void ChatDock::ResolveKickChatroom()
{
	EnsureToken(Kick, [this](bool ok) {
		if (!ok) {
			kickConnectStarted = false;
			AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(Kick)), Kick);
			return;
		}
		const Account &acc = accounts[Kick];
		Http(QStringLiteral("https://api.kick.com/public/v1/channels"),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300) {
				     AppendSystemMessage(Tr("ChatDockKickResolveFailed"), Kick);
				     return;
			     }
			     const QJsonObject channel = FirstDataObject(body);
			     kickSlug = channel.value("slug").toString();
			     if (kickSlug.isEmpty())
				     kickSlug = accounts[Kick].userName;
			     if (accounts[Kick].userId.isEmpty())
				     accounts[Kick].userId = JsonId(channel.value("broadcaster_user_id").isUndefined()
									     ? channel.value("user_id")
									     : channel.value("broadcaster_user_id"));
			     kickChatroomId = JsonId(channel.value("chatroom_id"));
			     if (kickChatroomId.isEmpty())
				     kickChatroomId = JsonId(channel.value("chatroom").toObject().value("id"));
			     kickChannelId = JsonId(channel.value("broadcaster_user_id").isUndefined()
							    ? channel.value("user_id")
							    : channel.value("broadcaster_user_id"));
			     if (kickSlug.isEmpty()) {
				     AppendSystemMessage(Tr("ChatDockKickResolveFailed"), Kick);
				     return;
			     }
			     const QList<QPair<QByteArray, QByteArray>> browserHeaders = {
				     {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")},
				     {QByteArrayLiteral("Accept-Language"), QByteArrayLiteral("en-US,en;q=0.9")},
				     {QByteArrayLiteral("Origin"), QByteArrayLiteral("https://kick.com")},
				     {QByteArrayLiteral("Referer"), QByteArrayLiteral("https://kick.com/")},
				     {QByteArrayLiteral("User-Agent"),
				      QByteArrayLiteral(
					      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
					      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")},
			     };
			     Http(QStringLiteral("https://kick.com/api/v2/channels/") +
					  QString::fromUtf8(QUrl::toPercentEncoding(kickSlug)),
				  browserHeaders, {}, "GET", [this](int code, const QByteArray &payload, const QString &) {
					  if (code >= 200 && code < 300) {
						  const QJsonObject root = QJsonDocument::fromJson(payload).object();
						  const QString channelId = JsonId(root.value("id"));
						  if (!channelId.isEmpty())
							  kickChannelId = channelId;
						  const QString roomId = JsonId(root.value("chatroom").toObject().value("id"));
						  if (!roomId.isEmpty())
							  kickChatroomId = roomId;
						  if (kickChatroomId.isEmpty())
							  kickChatroomId = channelId;
						  IngestKickSubscriberBadges(root.value(QStringLiteral("subscriber_badges")).toArray());
					  }
					  if (kickChatroomId.isEmpty() && kickChannelId.isEmpty()) {
						  AppendSystemMessage(Tr("ChatDockKickResolveFailed"), Kick);
						  if (!kickReconnectTimer->isActive())
							  kickReconnectTimer->start(15000);
						  return;
					  }
					  ConnectKickChat();
				  });
		     });
	});
}

void ChatDock::StartKickCentrifugo()
{
	if (!connectionsActive)
		return;
	if (kickChatroomId.isEmpty() && kickChannelId.isEmpty()) {
		AppendSystemMessage(Tr("ChatDockKickResolveFailed"), Kick);
		return;
	}

	kickRealtimeClientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	const QString clientId = kickRealtimeClientId;
	const QJsonObject body{
		{QStringLiteral("client"),
		 QJsonObject{{QStringLiteral("id"), clientId}, {QStringLiteral("type"), QStringLiteral("web")}}},
		{QStringLiteral("capabilities"),
		 QJsonObject{{QStringLiteral("accepted_providers"),
			      QJsonArray{QJsonObject{{QStringLiteral("provider"), QStringLiteral("centrifugo")}}}}}},
	};
	const QList<QPair<QByteArray, QByteArray>> headers = {
		{QByteArrayLiteral("Origin"), QByteArrayLiteral("https://kick.com")},
		{QByteArrayLiteral("Referer"), QByteArrayLiteral("https://kick.com/")},
		{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
		{QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")},
		{QByteArrayLiteral("User-Agent"),
		 QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
				   "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")},
	};
	const QString channelKey = !kickChannelId.isEmpty() ? kickChannelId : kickChatroomId;
	const QString connUrl =
		QStringLiteral("https://web.kick.com/api/v1/realtime/channels/%1/chat/connection").arg(channelKey);

	auto fail = [this](const char *why) {
		blog(LOG_WARNING, "[Stream Suite] Kick Centrifugo setup failed: %s", why);
		AppendSystemMessage(Tr("ChatDockKickConnectFailed"), Kick);
		kickConnectStarted = false;
		if (!kickReconnectTimer->isActive())
			kickReconnectTimer->start(12000);
	};

	Http(connUrl, headers, QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
	     [this, headers, clientId, body, fail](int status, const QByteArray &resp, const QString &) {
		     auto parseConnections = [](const QByteArray &payload) -> QString {
			     const QJsonArray list = QJsonDocument::fromJson(payload)
							     .object()
							     .value(QStringLiteral("data"))
							     .toObject()
							     .value(QStringLiteral("connections"))
							     .toArray();
			     for (const QJsonValue &row : list) {
				     const QJsonObject obj = row.toObject();
				     if (obj.value(QStringLiteral("provider")).toString() != QLatin1String("centrifugo"))
					     continue;
				     const QString url =
					     obj.value(QStringLiteral("credentials")).toObject().value(QStringLiteral("url")).toString();
				     if (!url.isEmpty())
					     return url;
			     }
			     return {};
		     };

		     QString wsUrl;
		     if (status >= 200 && status < 300)
			     wsUrl = parseConnections(resp);

		     auto requestToken = [this, headers, clientId, fail](const QString &url) {
			     if (url.isEmpty()) {
				     fail("no centrifugo url");
				     return;
			     }
			     Http(QStringLiteral("https://web.kick.com/api/v1/realtime/auth/connection"), headers,
				  QJsonDocument(QJsonObject{{QStringLiteral("client_id"), clientId}})
					  .toJson(QJsonDocument::Compact),
				  "POST", [this, url, fail](int st, const QByteArray &tokBody, const QString &) {
					  if (st < 200 || st >= 300) {
						  fail("token http");
						  return;
					  }
					  const QString token = QJsonDocument::fromJson(tokBody)
									.object()
									.value(QStringLiteral("data"))
									.toObject()
									.value(QStringLiteral("token"))
									.toString();
					  if (token.isEmpty()) {
						  fail("empty token");
						  return;
					  }
					  QStringList channels;
					  if (!kickChatroomId.isEmpty()) {
						  channels << QStringLiteral("chatrooms.%1.v2").arg(kickChatroomId);
						  channels << QStringLiteral("chatrooms.%1").arg(kickChatroomId);
						  channels << QStringLiteral("chatroom_%1").arg(kickChatroomId);
					  }
					  if (!kickChannelId.isEmpty()) {
						  channels << QStringLiteral("channel.%1").arg(kickChannelId);
						  channels << QStringLiteral("channel_%1").arg(kickChannelId);
					  }
					  if (channels.isEmpty()) {
						  fail("no channels");
						  return;
					  }
					  StartKickCentrifugoWs(url, token, channels);
				  });
		     };

		     if (!wsUrl.isEmpty()) {
			     requestToken(wsUrl);
			     return;
		     }

		     Http(QStringLiteral("https://web.kick.com/api/v1/realtime/connection"), headers,
			  QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
			  [requestToken, parseConnections, fail](int st2, const QByteArray &resp2, const QString &) {
				  QString url;
				  if (st2 >= 200 && st2 < 300)
					  url = parseConnections(resp2);
				  if (url.isEmpty())
					  fail("connection http");
				  else
					  requestToken(url);
			  });
	     });
}

void ChatDock::StartKickCentrifugoWs(const QString &wsUrl, const QString &token, const QStringList &channels)
{
	DisconnectKickChat();
	if (wsUrl.isEmpty() || token.isEmpty() || channels.isEmpty() || !connectionsActive)
		return;

	kickSubscribed = false;
	kickWsGen++;
	const int gen = kickWsGen;
	auto session = std::make_shared<ChatCurlWsSession>();
	kickWs = session;
	QPointer<ChatDock> alive(this);
	const QByteArray urlBytes = wsUrl.toUtf8();
	const QByteArray tokenBytes = token.toUtf8();
	const QStringList feeds = channels;

	std::thread([alive, session, gen, urlBytes, tokenBytes, feeds] {
		CURL *curl = curl_easy_init();
		if (!curl)
			return;
		ApplyCurlSsl(curl);
		curl_slist *hdrs = nullptr;
		hdrs = curl_slist_append(hdrs, "Origin: https://kick.com");
		hdrs = curl_slist_append(hdrs, "Referer: https://kick.com/");
		hdrs = curl_slist_append(hdrs,
					 "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
					 "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
		curl_easy_setopt(curl, CURLOPT_URL, urlBytes.constData());
		curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
		const CURLcode connectRc = curl_easy_perform(curl);
		if (connectRc != CURLE_OK) {
			blog(LOG_WARNING, "[Stream Suite] Kick Centrifugo WS connect failed (%s): %s", urlBytes.constData(),
			     curl_easy_strerror(connectRc));
			curl_slist_free_all(hdrs);
			curl_easy_cleanup(curl);
			if (alive) {
				QMetaObject::invokeMethod(
					alive,
					[alive] {
						if (!alive || !alive->connectionsActive)
							return;
						alive->kickConnectStarted = false;
						alive->AppendSystemMessage(Tr("ChatDockKickConnectFailed"), Kick);
						if (!alive->kickReconnectTimer->isActive())
							alive->kickReconnectTimer->start(12000);
					},
					Qt::QueuedConnection);
			}
			return;
		}

		quint64 nextId = 1;
		QJsonObject connectObj{{QStringLiteral("id"), 1},
				       {QStringLiteral("connect"),
					QJsonObject{{QStringLiteral("token"), QString::fromUtf8(tokenBytes)},
						    {QStringLiteral("name"), QStringLiteral("js")}}}};
		CurlWsSendText(curl, QJsonDocument(connectObj).toJson(QJsonDocument::Compact));
		nextId = 2;

		bool connected = false;
		bool announced = false;
		QByteArray frameBuf;

		auto handleLine = [&](const QByteArray &line) {
			if (line == "{}") {
				CurlWsSendText(curl, QByteArrayLiteral("{}"));
				return;
			}
			const QJsonObject msg = QJsonDocument::fromJson(line).object();
			if (msg.isEmpty())
				return;

			if (msg.contains(QStringLiteral("id"))) {
				if (msg.contains(QStringLiteral("error"))) {
					blog(LOG_WARNING, "[Stream Suite] Kick Centrifugo reply error: %s", line.constData());
					return;
				}
				if (msg.contains(QStringLiteral("connect")) && !connected) {
					connected = true;
					QByteArray batch;
					for (const QString &channel : feeds) {
						QJsonObject sub{{QStringLiteral("id"), static_cast<double>(nextId++)},
								{QStringLiteral("subscribe"),
								 QJsonObject{{QStringLiteral("channel"), channel}}}};
						const QByteArray piece = QJsonDocument(sub).toJson(QJsonDocument::Compact);
						if (!batch.isEmpty())
							batch += '\n';
						batch += piece;
					}
					if (!batch.isEmpty())
						CurlWsSendText(curl, batch);
					return;
				}
				if (msg.contains(QStringLiteral("subscribe")) && !announced) {
					announced = true;
					if (alive) {
						QMetaObject::invokeMethod(
							alive,
							[alive, gen] {
								if (!alive || alive->kickWsGen != gen)
									return;
								alive->kickSubscribed = true;
								if (!alive->kickConnectedAnnounced) {
									alive->kickConnectedAnnounced = true;
									alive->AppendSystemMessage(
										Tr("ChatDockConnected").arg(PlatformLabel(Kick)), Kick);
								}
							},
							Qt::QueuedConnection);
					}
					return;
				}
				return;
			}

			if (msg.contains(QStringLiteral("push")) && alive) {
				const QByteArray payload = line;
				QMetaObject::invokeMethod(
					alive,
					[alive, gen, payload] {
						if (!alive || alive->kickWsGen != gen)
							return;
						alive->HandleKickCentrifugoFrame(payload);
					},
					Qt::QueuedConnection);
			}
		};

		while (!session->stop.load()) {
			for (const QByteArray &out : session->takeOutbound()) {
				if (!CurlWsSendText(curl, out)) {
					session->stop = true;
					break;
				}
			}
			char buffer[16384];
			size_t n = 0;
			const curl_ws_frame *meta = nullptr;
			const CURLcode rc = curl_ws_recv(curl, buffer, sizeof(buffer), &n, &meta);
			if (rc == CURLE_AGAIN) {
				CurlWsWaitReadable(curl, 200);
				continue;
			}
			if (rc != CURLE_OK || !meta)
				break;
			if (meta->flags & CURLWS_CLOSE)
				break;
			if (meta->flags & CURLWS_PING) {
				size_t sent = 0;
				curl_ws_send(curl, buffer, n, &sent, 0, CURLWS_PONG);
				continue;
			}
			if (!(meta->flags & CURLWS_TEXT) && !(meta->flags & CURLWS_BINARY))
				continue;
			if (n > 0)
				frameBuf.append(buffer, static_cast<int>(n));
			if (meta->flags & CURLWS_CONT)
				continue;
			if (frameBuf.isEmpty())
				continue;
			const QByteArray complete = frameBuf;
			frameBuf.clear();
			for (const QByteArray &line : complete.split('\n')) {
				const QByteArray trimmed = line.trimmed();
				if (!trimmed.isEmpty())
					handleLine(trimmed);
			}
		}

		curl_slist_free_all(hdrs);
		curl_easy_cleanup(curl);
		if (alive) {
			QMetaObject::invokeMethod(
				alive,
				[alive, gen] {
					if (!alive || alive->kickWsGen != gen || !alive->connectionsActive)
						return;
					alive->kickSubscribed = false;
					if (!alive->kickReconnectTimer->isActive())
						alive->kickReconnectTimer->start(10000);
				},
				Qt::QueuedConnection);
		}
	}).detach();
}

void ChatDock::ConnectKickChat()
{
	if (!connectionsActive)
		return;
	if (kickChatroomId.isEmpty() && kickChannelId.isEmpty())
		return;
	StartKickCentrifugo();
}

void ChatDock::DisconnectKickChat()
{
	kickWsGen++;
	if (kickWs) {
		kickWs->stop = true;
		kickWs.reset();
	}
	kickSubscribed = false;
}

static QString KickEventNameFromObject(const QJsonObject &obj)
{
	const QString type = obj.value(QStringLiteral("type")).toString();
	if (!type.isEmpty())
		return type;
	return obj.value(QStringLiteral("event")).toString(obj.value(QStringLiteral("event_type")).toString());
}

/* Kick wraps Centrifugo pubs as { type/event: "App\\Events\\...", data: {...|json-string} }. */
static QJsonObject KickUnwrapEventData(QJsonObject data, QString *eventName)
{
	for (int depth = 0; depth < 3; ++depth) {
		if (eventName && eventName->isEmpty())
			*eventName = KickEventNameFromObject(data);
		if (!data.contains(QStringLiteral("data")))
			break;
		/* Chat messages put content/sender on the outer object — don't unwrap those. */
		if (data.contains(QStringLiteral("content")) || data.contains(QStringLiteral("sender")))
			break;
		const QJsonValue inner = data.value(QStringLiteral("data"));
		QJsonObject next;
		if (inner.isObject()) {
			next = inner.toObject();
		} else if (inner.isString()) {
			next = QJsonDocument::fromJson(inner.toString().toUtf8()).object();
		} else {
			break;
		}
		if (next.isEmpty())
			break;
		data = next;
	}
	if (eventName && eventName->isEmpty())
		*eventName = KickEventNameFromObject(data);
	return data;
}

static QString KickDeletedMessageId(const QJsonObject &data)
{
	/* Prefer the nested original message id — top-level "id" is often the delete event id. */
	QString msgId = JsonId(data.value(QStringLiteral("message")).toObject().value(QStringLiteral("id")));
	if (msgId.isEmpty())
		msgId = JsonId(data.value(QStringLiteral("message_id")));
	if (msgId.isEmpty())
		msgId = JsonId(data.value(QStringLiteral("deleted_message_id")));
	if (msgId.isEmpty()) {
		const QJsonObject nested = data.value(QStringLiteral("data")).toObject();
		msgId = JsonId(nested.value(QStringLiteral("message")).toObject().value(QStringLiteral("id")));
		if (msgId.isEmpty())
			msgId = JsonId(nested.value(QStringLiteral("message_id")));
	}
	if (msgId.isEmpty())
		msgId = JsonId(data.value(QStringLiteral("id")));
	return msgId;
}

static bool KickLooksLikeMessageDeleted(const QString &eventName, const QJsonObject &data)
{
	if (eventName.contains(QStringLiteral("MessageDeleted"), Qt::CaseInsensitive) ||
	    eventName.contains(QStringLiteral("ChatMessageDeleted"), Qt::CaseInsensitive))
		return true;
	const QString type = data.value(QStringLiteral("type")).toString();
	if (type == QLatin1String("message_deleted") || type.contains(QStringLiteral("MessageDeleted")))
		return true;
	if (data.contains(QStringLiteral("message_id")) && data.contains(QStringLiteral("chatroom_id")) &&
	    !data.contains(QStringLiteral("content")) && !data.contains(QStringLiteral("sender")))
		return true;
	/* { id, message: { id } } — Kick delete. Pinned events nest full message with content/sender. */
	if (data.contains(QStringLiteral("message")) && data.value(QStringLiteral("message")).isObject() &&
	    !data.contains(QStringLiteral("content")) && !data.contains(QStringLiteral("sender"))) {
		const QJsonObject message = data.value(QStringLiteral("message")).toObject();
		if (!JsonId(message.value(QStringLiteral("id"))).isEmpty() && !message.contains(QStringLiteral("content")) &&
		    !message.contains(QStringLiteral("sender")))
			return true;
	}
	return false;
}

void ChatDock::HandleKickCentrifugoFrame(const QByteArray &text)
{
	const QJsonObject root = QJsonDocument::fromJson(text).object();
	const QJsonObject push = root.value(QStringLiteral("push")).toObject();
	if (push.isEmpty())
		return;
	if (push.contains(QStringLiteral("disconnect"))) {
		blog(LOG_WARNING, "[Stream Suite] Kick Centrifugo disconnect push");
		return;
	}
	const QJsonObject pub = push.value(QStringLiteral("pub")).toObject();
	QJsonValue dataVal = pub.value(QStringLiteral("data"));
	QJsonObject data;
	QString eventName;
	if (dataVal.isObject()) {
		data = dataVal.toObject();
	} else if (dataVal.isString()) {
		data = QJsonDocument::fromJson(dataVal.toString().toUtf8()).object();
	}
	data = KickUnwrapEventData(data, &eventName);
	HandleKickChatPayload(data, eventName);
}

void ChatDock::HandleKickChatPayload(const QJsonObject &data, const QString &eventName)
{
	if (data.isEmpty())
		return;
	QString resolvedEvent = eventName;
	const QJsonObject payload = KickUnwrapEventData(data, &resolvedEvent);

	if (resolvedEvent.contains(QStringLiteral("StopStreamBroadcast")) ||
	    payload.value(QStringLiteral("type")).toString().contains(QStringLiteral("StopStreamBroadcast"))) {
		OnKickStreamOffline();
		return;
	}

	const QString type = payload.value(QStringLiteral("type")).toString();
	if (KickLooksLikeMessageDeleted(resolvedEvent, payload)) {
		const QString msgId = KickDeletedMessageId(payload);
		if (!msgId.isEmpty()) {
			MarkMessageDeleted(msgId, Tr("ChatDockMessageRemoved"));
		} else {
			blog(LOG_INFO, "[Stream Suite] Kick MessageDeleted without message id: %s",
			     QJsonDocument(payload).toJson(QJsonDocument::Compact).constData());
		}
		return;
	}
	if (resolvedEvent.contains(QStringLiteral("ChatroomClear")) ||
	    resolvedEvent.contains(QStringLiteral("ChatCleared")) || type == QLatin1String("chatroom_clear") ||
	    type == QLatin1String("chat_cleared")) {
		MarkPlatformMessagesDeleted(Kick, Tr("ChatDockChatCleared"));
		AppendSystemMessage(Tr("ChatDockChatCleared"), Kick);
		return;
	}
	if (resolvedEvent.contains(QStringLiteral("UserBanned"))) {
		const QString user =
			payload.value(QStringLiteral("username"))
				.toString(payload.value(QStringLiteral("user"))
						  .toObject()
						  .value(QStringLiteral("username"))
						  .toString(payload.value(QStringLiteral("user"))
								    .toObject()
								    .value(QStringLiteral("slug"))
								    .toString()));
		if (!user.isEmpty()) {
			const QString tip = Tr("ChatDockMessageRemovedUser").arg(user);
			MarkMessagesDeletedByUser(Kick, user, tip);
			AppendSystemMessage(tip, Kick);
		}
		return;
	}

	const bool looksLikeChat = payload.contains(QStringLiteral("content")) ||
				   payload.contains(QStringLiteral("sender")) ||
				   resolvedEvent.contains(QStringLiteral("ChatMessage"));
	if (!looksLikeChat)
		return;
	if (!resolvedEvent.isEmpty() && !resolvedEvent.contains(QStringLiteral("ChatMessage")) &&
	    type != QLatin1String("message") && type != QLatin1String("reply") &&
	    !payload.contains(QStringLiteral("content")))
		return;

	const QJsonObject sender = payload.value(QStringLiteral("sender")).toObject();
	const QJsonObject identity = sender.value(QStringLiteral("identity")).toObject();
	ChatMessage msg;
	msg.platform = Kick;
	msg.id = JsonId(payload.value(QStringLiteral("id")));
	if (msg.id.isEmpty())
		msg.id = JsonId(payload.value(QStringLiteral("message_id")));
	msg.user = sender.value(QStringLiteral("username")).toString(sender.value(QStringLiteral("slug")).toString());
	msg.userLogin = sender.value(QStringLiteral("slug")).toString(msg.user).toLower();
	msg.actorId = JsonId(sender.value(QStringLiteral("id")));
	if (msg.actorId.isEmpty())
		msg.actorId = JsonId(sender.value(QStringLiteral("user_id")));
	if (msg.user.isEmpty())
		msg.user = QStringLiteral("Kick");
	msg.color = identity.value(QStringLiteral("color")).toString(QStringLiteral("#53FC18"));
	msg.text = payload.value(QStringLiteral("content"))
			   .toString(payload.value(QStringLiteral("message")).toString());
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.badges = KickBadgesFromIdentity(identity);
	if (msg.text.isEmpty())
		return;
	if (msg.id.isEmpty())
		msg.id = QStringLiteral("kick-%1-%2").arg(msg.ts).arg(msg.user);
	DropKickEcho(msg);
	AppendMessage(msg);
}

void ChatDock::OnKickStreamOffline()
{
	SetViewerCount(Kick, 0, false);
}

static bool ItemHasChatId(const QListWidgetItem *item, const QString &want);

void ChatDock::SendKickMessage(const QString &text)
{
	EnsureToken(Kick, [this, text](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Kick];
		QJsonObject body;
		body.insert("content", text);
		body.insert("type", "user");
		if (!acc.userId.isEmpty())
			body.insert("broadcaster_user_id", acc.userId.toLongLong());
		Http(QStringLiteral("https://api.kick.com/public/v1/chat"),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
		      {QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
		     [this, text](int status, const QByteArray &reply, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     const QJsonObject root = QJsonDocument::fromJson(reply).object();
			     const QJsonValue data = root.value(QStringLiteral("data"));
			     QJsonObject sent;
			     if (data.isArray()) {
				     const QJsonArray rows = data.toArray();
				     if (!rows.isEmpty())
					     sent = rows.at(0).toObject();
			     } else {
				     sent = data.toObject();
			     }
			     const QString messageId = JsonId(sent.value(QStringLiteral("message_id")));
			     if (!messageId.isEmpty() && seenMessageIds.contains(messageId))
				     return;
			     const qint64 now = QDateTime::currentMSecsSinceEpoch();
			     const QString self = accounts[Kick].userName;
			     if (messageList) {
				     for (int i = messageList->count() - 1; i >= 0; --i) {
					     QListWidgetItem *item = messageList->item(i);
					     if (!item || item->data(Qt::UserRole).toBool() || item->data(Qt::UserRole + 7).toBool())
						     continue;
					     if (item->data(Qt::UserRole + 4).toInt() != int(Kick))
						     continue;
					     if (!messageId.isEmpty() && ItemHasChatId(item, messageId))
						     return;
					     const qint64 ts = item->data(Qt::UserRole + 10).toLongLong();
					     if (ts > 0 && now - ts > 20000)
						     break;
					     if (item->data(Qt::UserRole + 5).toString().trimmed() != text.trimmed())
						     continue;
					     const QString name = item->data(Qt::UserRole + 9).toString();
					     if (!self.isEmpty() && name.compare(self, Qt::CaseInsensitive) != 0)
						     continue;
					     return;
				     }
			     }
			     ChatMessage msg;
			     msg.platform = Kick;
			     msg.echo = true;
			     msg.user = self.isEmpty() ? QStringLiteral("You") : self;
			     msg.userLogin = accounts[Kick].login;
			     msg.actorId = accounts[Kick].userId;
			     msg.color = QStringLiteral("#53FC18");
			     msg.text = text;
			     msg.ts = now;
			     msg.id = messageId.isEmpty() ? QStringLiteral("local-kick-%1").arg(msg.ts) : messageId;
			     AppendMessage(msg);
		     });
	});
}

bool ChatDock::TakeTwitchEcho(const ChatMessage &msg)
{
	if (!messageList || twitchLogin.isEmpty())
		return false;
	if (msg.userLogin.compare(twitchLogin, Qt::CaseInsensitive) != 0)
		return false;
	int match = -1;
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || !item->data(Qt::UserRole + 7).toBool())
			continue;
		if (item->data(Qt::UserRole + 4).toInt() != int(Twitch))
			continue;
		const QString id = item->data(Qt::UserRole + 1).toString();
		if (!id.startsWith(QLatin1String("local-tw-")))
			continue;
		const qint64 ts = item->data(Qt::UserRole + 10).toLongLong();
		const qint64 delta = msg.ts - ts;
		if (ts > 0 && msg.ts > 0 && (delta > 20000 || delta < -20000))
			continue;
		if (item->data(Qt::UserRole + 5).toString().trimmed() != msg.text.trimmed())
			continue;
		match = i;
		break;
	}
	if (match < 0)
		return false;
	QListWidgetItem *item = messageList->takeItem(match);
	if (!item)
		return false;
	const QString id = item->data(Qt::UserRole + 1).toString();
	const bool deleteAfter = twitchPendingDeletes.remove(id);
	if (!id.isEmpty())
		seenMessageIds.remove(id);
	for (const QString &alias : item->data(Qt::UserRole + 6).toStringList())
		seenMessageIds.remove(alias);
	delete item;
	return deleteAfter;
}

void ChatDock::DropKickEcho(const ChatMessage &msg)
{
	if (!messageList || msg.text.isEmpty())
		return;
	int match = -1;
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || !item->data(Qt::UserRole + 7).toBool())
			continue;
		if (item->data(Qt::UserRole + 4).toInt() != int(Kick))
			continue;
		if (!msg.id.isEmpty() && ItemHasChatId(item, msg.id)) {
			match = i;
			break;
		}
		if (match >= 0)
			continue;
		const qint64 ts = item->data(Qt::UserRole + 10).toLongLong();
		const qint64 delta = msg.ts - ts;
		if (ts > 0 && msg.ts > 0 && (delta > 20000 || delta < -20000))
			continue;
		if (item->data(Qt::UserRole + 5).toString().trimmed() != msg.text.trimmed())
			continue;
		match = i;
	}
	if (match < 0)
		return;
	QListWidgetItem *item = messageList->takeItem(match);
	if (!item)
		return;
	const QString id = item->data(Qt::UserRole + 1).toString();
	if (!id.isEmpty())
		seenMessageIds.remove(id);
	for (const QString &alias : item->data(Qt::UserRole + 6).toStringList())
		seenMessageIds.remove(alias);
	delete item;
}

void ChatDock::FetchTwitchBadgeSets()
{
	EnsureToken(Twitch, [this](bool ok) {
		if (!ok)
			return;
		const Account &acc = accounts[Twitch];
		auto load = [this](const QString &url) {
			const Account &a = accounts[Twitch];
			Http(url,
			     {{QByteArrayLiteral("Client-Id"), a.clientId.toUtf8()},
			      {QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + a.accessToken.toUtf8()}},
			     {}, "GET", [this](int status, const QByteArray &body, const QString &) {
				     if (status < 200 || status >= 300)
					     return;
				     IngestTwitchBadgeSets(QJsonDocument::fromJson(body).object().value("data").toArray());
			     });
		};
		load(QStringLiteral("https://api.twitch.tv/helix/chat/badges/global"));
		if (!acc.userId.isEmpty())
			load(QStringLiteral("https://api.twitch.tv/helix/chat/badges?broadcaster_id=") + acc.userId);
	});
}

void ChatDock::IngestTwitchBadgeSets(const QJsonArray &sets)
{
	for (const QJsonValue &row : sets) {
		const QJsonObject set = row.toObject();
		const QString setId = set.value(QStringLiteral("set_id")).toString();
		for (const QJsonValue &ver : set.value(QStringLiteral("versions")).toArray()) {
			const QJsonObject v = ver.toObject();
			const QString version = v.value(QStringLiteral("id")).toString();
			/* Prefer 2x — sharper when scaled into the 14px badge slot. */
			const QString url = v.value(QStringLiteral("image_url_2x"))
						   .toString(v.value(QStringLiteral("image_url_1x")).toString());
			if (setId.isEmpty() || version.isEmpty() || url.isEmpty())
				continue;
			const QString key = setId + QLatin1Char('/') + version;
			twitchBadgeUrls.insert(key, url);
			const QString title = v.value(QStringLiteral("title")).toString(setId);
			twitchBadgeTitles.insert(key, title);
		}
	}
}

QVector<ChatDock::ChatBadge> ChatDock::TwitchBadgesFromTags(const QString &badgesTag) const
{
	QVector<ChatBadge> out;
	if (badgesTag.isEmpty())
		return out;
	for (const QString &part : badgesTag.split(QLatin1Char(','))) {
		if (part.isEmpty())
			continue;
		const QString setId = part.section(QLatin1Char('/'), 0, 0);
		const QString version = part.section(QLatin1Char('/'), 1, 1);
		ChatBadge badge;
		badge.key = setId + QLatin1Char('/') + version;
		badge.label = twitchBadgeTitles.value(badge.key, setId);
		badge.url = twitchBadgeUrls.value(badge.key);
		if (badge.url.isEmpty())
			badge.url = twitchBadgeUrls.value(setId + QStringLiteral("/1"));
		if (badge.url.isEmpty())
			badge.url = twitchBadgeUrls.value(setId + QStringLiteral("/0"));
		if (badge.label == setId) {
			const QString altKey = badge.url.isEmpty() ? QString() : badge.key;
			Q_UNUSED(altKey);
			if (twitchBadgeTitles.contains(setId + QStringLiteral("/1")))
				badge.label = twitchBadgeTitles.value(setId + QStringLiteral("/1"));
			else if (twitchBadgeTitles.contains(setId + QStringLiteral("/0")))
				badge.label = twitchBadgeTitles.value(setId + QStringLiteral("/0"));
		}
		/* Skip badges with no CDN URL yet — never invent letter pills (that was the Kick "B"). */
		if (badge.url.isEmpty())
			continue;
		out.append(badge);
	}
	return out;
}

QVector<ChatDock::ChatEmoteSpan> ChatDock::TwitchEmotesFromTag(const QString &emotesTag)
{
	QVector<ChatEmoteSpan> out;
	if (emotesTag.isEmpty())
		return out;
	/* Format: id:start-end,start-end/id2:start-end */
	for (const QString &group : emotesTag.split(QLatin1Char('/'))) {
		if (group.isEmpty())
			continue;
		const int colon = group.indexOf(QLatin1Char(':'));
		if (colon <= 0)
			continue;
		const QString id = group.left(colon);
		const QString ranges = group.mid(colon + 1);
		for (const QString &range : ranges.split(QLatin1Char(','))) {
			const int dash = range.indexOf(QLatin1Char('-'));
			if (dash < 0)
				continue;
			bool okS = false;
			bool okE = false;
			const int start = range.left(dash).toInt(&okS);
			const int end = range.mid(dash + 1).toInt(&okE);
			if (!okS || !okE || start < 0 || end < start)
				continue;
			ChatEmoteSpan span;
			span.id = id;
			span.start = start;
			span.end = end;
			out.append(span);
		}
	}
	std::sort(out.begin(), out.end(),
		  [](const ChatEmoteSpan &a, const ChatEmoteSpan &b) { return a.start < b.start; });
	return out;
}

QString ChatDock::TwitchEmoteImageUrl(const QString &emoteId)
{
	/* "default" returns animated GIF when available, otherwise static PNG. */
	return QStringLiteral("https://static-cdn.jtvnw.net/emoticons/v2/%1/default/dark/1.0").arg(emoteId);
}

QWidget *ChatDock::BuildTwitchEmoteBody(const ChatMessage &msg, const QString &prefixHtml, QWidget *parent)
{
	auto *flow = new ChatFlowBody(parent);
	flow->addHtml(prefixHtml);

	int last = 0;
	const QString &raw = msg.text;
	for (const ChatEmoteSpan &span : msg.emotes) {
		if (span.start < last || span.end >= raw.size() || span.start > span.end)
			continue;
		if (span.start > last)
			flow->addHtml(Linkify(EscapeHtml(raw.mid(last, span.start - last))));
		QLabel *slot = flow->addEmoteSlot(18);
		slot->setToolTip(raw.mid(span.start, span.end - span.start + 1));
		const QString cacheKey = QStringLiteral("twitch-emote:%1").arg(span.id);
		FetchImageToLabel(TwitchEmoteImageUrl(span.id), slot, 18, cacheKey);
		last = span.end + 1;
	}
	if (last < raw.size())
		flow->addHtml(Linkify(EscapeHtml(raw.mid(last))));
	return flow;
}

QVector<ChatDock::ChatEmoteSpan> ChatDock::KickEmotesFromText(const QString &raw)
{
	QVector<ChatEmoteSpan> out;
	static const QRegularExpression re(QStringLiteral(R"(\[emote:(\d+):([^\]]+)\])"));
	auto it = re.globalMatch(raw);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		ChatEmoteSpan span;
		span.id = m.captured(1);
		span.start = m.capturedStart();
		span.end = m.capturedEnd() - 1;
		out.append(span);
	}
	return out;
}

QWidget *ChatDock::BuildKickEmoteBody(const ChatMessage &msg, const QString &prefixHtml, QWidget *parent)
{
	auto *flow = new ChatFlowBody(parent);
	flow->addHtml(prefixHtml);

	const QVector<ChatEmoteSpan> emotes = KickEmotesFromText(msg.text);
	int last = 0;
	const QString &raw = msg.text;
	for (const ChatEmoteSpan &span : emotes) {
		if (span.start < last || span.end >= raw.size() || span.start > span.end)
			continue;
		if (span.start > last)
			flow->addHtml(Linkify(EscapeHtml(raw.mid(last, span.start - last))));
		QLabel *slot = flow->addEmoteSlot(18);
		/* Tooltip = emote name inside [emote:id:name]. */
		static const QRegularExpression nameRe(QStringLiteral(R"(\[emote:\d+:([^\]]+)\])"));
		const auto nameMatch = nameRe.match(raw.mid(span.start, span.end - span.start + 1));
		slot->setToolTip(nameMatch.hasMatch() ? nameMatch.captured(1)
						      : raw.mid(span.start, span.end - span.start + 1));
		const QString cacheKey = QStringLiteral("kick-emote:%1").arg(span.id);
		FetchImageToLabel(KickEmoteImageUrl(span.id), slot, 18, cacheKey);
		last = span.end + 1;
	}
	if (last < raw.size())
		flow->addHtml(Linkify(EscapeHtml(raw.mid(last))));
	return flow;
}

void ChatDock::IngestKickSubscriberBadges(const QJsonArray &badges)
{
	kickSubBadges.clear();
	for (const QJsonValue &row : badges) {
		const QJsonObject obj = row.toObject();
		KickSubBadgeTier tier;
		tier.months = obj.value(QStringLiteral("months")).toInt();
		tier.url = obj.value(QStringLiteral("badge_image")).toObject().value(QStringLiteral("src")).toString();
		if (tier.url.isEmpty())
			tier.url = obj.value(QStringLiteral("badge_image")).toString();
		if (tier.url.isEmpty())
			continue;
		kickSubBadges.append(tier);
	}
	std::sort(kickSubBadges.begin(), kickSubBadges.end(),
		  [](const KickSubBadgeTier &a, const KickSubBadgeTier &b) { return a.months < b.months; });
}

QString ChatDock::KickSubscriberBadgeUrl(int months) const
{
	QString best;
	for (const KickSubBadgeTier &tier : kickSubBadges) {
		if (tier.months <= months)
			best = tier.url;
	}
	return best;
}

QString ChatDock::KickRoleBadgeKey(const QString &type, int count)
{
	QString t = type.trimmed().toLower();
	if (t == QLatin1String("host"))
		t = QStringLiteral("broadcaster");
	else if (t == QLatin1String("mod"))
		t = QStringLiteral("moderator");
	static const QSet<QString> kKnown{
		QStringLiteral("broadcaster"), QStringLiteral("moderator"), QStringLiteral("vip"),
		QStringLiteral("og"),          QStringLiteral("verified"),  QStringLiteral("staff"),
		QStringLiteral("founder"),     QStringLiteral("sidekick"),  QStringLiteral("subscriber"),
		QStringLiteral("sub_gifter"),  QStringLiteral("bot"),
	};
	if (!kKnown.contains(t))
		return {};
	if (t == QLatin1String("sub_gifter") && count > 0)
		return QStringLiteral("kick-role:sub_gifter:%1").arg(count);
	if (t == QLatin1String("subscriber") && count > 0)
		return QStringLiteral("kick-role:subscriber:%1").arg(count);
	return QStringLiteral("kick-role:%1").arg(t);
}

QPixmap ChatDock::KickRoleBadgePixmap(const QString &role, int size)
{
	QString r = role.trimmed().toLower();
	int gifterCount = 0;
	if (r.startsWith(QLatin1String("subscriber:"))) {
		r = QStringLiteral("subscriber");
	} else if (r.startsWith(QLatin1String("sub_gifter:"))) {
		gifterCount = r.section(QLatin1Char(':'), 1).toInt();
		r = QStringLiteral("sub_gifter");
	}

	QString fileStem = r;
	if (r == QLatin1String("sub_gifter")) {
		if (gifterCount >= 200)
			fileStem = QStringLiteral("subGifter200");
		else if (gifterCount >= 100)
			fileStem = QStringLiteral("subGifter100");
		else if (gifterCount >= 50)
			fileStem = QStringLiteral("subGifter50");
		else if (gifterCount >= 25)
			fileStem = QStringLiteral("subGifter25");
		else
			fileStem = QStringLiteral("sub_gifter");
	}

	const QString cacheKey = fileStem + QLatin1Char('@') + QString::number(size);
	static QHash<QString, QPixmap> cache;
	if (cache.contains(cacheKey))
		return cache.value(cacheKey);

	QPixmap pix(size, size);
	pix.fill(Qt::transparent);

	/* Official Kick badge SVGs shipped under data/kick-badges/. */
	const QString rel = QStringLiteral("kick-badges/%1.svg").arg(fileStem);
	char *modPath = obs_module_file(rel.toUtf8().constData());
	QString svgPath = modPath ? QString::fromUtf8(modPath) : QString();
	if (modPath)
		bfree(modPath);

	QSvgRenderer renderer;
	if (!svgPath.isEmpty() && QFile::exists(svgPath))
		renderer.load(svgPath);
	if (!renderer.isValid()) {
		/* Dev/rundir fallback next to the source tree data folder. */
		const QString local = QStringLiteral("data/kick-badges/%1.svg").arg(fileStem);
		if (QFile::exists(local))
			renderer.load(local);
	}
	if (renderer.isValid()) {
		QPainter p(&pix);
		p.setRenderHint(QPainter::Antialiasing, true);
		renderer.render(&p, QRectF(0, 0, size, size));
		cache.insert(cacheKey, pix);
		return pix;
	}

	/* Minimal fallback if the SVG asset is missing (e.g. staff). */
	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.setBrush(r == QLatin1String("staff") ? QColor(0x53, 0xFC, 0x18) : QColor(0x66, 0x66, 0x66));
	p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
	if (r == QLatin1String("staff")) {
		p.setPen(Qt::black);
		QFont f = p.font();
		f.setBold(true);
		f.setPixelSize(qMax(7, int(size * 0.55)));
		p.setFont(f);
		p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QStringLiteral("K"));
	}
	cache.insert(cacheKey, pix);
	return pix;
}

QVector<ChatDock::ChatBadge> ChatDock::KickBadgesFromIdentity(const QJsonObject &identity) const
{
	QVector<ChatBadge> out;
	QSet<QString> seenKeys;
	bool hasBroadcaster = false;

	auto normalizeRole = [](QString t) {
		t = t.trimmed().toLower();
		if (t == QLatin1String("host"))
			return QStringLiteral("broadcaster");
		if (t == QLatin1String("mod"))
			return QStringLiteral("moderator");
		return t;
	};

	/* 1) Role badges from legacy identity.badges — official Kick SVGs (or channel sub art). */
	struct LegacyBadge {
		QString type;
		QString label;
		int count = 0;
		int order = 100;
	};
	QVector<LegacyBadge> legacy;
	for (const QJsonValue &row : identity.value(QStringLiteral("badges")).toArray()) {
		const QJsonObject obj = row.toObject();
		LegacyBadge b;
		b.type = normalizeRole(obj.value(QStringLiteral("type"))
					       .toString(obj.value(QStringLiteral("name")).toString()));
		b.label = obj.value(QStringLiteral("text")).toString(b.type);
		b.count = obj.value(QStringLiteral("count")).toInt(obj.value(QStringLiteral("months")).toInt());
		b.order = obj.value(QStringLiteral("sort_order")).toInt(100);
		if (!b.type.isEmpty())
			legacy.append(b);
	}
	std::sort(legacy.begin(), legacy.end(),
		  [](const LegacyBadge &a, const LegacyBadge &b) { return a.order < b.order; });

	for (const LegacyBadge &b : legacy) {
		if (seenKeys.contains(b.type))
			continue;
		ChatBadge badge;
		badge.key = b.type;
		badge.label = b.label.isEmpty() ? b.type : b.label;
		if (b.type == QLatin1String("subscriber")) {
			const QString channelUrl = KickSubscriberBadgeUrl(qMax(1, b.count));
			badge.url = channelUrl.isEmpty() ? KickRoleBadgeKey(b.type, b.count) : channelUrl;
		} else {
			badge.url = KickRoleBadgeKey(b.type, b.count);
		}
		if (badge.url.isEmpty())
			continue;
		seenKeys.insert(b.type);
		if (b.type == QLatin1String("broadcaster"))
			hasBroadcaster = true;
		out.append(badge);
	}

	/*
	 * 2) badges_v2 — Kick cosmetics the user opted to show (selected:true), including
	 *    viewer level + global/custom badges. Broadcaster rows on Kick only show the
	 *    host mic, so skip v2 there.
	 */
	if (!hasBroadcaster) {
		QVector<QJsonObject> v2;
		for (const QJsonValue &row : identity.value(QStringLiteral("badges_v2")).toArray()) {
			const QJsonObject obj = row.toObject();
			if (obj.value(QStringLiteral("image_url")).toString().isEmpty())
				continue;
			if (obj.contains(QStringLiteral("selected")) && !obj.value(QStringLiteral("selected")).toBool())
				continue;
			v2.append(obj);
		}
		std::sort(v2.begin(), v2.end(), [](const QJsonObject &a, const QJsonObject &b) {
			return a.value(QStringLiteral("sort_order")).toInt() <
			       b.value(QStringLiteral("sort_order")).toInt();
		});
		for (const QJsonObject &obj : v2) {
			const QString name =
				normalizeRole(obj.value(QStringLiteral("name"))
						      .toString(obj.value(QStringLiteral("badge_type")).toString()));
			const QString badgeType = normalizeRole(obj.value(QStringLiteral("badge_type")).toString());
			/* Don't duplicate a role we already rendered from legacy badges. */
			if ((!name.isEmpty() && seenKeys.contains(name)) ||
			    (!badgeType.isEmpty() && seenKeys.contains(badgeType)))
				continue;
			ChatBadge badge;
			badge.key = name.isEmpty() ? badgeType : name;
			badge.label = obj.value(QStringLiteral("text")).toString(badge.key);
			if (badge.key == QLatin1String("level")) {
				const int level = obj.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("level")).toInt();
				if (level > 0)
					badge.label = QStringLiteral("Level %1").arg(level);
			}
			badge.url = obj.value(QStringLiteral("image_url")).toString();
			if (badge.url.isEmpty() || badge.key.isEmpty())
				continue;
			seenKeys.insert(badge.key.toLower());
			out.append(badge);
		}
	}

	return out;
}

QString ChatDock::KickEmoteImageUrl(const QString &emoteId)
{
	return QStringLiteral("https://files.kick.com/emotes/%1/fullsize").arg(emoteId);
}

void ChatDock::ApplyBadgeToLabel(const ChatBadge &badge, QLabel *target)
{
	if (!target || badge.url.isEmpty())
		return;
	if (badge.url.startsWith(QLatin1String("kick-role:"))) {
		const QString role = badge.url.mid(QStringLiteral("kick-role:").size());
		target->setMovie(nullptr);
		target->setPixmap(KickRoleBadgePixmap(role, 14));
		return;
	}
	if (badge.url.startsWith(QLatin1String("yt-role:"))) {
		const QString role = badge.url.mid(QStringLiteral("yt-role:").size());
		target->setMovie(nullptr);
		target->setPixmap(YouTubeRoleBadgePixmap(role, 14));
		return;
	}
	FetchBadgePixmap(badge.url, target);
}

QPixmap ChatDock::YouTubeRoleBadgePixmap(const QString &role, int size)
{
	QString r = role.trimmed().toLower();
	if (r == QLatin1String("mod"))
		r = QStringLiteral("moderator");
	else if (r == QLatin1String("sponsor"))
		r = QStringLiteral("member");

	const QString cacheKey = QStringLiteral("yt:") + r + QLatin1Char('@') + QString::number(size);
	static QHash<QString, QPixmap> cache;
	if (cache.contains(cacheKey))
		return cache.value(cacheKey);

	QPixmap pix(size, size);
	pix.fill(Qt::transparent);

	const QString rel = QStringLiteral("youtube-badges/%1.svg").arg(r);
	char *modPath = obs_module_file(rel.toUtf8().constData());
	QString svgPath = modPath ? QString::fromUtf8(modPath) : QString();
	if (modPath)
		bfree(modPath);

	QSvgRenderer renderer;
	if (!svgPath.isEmpty() && QFile::exists(svgPath))
		renderer.load(svgPath);
	if (!renderer.isValid()) {
		const QString local = QStringLiteral("data/youtube-badges/%1.svg").arg(r);
		if (QFile::exists(local))
			renderer.load(local);
	}
	if (renderer.isValid()) {
		QPainter p(&pix);
		p.setRenderHint(QPainter::Antialiasing, true);
		renderer.render(&p, QRectF(0, 0, size, size));
		cache.insert(cacheKey, pix);
		return pix;
	}

	/* Colored square fallback if SVG missing. */
	QColor fill(0x60, 0x60, 0x60);
	if (r == QLatin1String("owner"))
		fill = QColor(0xFF, 0x00, 0x00);
	else if (r == QLatin1String("moderator"))
		fill = QColor(0x5B, 0x8D, 0xEF);
	else if (r == QLatin1String("member"))
		fill = QColor(0x0F, 0x9D, 0x58);
	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.2, size * 0.2);
	cache.insert(cacheKey, pix);
	return pix;
}

void ChatDock::ApplyImageDataToLabel(QLabel *label, const QByteArray &body, int size)
{
	if (!label || body.isEmpty() || size <= 0)
		return;

	/* YouTube standard emoji often arrives as SVG. QPixmap does not load that. */
	if (body.startsWith("<svg") || body.startsWith("<?xml") || body.left(256).contains("<svg")) {
		QSvgRenderer renderer(body);
		if (renderer.isValid()) {
			QPixmap pix(size, size);
			pix.fill(Qt::transparent);
			QPainter painter(&pix);
			renderer.render(&painter);
			label->setMovie(nullptr);
			label->setScaledContents(true);
			label->setPixmap(pix);
			return;
		}
	}

	auto useStatic = [&]() {
		QPixmap pix;
		if (!pix.loadFromData(body))
			return;
		label->setMovie(nullptr);
		label->setScaledContents(true);
		label->setPixmap(pix.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	};

	/* GIF (Kick fullsize + Twitch /default when animated) — play via QMovie.
	 * Do not use HTML <img>: QLabel rich-text only shows the first frame. */
	const bool looksGif = body.startsWith("GIF87") || body.startsWith("GIF89") || body.startsWith("GIF8");
	if (looksGif) {
		auto *buf = new QBuffer(label);
		buf->setData(body);
		if (!buf->open(QIODevice::ReadOnly)) {
			delete buf;
			useStatic();
			return;
		}
		auto *movie = new QMovie(buf, QByteArrayLiteral("GIF"), label);
		movie->setCacheMode(QMovie::CacheAll);
		movie->setScaledSize(QSize(size, size));
		if (!movie->isValid()) {
			delete movie;
			useStatic();
			return;
		}
		label->setScaledContents(false);
		label->setPixmap(QPixmap());
		label->setMovie(movie);
		movie->start();
		return;
	}

	/* Animated WebP (RIFF....WEBP) if the Qt image plugin supports it. */
	if (body.size() > 12 && body.startsWith("RIFF") && body.mid(8, 4) == QByteArrayLiteral("WEBP")) {
		auto *buf = new QBuffer(label);
		buf->setData(body);
		if (buf->open(QIODevice::ReadOnly)) {
			auto *movie = new QMovie(buf, QByteArrayLiteral("webp"), label);
			movie->setCacheMode(QMovie::CacheAll);
			movie->setScaledSize(QSize(size, size));
			if (movie->isValid() && movie->frameCount() > 1) {
				label->setScaledContents(false);
				label->setPixmap(QPixmap());
				label->setMovie(movie);
				movie->start();
				return;
			}
			delete movie;
		} else {
			delete buf;
		}
	}

	useStatic();
}

void ChatDock::FetchImageToLabel(const QString &url, QLabel *target, int size, const QString &cacheKey)
{
	if (!target || url.isEmpty() || size <= 0)
		return;
	const QString key = cacheKey.isEmpty() ? url : cacheKey;

	/* Always prefer raw bytes so GIFs keep animating. Never short-circuit to the
	 * static QPixmap cache (that was freezing Kick/Twitch animated emotes). */
	if (badgeRawBytes.contains(key)) {
		ApplyImageDataToLabel(target, badgeRawBytes.value(key), size);
		return;
	}
	if (badgeRawBytes.contains(url)) {
		ApplyImageDataToLabel(target, badgeRawBytes.value(url), size);
		return;
	}

	badgeWaiters[key].append(QPointer<QLabel>(target));
	if (badgeFetchPending.contains(key))
		return;
	badgeFetchPending.insert(key);

	const QByteArray referer = url.contains(QLatin1String("kick.com"), Qt::CaseInsensitive)
					   ? QByteArrayLiteral("https://kick.com/")
					   : (url.contains(QLatin1String("ggpht.com"), Qt::CaseInsensitive) ||
					      url.contains(QLatin1String("ytimg.com"), Qt::CaseInsensitive) ||
					      url.contains(QLatin1String("gstatic.com"), Qt::CaseInsensitive))
						     ? QByteArrayLiteral("https://www.youtube.com/")
						     : url.contains(QLatin1String("jsdelivr.net"), Qt::CaseInsensitive)
							   ? QByteArrayLiteral("https://cdn.jsdelivr.net/")
							   : QByteArrayLiteral("https://www.twitch.tv/");
	const QList<QPair<QByteArray, QByteArray>> headers = {
		{QByteArrayLiteral("User-Agent"),
		 QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
				   "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")},
		{QByteArrayLiteral("Referer"), referer},
	};
	Http(url, headers, {}, "GET", [this, url, key, size](int status, const QByteArray &body, const QString &) {
		badgeFetchPending.remove(key);
		QList<QPointer<QLabel>> waiters = badgeWaiters.take(key);
		if (status < 200 || status >= 300 || body.isEmpty())
			return;
		badgeRawBytes.insert(key, body);
		if (key != url)
			badgeRawBytes.insert(url, body);
		QPixmap pix;
		if (pix.loadFromData(body)) {
			const QPixmap scaled = pix.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
			badgePixmaps.insert(key, scaled);
			if (key != url)
				badgePixmaps.insert(url, scaled);
		}
		for (const QPointer<QLabel> &alive : waiters) {
			if (alive)
				ApplyImageDataToLabel(alive, body, size);
		}
	});
}

void ChatDock::FetchBadgePixmap(const QString &url, QLabel *target)
{
	FetchImageToLabel(url, target, 14);
}

int ChatDock::MessageTextHeight(const QString &html, int width, const QFont &font)
{
	QTextDocument doc;
	doc.setDefaultFont(font);
	doc.setDocumentMargin(0);
	doc.setHtml(html);
	doc.setTextWidth(qMax(40, width));
	return qMax(16, int(std::ceil(doc.size().height())));
}

void ChatDock::AppendSystemMessage(const QString &text, Platform platform, bool withIcon)
{
	ChatMessage msg;
	msg.system = true;
	msg.showPlatformIcon = withIcon;
	msg.platform = platform;
	msg.user = QStringLiteral("System");
	msg.color = QStringLiteral("#9a9a9a");
	msg.text = text;
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	/* Stable-ish id avoids duplicates if the same status is emitted twice quickly. */
	msg.id = QStringLiteral("sys-%1-%2").arg(int(platform)).arg(text);
	AppendMessage(msg);
}

void ChatDock::ClearSystemMessages()
{
	if (!messageList)
		return;
	for (int i = messageList->count() - 1; i >= 0; --i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || !item->data(Qt::UserRole).toBool())
			continue;
		const QString id = item->data(Qt::UserRole + 1).toString();
		if (!id.isEmpty())
			seenMessageIds.remove(id);
		delete messageList->takeItem(i);
	}
}

int ChatDock::LayoutMessageRow(QWidget *row, QListWidgetItem *item, int viewportWidth) const
{
	if (!row || !item)
		return 0;

	auto *body = row->findChild<QWidget *>(QStringLiteral("chatMsgBody"));
	if (!body)
		return 0;

	/* Leave a small gutter so the rounded card never clips against the scrollbar. */
	const int rowW = qMax(120, viewportWidth - 8);
	row->setFixedWidth(rowW);

	auto *lay = qobject_cast<QHBoxLayout *>(row->layout());
	const int marginL = lay ? lay->contentsMargins().left() : 10;
	const int marginR = lay ? lay->contentsMargins().right() : 12;
	const int marginT = lay ? lay->contentsMargins().top() : 8;
	const int marginB = lay ? lay->contentsMargins().bottom() : 8;
	const int spacing = lay ? lay->spacing() : 8;

	/* Fixed icon/badge sizes — sizeHint is unreliable before the row is shown. */
	int side = 0;
	int sideCount = 0;
	if (lay) {
		for (int i = 0; i < lay->count(); ++i) {
			QWidget *w = lay->itemAt(i) ? lay->itemAt(i)->widget() : nullptr;
			if (!w || w == body)
				continue;
			const int fixed = (w->minimumWidth() > 0 && w->minimumWidth() == w->maximumWidth()) ? w->minimumWidth()
													  : 14;
			side += fixed;
			++sideCount;
		}
	}
	const int chrome = marginL + marginR + side + sideCount * spacing;
	const int textW = qMax(60, rowW - chrome);

	body->setMinimumWidth(0);
	body->setMaximumWidth(textW);
	body->setFixedWidth(textW);

	int textH = 16;
	if (auto *label = qobject_cast<QLabel *>(body)) {
		textH = MessageTextHeight(label->text(), textW, label->font());
	} else if (body->hasHeightForWidth()) {
		textH = body->heightForWidth(textW);
	} else {
		textH = body->sizeHint().height();
	}
	body->setFixedHeight(textH);

	int sideH = 18;
	if (lay) {
		for (int i = 0; i < lay->count(); ++i) {
			QWidget *w = lay->itemAt(i) ? lay->itemAt(i)->widget() : nullptr;
			if (!w || w == body)
				continue;
			const int fixedH = (w->minimumHeight() > 0 && w->minimumHeight() == w->maximumHeight())
						   ? w->minimumHeight()
						   : w->height();
			sideH = qMax(sideH, fixedH);
		}
	}
	const int contentH = qMax(sideH, textH);
	const int rowH = contentH + marginT + marginB;
	row->setFixedHeight(rowH);
	item->setSizeHint(QSize(rowW, rowH));
	return rowH;
}

void ChatDock::RelayoutMessageItems()
{
	if (!messageList)
		return;
	const int vw = qMax(120, messageList->viewport()->width());
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		QWidget *row = messageList->itemWidget(item);
		LayoutMessageRow(row, item, vw);
	}
}

QPixmap ChatDock::DeletedMessageIconPixmap(int size)
{
	static QHash<int, QPixmap> cache;
	if (cache.contains(size))
		return cache.value(size);

	QPixmap pix(size, size);
	pix.fill(Qt::transparent);
	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing, true);
	/* Soft red circle with a white "✕" — tooltip carries the full explanation. */
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0xE5, 0x3E, 0x3E));
	p.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));
	QPen pen(Qt::white, qMax(1.4, size * 0.14), Qt::SolidLine, Qt::RoundCap);
	p.setPen(pen);
	const qreal m = size * 0.30;
	p.drawLine(QPointF(m, m), QPointF(size - m, size - m));
	p.drawLine(QPointF(size - m, m), QPointF(m, size - m));
	cache.insert(size, pix);
	return pix;
}

void ChatDock::ApplyDeletedStyleToRow(QWidget *row, QListWidgetItem *item, const QString &tooltip)
{
	if (!row || !item)
		return;
	if (item->data(Qt::UserRole + 2).toBool())
		return;
	item->setData(Qt::UserRole + 2, true);

	row->setStyleSheet(QStringLiteral(
		"QFrame#chatMsgRow { background: #141820; border-radius: 6px; border: 1px solid #4a2a2a; }"));

	if (auto *body = row->findChild<QWidget *>(QStringLiteral("chatMsgBody"))) {
		body->setStyleSheet(
			QStringLiteral("background: transparent; color: #7a7a7a; border: none; padding: 0; margin: 0;"));
		auto *fade = new QGraphicsOpacityEffect(body);
		fade->setOpacity(0.55);
		body->setGraphicsEffect(fade);
		const auto labels = body->findChildren<QLabel *>();
		QList<QLabel *> strike = labels;
		if (auto *lab = qobject_cast<QLabel *>(body))
			strike.append(lab);
		for (QLabel *lab : strike) {
			QFont f = lab->font();
			f.setStrikeOut(true);
			lab->setFont(f);
			lab->setStyleSheet(QStringLiteral(
				"background: transparent; color: #7a7a7a; border: none; padding: 0; margin: 0;"));
		}
	}

	auto *lay = qobject_cast<QHBoxLayout *>(row->layout());
	if (lay && !row->findChild<QLabel *>(QStringLiteral("chatMsgDeletedIcon"))) {
		auto *delIcon = new QLabel(row);
		delIcon->setObjectName(QStringLiteral("chatMsgDeletedIcon"));
		delIcon->setFixedSize(14, 14);
		delIcon->setScaledContents(true);
		delIcon->setAlignment(Qt::AlignCenter);
		delIcon->setPixmap(DeletedMessageIconPixmap(14));
		delIcon->setToolTip(tooltip.isEmpty() ? Tr("ChatDockMessageRemoved") : tooltip);
		delIcon->setCursor(Qt::WhatsThisCursor);
		int insertAt = lay->count();
		for (int i = 0; i < lay->count(); ++i) {
			QWidget *w = lay->itemAt(i) ? lay->itemAt(i)->widget() : nullptr;
			if (w && w->objectName() == QLatin1String("chatMsgActions")) {
				insertAt = i;
				break;
			}
		}
		lay->insertWidget(insertAt, delIcon, 0, Qt::AlignVCenter);
	}

	if (messageList)
		LayoutMessageRow(row, item, messageList->viewport()->width());
}

static bool ItemHasChatId(const QListWidgetItem *item, const QString &want)
{
	if (!item || want.isEmpty())
		return false;
	const QString primary = item->data(Qt::UserRole + 1).toString();
	if (primary == want || primary.compare(want, Qt::CaseInsensitive) == 0)
		return true;
	for (const QString &id : item->data(Qt::UserRole + 6).toStringList()) {
		if (id == want || id.compare(want, Qt::CaseInsensitive) == 0)
			return true;
	}
	return false;
}

static void AddChatIdAlias(QListWidgetItem *item, const QString &id)
{
	if (!item || id.isEmpty())
		return;
	QStringList ids = item->data(Qt::UserRole + 6).toStringList();
	const QString primary = item->data(Qt::UserRole + 1).toString();
	if (!primary.isEmpty() && !ids.contains(primary))
		ids.append(primary);
	if (!ids.contains(id))
		ids.append(id);
	item->setData(Qt::UserRole + 6, ids);
	item->setData(Qt::UserRole + 1, id);
}

bool ChatDock::AdoptYouTubeMessage(const ChatMessage &msg)
{
	if (!messageList || msg.id.isEmpty())
		return false;
	for (int i = 0; i < messageList->count(); ++i) {
		if (ItemHasChatId(messageList->item(i), msg.id))
			return true;
	}
	for (int i = messageList->count() - 1; i >= 0; --i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || item->data(Qt::UserRole).toBool())
			continue;
		if (item->data(Qt::UserRole + 4).toInt() != int(YouTube))
			continue;
		if (item->data(Qt::UserRole + 7).toBool())
			continue;
		bool echo = item->data(Qt::UserRole + 1).toString().startsWith(QLatin1String("local-yt-"));
		if (!echo) {
			for (const QString &id : item->data(Qt::UserRole + 6).toStringList()) {
				if (id.startsWith(QLatin1String("local-yt-"))) {
					echo = true;
					break;
				}
			}
		}
		if (!echo)
			continue;
		auto sameText = [](QString a, const QString &b) {
			a.remove(QChar(0xFE0F));
			QString right = b;
			right.remove(QChar(0xFE0F));
			return a.trimmed() == right.trimmed();
		};
		if (!sameText(item->data(Qt::UserRole + 5).toString(), msg.text))
			continue;
		AddChatIdAlias(item, msg.id);
		item->setData(Qt::UserRole + 7, true);
		seenMessageIds.insert(msg.id);
		if (ytPendingDeletes.contains(msg.id)) {
			ytPendingDeletes.remove(msg.id);
			ApplyDeletedStyleToRow(messageList->itemWidget(item), item, Tr("ChatDockMessageRemoved"));
		}
		return true;
	}
	return false;
}

void ChatDock::MarkMessageDeleted(const QString &messageId, const QString &tooltip)
{
	if (messageId.isEmpty() || !messageList)
		return;
	const QString want = messageId.trimmed();
	bool found = false;
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		if (!ItemHasChatId(item, want))
			continue;
		ApplyDeletedStyleToRow(messageList->itemWidget(item), item, tooltip);
		ytPendingDeletes.remove(want);
		found = true;
	}
	if (!found) {
		ytPendingDeletes.insert(want);
		if (ytPendingDeletes.size() > 400)
			ytPendingDeletes.clear();
		blog(LOG_INFO, "[Stream Suite] Deleted message id not in chat list yet: %s", want.toUtf8().constData());
	}
}

void ChatDock::MarkMessagesDeletedByUser(Platform platform, const QString &userLogin, const QString &tooltip)
{
	if (userLogin.isEmpty() || !messageList)
		return;
	const QString want = userLogin.toLower();
	const QString tip = tooltip.isEmpty() ? Tr("ChatDockMessageRemovedUser").arg(userLogin) : tooltip;
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || item->data(Qt::UserRole).toBool())
			continue;
		if (item->data(Qt::UserRole + 4).toInt() != int(platform))
			continue;
		const QString login = item->data(Qt::UserRole + 3).toString();
		if (login != want && login.toLower() != want)
			continue;
		ApplyDeletedStyleToRow(messageList->itemWidget(item), item, tip);
	}
}

void ChatDock::MarkPlatformMessagesDeleted(Platform platform, const QString &tooltip)
{
	if (!messageList)
		return;
	const QString tip = tooltip.isEmpty() ? Tr("ChatDockChatCleared") : tooltip;
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || item->data(Qt::UserRole).toBool())
			continue;
		if (item->data(Qt::UserRole + 4).toInt() != int(platform))
			continue;
		ApplyDeletedStyleToRow(messageList->itemWidget(item), item, tip);
	}
}

void ChatDock::AppendMessage(const ChatMessage &msg)
{
	if (msg.text.isEmpty())
		return;
	if (!msg.id.isEmpty()) {
		if (seenMessageIds.contains(msg.id))
			return;
		seenMessageIds.insert(msg.id);
		if (seenMessageIds.size() > 2000)
			seenMessageIds.clear();
	}

	const QString nameColor = msg.system ? QStringLiteral("#9a9a9a")
					     : (msg.color.isEmpty() ? PlatformAccent(msg.platform).name() : msg.color);

	/* Image badges (Twitch CDN / Kick+YouTube role SVGs) live in their own QLabels — never letter pills. */
	const QString prefixHtml =
		QStringLiteral("<span style='color:%1;font-weight:700;'>%2</span><span style='color:#e6e6e6;'>: </span>")
			.arg(nameColor, EscapeHtml(msg.user));
	const bool kickEmotes = !msg.system && msg.platform == Kick && msg.text.contains(QLatin1String("[emote:"));
	const bool twitchEmotes = !msg.system && msg.platform == Twitch && !msg.emotes.isEmpty();

	auto *item = new QListWidgetItem(messageList);
	item->setData(Qt::UserRole, msg.system);
	item->setData(Qt::UserRole + 1, msg.id);
	item->setData(Qt::UserRole + 2, false); /* deleted */
	item->setData(Qt::UserRole + 3,
		      msg.userLogin.isEmpty() ? msg.user.toLower() : msg.userLogin.toLower());
	item->setData(Qt::UserRole + 4, int(msg.platform));
	item->setData(Qt::UserRole + 5, msg.text);
	item->setData(Qt::UserRole + 7, msg.echo);
	item->setData(Qt::UserRole + 8, msg.actorId.isEmpty() ? msg.userLogin : msg.actorId);
	item->setData(Qt::UserRole + 9, msg.user);
	item->setData(Qt::UserRole + 10, msg.ts);

	auto *row = new QFrame(messageList);
	row->setObjectName(QStringLiteral("chatMsgRow"));
	row->setAttribute(Qt::WA_StyledBackground, true);
	row->setStyleSheet(QStringLiteral(
		"QFrame#chatMsgRow { background: #1b2230; border-radius: 6px; border: none; }"));

	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(10, 8, 12, 8);
	layout->setSpacing(8);

	auto *icon = new QLabel(row);
	icon->setObjectName(QStringLiteral("chatMsgIcon"));
	icon->setFixedSize(18, 18);
	icon->setScaledContents(true);
	icon->setAlignment(Qt::AlignCenter);
	if (msg.showPlatformIcon)
		icon->setPixmap(PlatformIcon(msg.platform).pixmap(18, 18));
	layout->addWidget(icon, 0, Qt::AlignTop);

	for (const ChatBadge &badge : msg.badges) {
		if (badge.url.isEmpty())
			continue;
		auto *badgeLabel = new QLabel(row);
		badgeLabel->setFixedSize(14, 14);
		badgeLabel->setScaledContents(true);
		badgeLabel->setAlignment(Qt::AlignCenter);
		badgeLabel->setToolTip(badge.label.isEmpty() ? badge.key : badge.label);
		ApplyBadgeToLabel(badge, badgeLabel);
		layout->addWidget(badgeLabel, 0, Qt::AlignTop);
	}

	QWidget *body = nullptr;
	if (twitchEmotes) {
		/* Flow + QMovie so animated Twitch GIFs actually play. */
		body = BuildTwitchEmoteBody(msg, prefixHtml, row);
	} else if (kickEmotes) {
		/* Same path for Kick — HTML <img> only showed the first GIF frame. */
		body = BuildKickEmoteBody(msg, prefixHtml, row);
	} else if (!msg.runs.isEmpty()) {
		/* YouTube site feed: text runs plus the real emoji image URL. */
		auto *flow = new ChatFlowBody(row);
		flow->addHtml(prefixHtml);
		auto addText = [&](const QString &text) {
			for (const EmojiRun &run : SplitEmojiRuns(text)) {
				if (run.emoji) {
					QLabel *slot = flow->addEmoteSlot(22);
					slot->setToolTip(run.text);
					const QPixmap local = RenderEmojiPixmap(run.text, 22);
					if (!local.isNull())
						slot->setPixmap(local);
					if (!run.url.isEmpty())
						FetchImageToLabel(run.url, slot, 22, run.url);
					continue;
				}
				const QStringList parts = run.text.split(QRegularExpression(QStringLiteral("(\\s+)")));
				for (const QString &part : parts) {
					if (part.isEmpty())
						continue;
					if (part.startsWith(QLatin1String("http://")) ||
					    part.startsWith(QLatin1String("https://")))
						flow->addHtml(Linkify(EscapeHtml(part)));
					else
						flow->addPlain(part);
				}
			}
		};
		for (const ChatRun &run : msg.runs) {
			if (!run.imageUrl.isEmpty()) {
				QLabel *slot = flow->addEmoteSlot(22);
				if (!run.text.isEmpty())
					slot->setToolTip(run.text);
				const QPixmap local = RenderEmojiPixmap(run.text, 22);
				if (!local.isNull())
					slot->setPixmap(local);
				FetchImageToLabel(run.imageUrl, slot, 22, run.imageUrl);
				continue;
			}
			addText(run.text);
		}
		body = flow;
	} else {
		/* Emoji are images (Twemoji). Qt text + the OBS theme color drops color emoji. */
		auto *flow = new ChatFlowBody(row);
		flow->addHtml(prefixHtml);
		for (const EmojiRun &run : SplitEmojiRuns(msg.text)) {
			if (run.emoji) {
				QLabel *slot = flow->addEmoteSlot(22);
				slot->setToolTip(run.text);
				const QPixmap local = RenderEmojiPixmap(run.text, 22);
				if (!local.isNull())
					slot->setPixmap(local);
				if (!run.url.isEmpty())
					FetchImageToLabel(run.url, slot, 22, run.url);
				continue;
			}
			const QStringList parts = run.text.split(QRegularExpression(QStringLiteral("(\\s+)")));
			for (const QString &part : parts) {
				if (part.isEmpty())
					continue;
				if (part.startsWith(QLatin1String("http://")) || part.startsWith(QLatin1String("https://")))
					flow->addHtml(Linkify(EscapeHtml(part)));
				else
					flow->addPlain(part);
			}
		}
		body = flow;
	}
	layout->addWidget(body, 1, Qt::AlignTop);

	if (!msg.system) {
		auto *actions = new ChatActionsButton(row);
		actions->setToolTip(Tr("ChatDockActions"));
		layout->setAlignment(body, Qt::AlignVCenter);
		layout->addWidget(actions, 0, Qt::AlignVCenter);
		actions->onClick = [this, item, actions] {
			if (!messageList)
				return;
			for (int i = 0; i < messageList->count(); ++i) {
				if (messageList->item(i) == item) {
					ShowMessageActions(item, actions);
					return;
				}
			}
		};
	}

	messageList->addItem(item);
	messageList->setItemWidget(item, row);
	LayoutMessageRow(row, item, messageList->viewport()->width());
	if (!msg.id.isEmpty() && ytPendingDeletes.contains(msg.id)) {
		ytPendingDeletes.remove(msg.id);
		ApplyDeletedStyleToRow(row, item, Tr("ChatDockMessageRemoved"));
	}

	while (messageList->count() > maxMessages) {
		QListWidgetItem *old = messageList->takeItem(0);
		if (old) {
			const QString id = old->data(Qt::UserRole + 1).toString();
			if (!id.isEmpty())
				seenMessageIds.remove(id);
			delete old;
		}
	}
	messageList->scrollToBottom();
}

void ChatDock::SendChatMessage()
{
	const QString text = messageEdit->text().trimmed();
	if (text.isEmpty())
		return;
	messageEdit->clear();
	switch (sendPlatform) {
	case YouTube:
		SendYouTubeMessage(text);
		break;
	case Kick:
		SendKickMessage(text);
		break;
	case Twitch:
	default:
		SendTwitchMessage(text);
		break;
	}
}
