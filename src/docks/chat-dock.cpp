#include "chat-dock.hpp"
#include <src/utils/icon.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QAbstractItemView>
#include <QBuffer>
#include <QColor>
#include <QDateTime>
#include <QDockWidget>
#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
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
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

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

} // namespace
#include <util/base.h>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <cmath>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#endif

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
	youtubeDiscoverTimer->setInterval(30000);
	connect(youtubeDiscoverTimer, &QTimer::timeout, this, [this] {
		if (!connectionsActive || !availablePlatforms.contains(YouTube))
			return;
		if (ytLiveChatId.isEmpty())
			StartYouTubeChat();
	});

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
			if (verbBytes != "GET") {
				curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, verbBytes.constData());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.constData());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
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
	StopConnections();
	connectionsActive = true;
	EnsurePlatformConnections();
	RefreshViewers();
}

void ChatDock::StopConnections()
{
	viewerTimer->stop();
	youtubeTimer->stop();
	youtubeDiscoverTimer->stop();
	kickReconnectTimer->stop();
	twitchReconnectTimer->stop();
	DisconnectTwitchIrc();
	DisconnectKickChat();
	ytLiveChatId.clear();
	ytVideoId.clear();
	ytNextPage.clear();
	kickSlug.clear();
	kickChatroomId.clear();
	kickChannelId.clear();
	kickRealtimeClientId.clear();
	connectionsActive = false;
	twitchConnectedAnnounced = false;
	kickConnectedAnnounced = false;
	youtubeConnectedAnnounced = false;
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
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok) {
			SetViewerCount(YouTube, 0, false);
			return;
		}
		auto applyVideoId = [this](const QString &videoId) {
			if (videoId.isEmpty()) {
				SetViewerCount(YouTube, 0, false);
				return;
			}
			if (ytVideoId.isEmpty())
				ytVideoId = videoId;
			const Account &acc = accounts[YouTube];
			const QString url =
				QStringLiteral("https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=") +
				videoId;
			Http(url,
			     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {},
			     "GET", [this](int status, const QByteArray &body, const QString &) {
				     if (status < 200 || status >= 300) {
					     SetViewerCount(YouTube, 0, false);
					     return;
				     }
				     const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
				     if (items.isEmpty()) {
					     SetViewerCount(YouTube, 0, false);
					     return;
				     }
				     const QString viewers = items.at(0)
								    .toObject()
								    .value("liveStreamingDetails")
								    .toObject()
								    .value("concurrentViewers")
								    .toString();
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
		const Account &acc = accounts[YouTube];
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "id,snippet,status");
		query.addQueryItem("broadcastStatus", "active");
		query.addQueryItem("broadcastType", "all");
		query.addQueryItem("maxResults", "1");
		url.setQuery(query);
		Http(url.toString(),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {}, "GET",
		     [this, applyVideoId](int status, const QByteArray &body, const QString &) {
			     if (status < 200 || status >= 300) {
				     SetViewerCount(YouTube, 0, false);
				     return;
			     }
			     const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
			     if (items.isEmpty()) {
				     SetViewerCount(YouTube, 0, false);
				     return;
			     }
			     const QJsonObject item = items.at(0).toObject();
			     const QString id = item.value("id").toString();
			     const QString chatId = item.value("snippet").toObject().value("activeLiveChatId").toString();
			     if (!chatId.isEmpty() && ytLiveChatId.isEmpty())
				     ytLiveChatId = chatId;
			     applyVideoId(id);
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
				     CurlWsSendText(curl, "CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");

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
	AppendMessage(msg);
}

void ChatDock::SendTwitchMessage(const QString &text)
{
	if (!twitchWs || !twitchJoined || twitchLogin.isEmpty())
		return;
	twitchWs->enqueue("PRIVMSG #" + twitchLogin.toUtf8() + " :" + text.toUtf8() + "\r\n");
	ChatMessage msg;
	msg.platform = Twitch;
	msg.user = accounts[Twitch].userName.isEmpty() ? twitchLogin : accounts[Twitch].userName;
	msg.color = QStringLiteral("#9146FF");
	msg.text = text;
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.id = QStringLiteral("local-tw-%1").arg(msg.ts);
	AppendMessage(msg);
}

void ChatDock::StartYouTubeChat()
{
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok) {
			youtubeConnectStarted = false;
			AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(YouTube)), YouTube);
			return;
		}
		const Account &acc = accounts[YouTube];
		auto authHdr = [acc] {
			return QList<QPair<QByteArray, QByteArray>>{
				{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}};
		};
		auto pickBroadcast = [](const QJsonArray &items) -> QJsonObject {
			QJsonObject best;
			int bestScore = -1;
			for (const QJsonValue &row : items) {
				const QJsonObject item = row.toObject();
				const QString life = item.value("status").toObject().value("lifeCycleStatus").toString();
				const bool hasChat = !item.value("snippet").toObject().value("activeLiveChatId").toString().isEmpty();
				int score = 0;
				if (life == QLatin1String("live"))
					score = 50;
				else if (life == QLatin1String("liveStarting"))
					score = 40;
				else if (life == QLatin1String("testing"))
					score = 30;
				else if (life == QLatin1String("ready"))
					score = 20;
				else if (life == QLatin1String("created") || life == QLatin1String("upcoming"))
					score = 10;
				if (hasChat)
					score += 100;
				if (score > bestScore) {
					bestScore = score;
					best = item;
				}
			}
			return best;
		};
		auto startPollOrResolve = [this, authHdr](QString videoId, QString chatId) {
			ytVideoId = videoId;
			ytLiveChatId = chatId;
			auto begin = [this] {
				if (ytLiveChatId.isEmpty()) {
					if (!youtubeConnectedAnnounced)
						AppendSystemMessage(Tr("ChatDockYouTubeWaiting"), YouTube);
					return;
				}
				ytNextPage.clear();
				PollYouTubeChat();
			};
			if (!ytLiveChatId.isEmpty() || ytVideoId.isEmpty()) {
				begin();
				return;
			}
			Http(QStringLiteral("https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=") +
				     ytVideoId,
			     authHdr(), {}, "GET", [this, begin](int st, const QByteArray &payload, const QString &) {
				     if (st >= 200 && st < 300) {
					     const QJsonArray vids =
						     QJsonDocument::fromJson(payload).object().value("items").toArray();
					     if (!vids.isEmpty())
						     ytLiveChatId = vids.at(0)
									    .toObject()
									    .value("liveStreamingDetails")
									    .toObject()
									    .value("activeLiveChatId")
									    .toString();
				     }
				     begin();
			     });
		};
		auto trySearchLive = [this, authHdr, startPollOrResolve] {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/search"));
			QUrlQuery query;
			query.addQueryItem("part", "snippet");
			query.addQueryItem("forMine", "true");
			query.addQueryItem("type", "video");
			query.addQueryItem("eventType", "live");
			query.addQueryItem("maxResults", "5");
			url.setQuery(query);
			Http(url.toString(), authHdr(), {}, "GET",
			     [this, startPollOrResolve](int code, const QByteArray &body, const QString &) {
				     QString videoId;
				     if (code >= 200 && code < 300) {
					     const QJsonArray items =
						     QJsonDocument::fromJson(body).object().value("items").toArray();
					     if (!items.isEmpty())
						     videoId = items.at(0).toObject().value("id").toObject().value("videoId").toString();
				     }
				     if (videoId.isEmpty()) {
					     if (!youtubeConnectedAnnounced)
						     AppendSystemMessage(Tr("ChatDockYouTubeWaiting"), YouTube);
					     return;
				     }
				     startPollOrResolve(videoId, QString());
			     });
		};
		auto tryMine = [this, authHdr, pickBroadcast, startPollOrResolve, trySearchLive] {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
			QUrlQuery query;
			query.addQueryItem("part", "snippet,contentDetails,status");
			query.addQueryItem("mine", "true");
			query.addQueryItem("broadcastType", "all");
			query.addQueryItem("maxResults", "50");
			url.setQuery(query);
			Http(url.toString(), authHdr(), {}, "GET",
			     [pickBroadcast, startPollOrResolve, trySearchLive](int code, const QByteArray &body, const QString &) {
				     if (code >= 200 && code < 300) {
					     const QJsonObject best =
						     pickBroadcast(QJsonDocument::fromJson(body).object().value("items").toArray());
					     if (!best.isEmpty()) {
						     startPollOrResolve(best.value("id").toString(),
									best.value("snippet").toObject().value("activeLiveChatId").toString());
						     return;
					     }
				     }
				     trySearchLive();
			     });
		};
		auto tryStatus = [this, authHdr, pickBroadcast, startPollOrResolve](const QString &status,
										     std::function<void()> next) {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
			QUrlQuery query;
			query.addQueryItem("part", "snippet,contentDetails,status");
			query.addQueryItem("broadcastStatus", status);
			query.addQueryItem("broadcastType", "all");
			query.addQueryItem("maxResults", "25");
			url.setQuery(query);
			Http(url.toString(), authHdr(), {}, "GET",
			     [pickBroadcast, startPollOrResolve, next](int code, const QByteArray &body, const QString &) {
				     if (code >= 200 && code < 300) {
					     const QJsonObject best =
						     pickBroadcast(QJsonDocument::fromJson(body).object().value("items").toArray());
					     if (!best.isEmpty()) {
						     startPollOrResolve(best.value("id").toString(),
									best.value("snippet").toObject().value("activeLiveChatId").toString());
						     return;
					     }
				     } else if (code == 401 || code == 403) {
					     blog(LOG_WARNING, "[Stream Suite] YouTube liveBroadcasts HTTP %d", code);
				     }
				     next();
			     });
		};
		tryStatus(QStringLiteral("active"), [this, tryStatus, tryMine] {
			tryStatus(QStringLiteral("upcoming"), tryMine);
		});
	});
}

void ChatDock::PollYouTubeChat()
{
	if (ytLiveChatId.isEmpty())
		return;
	EnsureToken(YouTube, [this](bool ok) {
		if (!ok || ytLiveChatId.isEmpty()) {
			if (!ok)
				blog(LOG_WARNING, "[Stream Suite] YouTube chat poll: token failed");
			return;
		}
		const Account &acc = accounts[YouTube];
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChatMessages"));
		QUrlQuery query;
		query.addQueryItem("liveChatId", ytLiveChatId);
		query.addQueryItem("part", "snippet,authorDetails");
		query.addQueryItem("maxResults", "200");
		if (!ytNextPage.isEmpty())
			query.addQueryItem("pageToken", ytNextPage);
		url.setQuery(query);
		Http(url.toString(),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()}}, {}, "GET",
		     [this](int status, const QByteArray &body, const QString &error) {
			     int interval = 5000;
			     if (status >= 200 && status < 300) {
				     const QJsonObject root = QJsonDocument::fromJson(body).object();
				     if (root.contains(QStringLiteral("offlineAt"))) {
					     SetViewerCount(YouTube, 0, false);
					     ytLiveChatId.clear();
					     ytNextPage.clear();
					     blog(LOG_INFO, "[Stream Suite] YouTube live chat went offline");
					     return;
				     }
				     ytNextPage = root.value("nextPageToken").toString();
				     interval = qMax(2000, root.value("pollingIntervalMillis").toInt(5000));
				     if (!youtubeConnectedAnnounced) {
					     youtubeConnectedAnnounced = true;
					     AppendSystemMessage(Tr("ChatDockConnected").arg(PlatformLabel(YouTube)), YouTube);
				     }
				     const QJsonArray items = root.value("items").toArray();
				     for (const QJsonValue &row : items) {
					     const QJsonObject obj = row.toObject();
					     const QJsonObject snip = obj.value("snippet").toObject();
					     const QString type = snip.value("type").toString();
					     if (type == QLatin1String("tombstone")) {
						     const QString tombId = obj.value(QStringLiteral("id")).toString();
						     if (!tombId.isEmpty())
							     MarkMessageDeleted(tombId, Tr("ChatDockMessageRemoved"));
						     continue;
					     }
					     if (type == QLatin1String("messageDeletedEvent")) {
						     QString deletedId =
							     snip.value(QStringLiteral("messageDeletedDetails"))
								     .toObject()
								     .value(QStringLiteral("deletedMessageId"))
								     .toString();
						     if (deletedId.isEmpty())
							     deletedId = obj.value(QStringLiteral("id")).toString();
						     if (!deletedId.isEmpty())
							     MarkMessageDeleted(deletedId, Tr("ChatDockMessageRemoved"));
						     continue;
					     }
					     QString text =
						     snip.value("textMessageDetails").toObject().value("messageText").toString();
					     if (text.isEmpty())
						     text = snip.value("displayMessage").toString();
					     /* Skip empty / pure membership noise without display text. */
					     if (text.isEmpty())
						     continue;
					     if (!type.isEmpty() && type != QLatin1String("textMessageEvent") &&
						 type != QLatin1String("superChatEvent") &&
						 type != QLatin1String("superStickerEvent") &&
						 type != QLatin1String("membershipGiftingEvent") &&
						 type != QLatin1String("giftMembershipReceivedEvent") &&
						 type != QLatin1String("newSponsorEvent") &&
						 type != QLatin1String("memberMilestoneChatEvent"))
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
					     if (author.value("isChatOwner").toBool())
						     msg.badges.append({QStringLiteral("owner"), QString(), QStringLiteral("Owner")});
					     if (author.value("isChatModerator").toBool())
						     msg.badges.append({QStringLiteral("mod"), QString(), QStringLiteral("Mod")});
					     if (author.value("isChatSponsor").toBool())
						     msg.badges.append(
							     {QStringLiteral("member"), QString(), QStringLiteral("Member")});
					     if (author.value("isVerified").toBool())
						     msg.badges.append(
							     {QStringLiteral("verified"), QString(), QStringLiteral("Verified")});
					     AppendMessage(msg);
				     }
			     } else {
				     blog(LOG_WARNING, "[Stream Suite] YouTube chat poll HTTP %d: %s", status,
					  error.isEmpty() ? body.left(200).constData() : error.toUtf8().constData());
				     if (status == 401 || status == 403) {
					     youtubeConnectStarted = false;
					     AppendSystemMessage(Tr("ChatDockAuthFailed").arg(PlatformLabel(YouTube)), YouTube);
					     return;
				     }
				     interval = 8000;
			     }
			     if (connectionsActive && !ytLiveChatId.isEmpty())
				     youtubeTimer->start(interval);
		     });
	});
}

void ChatDock::SendYouTubeMessage(const QString &text)
{
	if (ytLiveChatId.isEmpty())
		return;
	EnsureToken(YouTube, [this, text](bool ok) {
		if (!ok || ytLiveChatId.isEmpty())
			return;
		const Account &acc = accounts[YouTube];
		QJsonObject snip;
		snip.insert("liveChatId", ytLiveChatId);
		snip.insert("type", "textMessageEvent");
		QJsonObject details;
		details.insert("messageText", text);
		snip.insert("textMessageDetails", details);
		QJsonObject body;
		body.insert("snippet", snip);
		Http(QStringLiteral("https://www.googleapis.com/youtube/v3/liveChatMessages?part=snippet"),
		     {{QByteArrayLiteral("Authorization"), QByteArrayLiteral("Bearer ") + acc.accessToken.toUtf8()},
		      {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")}},
		     QJsonDocument(body).toJson(QJsonDocument::Compact), "POST",
		     [this, text](int status, const QByteArray &, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     ChatMessage msg;
			     msg.platform = YouTube;
			     msg.user = accounts[YouTube].userName.isEmpty() ? QStringLiteral("You") : accounts[YouTube].userName;
			     msg.color = QStringLiteral("#FF5555");
			     msg.text = text;
			     msg.ts = QDateTime::currentMSecsSinceEpoch();
			     msg.id = QStringLiteral("local-yt-%1").arg(msg.ts);
			     AppendMessage(msg);
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
		eventName = data.value(QStringLiteral("type")).toString(data.value(QStringLiteral("event")).toString());
		if (data.contains(QStringLiteral("data")) && !data.contains(QStringLiteral("content")) &&
		    !data.contains(QStringLiteral("sender"))) {
			const QJsonValue inner = data.value(QStringLiteral("data"));
			if (inner.isObject())
				data = inner.toObject();
			else if (inner.isString())
				data = QJsonDocument::fromJson(inner.toString().toUtf8()).object();
		}
	} else if (dataVal.isString()) {
		data = QJsonDocument::fromJson(dataVal.toString().toUtf8()).object();
		eventName = data.value(QStringLiteral("type")).toString(data.value(QStringLiteral("event")).toString());
	}
	HandleKickChatPayload(data, eventName);
}

void ChatDock::HandleKickChatPayload(const QJsonObject &data, const QString &eventName)
{
	if (data.isEmpty())
		return;
	if (eventName.contains(QStringLiteral("StopStreamBroadcast")) ||
	    data.value(QStringLiteral("type")).toString().contains(QStringLiteral("StopStreamBroadcast"))) {
		OnKickStreamOffline();
		return;
	}

	const QString type = data.value(QStringLiteral("type")).toString();
	if (eventName.contains(QStringLiteral("MessageDeleted")) ||
	    eventName.contains(QStringLiteral("ChatMessageDeleted")) || type == QLatin1String("message_deleted")) {
		QString msgId = JsonId(data.value(QStringLiteral("message_id")));
		if (msgId.isEmpty())
			msgId = JsonId(data.value(QStringLiteral("id")));
		if (msgId.isEmpty())
			msgId = JsonId(data.value(QStringLiteral("message")).toObject().value(QStringLiteral("id")));
		if (!msgId.isEmpty())
			MarkMessageDeleted(msgId, Tr("ChatDockMessageRemoved"));
		return;
	}
	if (eventName.contains(QStringLiteral("ChatroomClear")) || eventName.contains(QStringLiteral("ChatCleared")) ||
	    type == QLatin1String("chatroom_clear") || type == QLatin1String("chat_cleared")) {
		MarkPlatformMessagesDeleted(Kick, Tr("ChatDockChatCleared"));
		AppendSystemMessage(Tr("ChatDockChatCleared"), Kick);
		return;
	}
	if (eventName.contains(QStringLiteral("UserBanned"))) {
		const QString user = data.value(QStringLiteral("username"))
					     .toString(data.value(QStringLiteral("user"))
							       .toObject()
							       .value(QStringLiteral("username"))
							       .toString());
		if (!user.isEmpty()) {
			const QString tip = Tr("ChatDockMessageRemovedUser").arg(user);
			MarkMessagesDeletedByUser(Kick, user, tip);
			AppendSystemMessage(tip, Kick);
		}
		return;
	}

	const bool looksLikeChat = data.contains(QStringLiteral("content")) || data.contains(QStringLiteral("sender")) ||
				   eventName.contains(QStringLiteral("ChatMessage"));
	if (!looksLikeChat)
		return;
	if (!eventName.isEmpty() && !eventName.contains(QStringLiteral("ChatMessage")) &&
	    type != QLatin1String("message") && type != QLatin1String("reply") &&
	    !data.contains(QStringLiteral("content")))
		return;

	const QJsonObject sender = data.value(QStringLiteral("sender")).toObject();
	const QJsonObject identity = sender.value(QStringLiteral("identity")).toObject();
	ChatMessage msg;
	msg.platform = Kick;
	msg.id = JsonId(data.value(QStringLiteral("id")));
	if (msg.id.isEmpty())
		msg.id = JsonId(data.value(QStringLiteral("message_id")));
	msg.user = sender.value(QStringLiteral("username")).toString(sender.value(QStringLiteral("slug")).toString());
	msg.userLogin = sender.value(QStringLiteral("slug")).toString(msg.user).toLower();
	if (msg.user.isEmpty())
		msg.user = QStringLiteral("Kick");
	msg.color = identity.value(QStringLiteral("color")).toString(QStringLiteral("#53FC18"));
	msg.text = data.value(QStringLiteral("content")).toString(data.value(QStringLiteral("message")).toString());
	msg.ts = QDateTime::currentMSecsSinceEpoch();
	msg.badges = KickBadgesFromIdentity(identity);
	if (msg.text.isEmpty())
		return;
	if (msg.id.isEmpty())
		msg.id = QStringLiteral("kick-%1-%2").arg(msg.ts).arg(msg.user);
	AppendMessage(msg);
}

void ChatDock::OnKickStreamOffline()
{
	SetViewerCount(Kick, 0, false);
}

void ChatDock::OnKickWsMessage(const QString &message)
{
	const QJsonObject root = QJsonDocument::fromJson(message.toUtf8()).object();
	if (root.contains(QStringLiteral("push"))) {
		HandleKickCentrifugoFrame(message.toUtf8());
		return;
	}
	const QString event = root.value(QStringLiteral("event")).toString();
	if (event.contains(QStringLiteral("StopStreamBroadcast"))) {
		OnKickStreamOffline();
		return;
	}
	if (!event.contains(QStringLiteral("ChatMessage")) && !event.contains(QStringLiteral("MessageDeleted")) &&
	    !event.contains(QStringLiteral("ChatroomClear")) && !event.contains(QStringLiteral("UserBanned")))
		return;
	QJsonObject data = root.value(QStringLiteral("data")).toObject();
	if (data.isEmpty())
		data = QJsonDocument::fromJson(root.value(QStringLiteral("data")).toString().toUtf8()).object();
	HandleKickChatPayload(data, event);
}

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
		     [this, text](int status, const QByteArray &, const QString &) {
			     if (status < 200 || status >= 300)
				     return;
			     ChatMessage msg;
			     msg.platform = Kick;
			     msg.user = accounts[Kick].userName.isEmpty() ? QStringLiteral("You") : accounts[Kick].userName;
			     msg.color = QStringLiteral("#53FC18");
			     msg.text = text;
			     msg.ts = QDateTime::currentMSecsSinceEpoch();
			     msg.id = QStringLiteral("local-kick-%1").arg(msg.ts);
			     AppendMessage(msg);
		     });
	});
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

QString ChatDock::FormatTwitchContentHtml(const QString &raw, const QVector<ChatEmoteSpan> &emotes) const
{
	if (emotes.isEmpty())
		return Linkify(EscapeHtml(raw));
	QString out;
	int last = 0;
	for (const ChatEmoteSpan &span : emotes) {
		if (span.start < last || span.end >= raw.size() || span.start > span.end)
			continue;
		if (span.start > last)
			out += Linkify(EscapeHtml(raw.mid(last, span.start - last)));
		const QString cacheKey = QStringLiteral("twitch-emote:%1").arg(span.id);
		const QString name = raw.mid(span.start, span.end - span.start + 1);
		if (badgePixmaps.contains(cacheKey)) {
			out += QStringLiteral("<img src='%1' height='18' style='vertical-align:middle;' />")
				       .arg(PixmapDataUri(badgePixmaps.value(cacheKey)));
		} else {
			out += QStringLiteral("<span style='color:#9a9a9a;'>:%1:</span>").arg(EscapeHtml(name));
		}
		last = span.end + 1;
	}
	if (last < raw.size())
		out += Linkify(EscapeHtml(raw.mid(last)));
	return out;
}

void ChatDock::RefreshTwitchMessageLabel(QLabel *label)
{
	if (!label)
		return;
	const QString raw = label->property("twitchRawText").toString();
	const QString prefix = label->property("twitchPrefixHtml").toString();
	if (raw.isEmpty())
		return;
	const QVector<ChatEmoteSpan> spans = TwitchEmotesFromTag(label->property("twitchEmotesTag").toString());
	label->setText(prefix + FormatTwitchContentHtml(raw, spans));
	if (auto *row = qobject_cast<QWidget *>(label->parent())) {
		for (int i = 0; messageList && i < messageList->count(); ++i) {
			QListWidgetItem *it = messageList->item(i);
			if (messageList->itemWidget(it) == row) {
				LayoutMessageRow(row, it, messageList->viewport()->width());
				break;
			}
		}
	}
}

void ChatDock::PrefetchTwitchEmotes(const QVector<ChatEmoteSpan> &emotes, QLabel *label)
{
	QSet<QString> ids;
	for (const ChatEmoteSpan &span : emotes)
		ids.insert(span.id);
	if (ids.isEmpty())
		return;

	QPointer<QLabel> alive(label);
	for (const QString &id : ids) {
		const QString cacheKey = QStringLiteral("twitch-emote:%1").arg(id);
		if (badgePixmaps.contains(cacheKey) || badgeFetchPending.contains(cacheKey))
			continue;
		badgeFetchPending.insert(cacheKey);
		const QString url = TwitchEmoteImageUrl(id);
		Http(url,
		     {{QByteArrayLiteral("User-Agent"),
		       QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
					 "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")}},
		     {}, "GET", [this, cacheKey, alive](int status, const QByteArray &body, const QString &) {
			     badgeFetchPending.remove(cacheKey);
			     if (status < 200 || status >= 300 || body.isEmpty())
				     return;
			     badgeRawBytes.insert(cacheKey, body);
			     QPixmap pix;
			     if (!pix.loadFromData(body))
				     return;
			     badgePixmaps.insert(cacheKey,
						 pix.scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
			     if (alive)
				     RefreshTwitchMessageLabel(alive);
		     });
	}
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
		QStringLiteral("sub_gifter"),
	};
	if (!kKnown.contains(t))
		return {};
	if (t == QLatin1String("subscriber") && count > 0)
		return QStringLiteral("kick-role:subscriber:%1").arg(count);
	return QStringLiteral("kick-role:%1").arg(t);
}

QPixmap ChatDock::KickRoleBadgePixmap(const QString &role, int size)
{
	QString r = role.trimmed().toLower();
	if (r.startsWith(QLatin1String("subscriber:")))
		r = QStringLiteral("subscriber");
	const QString cacheKey = r + QLatin1Char('@') + QString::number(size);
	static QHash<QString, QPixmap> cache;
	if (cache.contains(cacheKey))
		return cache.value(cacheKey);

	QPixmap pix(size, size);
	pix.fill(Qt::transparent);
	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);

	auto fillRounded = [&](const QColor &c) {
		p.setPen(Qt::NoPen);
		p.setBrush(c);
		p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
	};

	if (r == QLatin1String("broadcaster")) {
		/* Kick host badge: purple tile + white mic. */
		fillRounded(QColor(0x8B, 0x5C, 0xF6));
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		const qreal cx = size / 2.0;
		const qreal micW = size * 0.22;
		const qreal micH = size * 0.34;
		const qreal micTop = size * 0.16;
		p.drawRoundedRect(QRectF(cx - micW / 2.0, micTop, micW, micH), micW / 2.0, micW / 2.0);
		QPen pen(Qt::white, qMax(1.2, size * 0.09));
		pen.setCapStyle(Qt::RoundCap);
		pen.setJoinStyle(Qt::RoundJoin);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawArc(QRectF(cx - size * 0.26, micTop + micH * 0.28, size * 0.52, size * 0.40), 0, -180 * 16);
		p.drawLine(QPointF(cx, micTop + micH * 0.72), QPointF(cx, size * 0.80));
		p.drawLine(QPointF(cx - size * 0.13, size * 0.80), QPointF(cx + size * 0.13, size * 0.80));
	} else if (r == QLatin1String("moderator")) {
		fillRounded(QColor(0x00, 0xC9, 0x7B));
		p.setPen(QPen(Qt::white, qMax(1.2, size * 0.10), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		const qreal m = size * 0.22;
		p.drawLine(QPointF(m, size - m), QPointF(size - m, m));
		p.drawLine(QPointF(size - m, m), QPointF(size - m * 1.35, m * 1.55));
		p.drawLine(QPointF(size - m, m), QPointF(size - m * 1.55, m * 1.35));
	} else if (r == QLatin1String("vip")) {
		fillRounded(QColor(0xE9, 0x1E, 0x63));
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		QPainterPath diamond;
		const qreal cx = size / 2.0;
		const qreal cy = size / 2.0;
		const qreal s = size * 0.28;
		diamond.moveTo(cx, cy - s);
		diamond.lineTo(cx + s, cy);
		diamond.lineTo(cx, cy + s);
		diamond.lineTo(cx - s, cy);
		diamond.closeSubpath();
		p.drawPath(diamond);
	} else if (r == QLatin1String("og")) {
		fillRounded(QColor(0xFF, 0xB3, 0x00));
		p.setPen(Qt::white);
		QFont f = p.font();
		f.setBold(true);
		f.setPixelSize(qMax(7, int(size * 0.42)));
		p.setFont(f);
		p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QStringLiteral("OG"));
	} else if (r == QLatin1String("verified")) {
		fillRounded(QColor(0x1D, 0x9B, 0xF0));
		p.setPen(QPen(Qt::white, qMax(1.2, size * 0.12), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.setBrush(Qt::NoBrush);
		p.drawLine(QPointF(size * 0.22, size * 0.52), QPointF(size * 0.42, size * 0.72));
		p.drawLine(QPointF(size * 0.42, size * 0.72), QPointF(size * 0.78, size * 0.30));
	} else if (r == QLatin1String("staff")) {
		fillRounded(QColor(0x53, 0xFC, 0x18));
		p.setPen(Qt::black);
		QFont f = p.font();
		f.setBold(true);
		f.setPixelSize(qMax(7, int(size * 0.55)));
		p.setFont(f);
		p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QStringLiteral("K"));
	} else if (r == QLatin1String("founder")) {
		fillRounded(QColor(0xF5, 0xA6, 0x23));
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		QPainterPath star;
		const qreal cx = size / 2.0;
		const qreal cy = size / 2.0;
		const qreal outer = size * 0.32;
		const qreal inner = size * 0.14;
		for (int i = 0; i < 10; ++i) {
			const qreal ang = -M_PI / 2.0 + i * M_PI / 5.0;
			const qreal rad = (i % 2 == 0) ? outer : inner;
			const QPointF pt(cx + std::cos(ang) * rad, cy + std::sin(ang) * rad);
			if (i == 0)
				star.moveTo(pt);
			else
				star.lineTo(pt);
		}
		star.closeSubpath();
		p.drawPath(star);
	} else if (r == QLatin1String("sidekick")) {
		fillRounded(QColor(0x00, 0xB8, 0xD4));
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawEllipse(QRectF(size * 0.28, size * 0.18, size * 0.44, size * 0.44));
		p.drawRoundedRect(QRectF(size * 0.22, size * 0.55, size * 0.56, size * 0.28), size * 0.14,
				   size * 0.14);
	} else if (r == QLatin1String("subscriber") || r == QLatin1String("sub_gifter")) {
		fillRounded(r == QLatin1String("sub_gifter") ? QColor(0xFF, 0x6D, 0x00) : QColor(0x7C, 0x4D, 0xFF));
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawEllipse(QRectF(size * 0.30, size * 0.22, size * 0.40, size * 0.40));
		p.drawRoundedRect(QRectF(size * 0.20, size * 0.58, size * 0.60, size * 0.26), size * 0.13,
				   size * 0.13);
	} else {
		fillRounded(QColor(0x55, 0x55, 0x55));
	}

	cache.insert(cacheKey, pix);
	return pix;
}

QVector<ChatDock::ChatBadge> ChatDock::KickBadgesFromIdentity(const QJsonObject &identity)
{
	QVector<ChatBadge> out;
	QSet<QString> seenRoles;
	bool hasBroadcaster = false;

	auto normalizeRole = [](QString t) {
		t = t.trimmed().toLower();
		if (t == QLatin1String("host"))
			return QStringLiteral("broadcaster");
		if (t == QLatin1String("mod"))
			return QStringLiteral("moderator");
		return t;
	};

	auto addRole = [&](const QString &type, const QString &label, int count) {
		const QString role = normalizeRole(type);
		if (role.isEmpty() || seenRoles.contains(role))
			return;
		const QString url = KickRoleBadgeKey(role, count);
		if (url.isEmpty())
			return;
		seenRoles.insert(role);
		if (role == QLatin1String("broadcaster"))
			hasBroadcaster = true;
		ChatBadge badge;
		badge.key = role;
		badge.label = label.isEmpty() ? type : label;
		badge.url = url;
		out.append(badge);
	};

	/* Role badges from legacy identity.badges (broadcaster/mod/vip/...). */
	struct LegacyBadge {
		QString type;
		QString label;
		int count = 0;
		int order = 100;
	};
	static const QHash<QString, int> kRoleOrder{
		{QStringLiteral("broadcaster"), 0}, {QStringLiteral("staff"), 1},
		{QStringLiteral("verified"), 2},    {QStringLiteral("moderator"), 3},
		{QStringLiteral("vip"), 4},         {QStringLiteral("og"), 5},
		{QStringLiteral("founder"), 6},     {QStringLiteral("sidekick"), 7},
		{QStringLiteral("sub_gifter"), 8},  {QStringLiteral("subscriber"), 9},
	};
	QVector<LegacyBadge> legacy;
	for (const QJsonValue &row : identity.value(QStringLiteral("badges")).toArray()) {
		const QJsonObject obj = row.toObject();
		LegacyBadge b;
		b.type = normalizeRole(obj.value(QStringLiteral("type"))
					       .toString(obj.value(QStringLiteral("name")).toString()));
		b.label = obj.value(QStringLiteral("text")).toString(b.type);
		b.count = obj.value(QStringLiteral("count")).toInt(obj.value(QStringLiteral("months")).toInt());
		b.order = kRoleOrder.value(b.type, 100);
		if (!b.type.isEmpty())
			legacy.append(b);
	}
	std::sort(legacy.begin(), legacy.end(),
		  [](const LegacyBadge &a, const LegacyBadge &b) { return a.order < b.order; });
	for (const LegacyBadge &b : legacy)
		addRole(b.type, b.label, b.count);

	/*
	 * badges_v2 carries channel/custom images (sub badges, etc).
	 * Broadcaster rows on Kick only show the host mic — skip v2 there so we don't
	 * paint the grey "level" badge (and a duplicate letter pill) next to the name.
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
			if (seenRoles.contains(name) || (!badgeType.isEmpty() && seenRoles.contains(badgeType)))
				continue;
			if (name == QLatin1String("level") || badgeType == QLatin1String("level"))
				continue;
			ChatBadge badge;
			badge.key = name.isEmpty() ? badgeType : name;
			badge.label = obj.value(QStringLiteral("text")).toString(badge.key);
			badge.url = obj.value(QStringLiteral("image_url")).toString();
			if (badge.url.isEmpty())
				continue;
			seenRoles.insert(badge.key.toLower());
			out.append(badge);
		}
	}

	return out;
}

QString ChatDock::KickEmoteImageUrl(const QString &emoteId)
{
	return QStringLiteral("https://files.kick.com/emotes/%1/fullsize").arg(emoteId);
}

QString ChatDock::PixmapDataUri(const QPixmap &pix)
{
	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	pix.save(&buffer, "PNG");
	return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

QString ChatDock::FormatKickContentHtml(const QString &raw) const
{
	static const QRegularExpression re(QStringLiteral(R"(\[emote:(\d+):([^\]]+)\])"));
	QString out;
	int last = 0;
	auto it = re.globalMatch(raw);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		out += Linkify(EscapeHtml(raw.mid(last, m.capturedStart() - last)));
		const QString id = m.captured(1);
		const QString name = m.captured(2);
		const QString cacheKey = QStringLiteral("kick-emote:%1").arg(id);
		if (badgePixmaps.contains(cacheKey)) {
			out += QStringLiteral("<img src='%1' height='18' style='vertical-align:middle;' />")
				       .arg(PixmapDataUri(badgePixmaps.value(cacheKey)));
		} else {
			out += QStringLiteral("<span style='color:#9a9a9a;'>:%1:</span>").arg(EscapeHtml(name));
		}
		last = m.capturedEnd();
	}
	out += Linkify(EscapeHtml(raw.mid(last)));
	return out;
}

void ChatDock::RefreshKickMessageLabel(QLabel *label)
{
	if (!label)
		return;
	const QString raw = label->property("kickRawText").toString();
	const QString prefix = label->property("kickPrefixHtml").toString();
	if (raw.isEmpty())
		return;
	label->setText(prefix + FormatKickContentHtml(raw));
	if (auto *row = qobject_cast<QWidget *>(label->parent())) {
		for (int i = 0; messageList && i < messageList->count(); ++i) {
			QListWidgetItem *it = messageList->item(i);
			if (messageList->itemWidget(it) == row) {
				LayoutMessageRow(row, it, messageList->viewport()->width());
				break;
			}
		}
	}
}

void ChatDock::PrefetchKickEmotes(const QString &raw, QLabel *label)
{
	static const QRegularExpression re(QStringLiteral(R"(\[emote:(\d+):([^\]]+)\])"));
	QSet<QString> ids;
	auto it = re.globalMatch(raw);
	while (it.hasNext())
		ids.insert(it.next().captured(1));
	if (ids.isEmpty())
		return;

	QPointer<QLabel> alive(label);
	for (const QString &id : ids) {
		const QString cacheKey = QStringLiteral("kick-emote:%1").arg(id);
		if (badgePixmaps.contains(cacheKey) || badgeFetchPending.contains(cacheKey))
			continue;
		badgeFetchPending.insert(cacheKey);
		const QString url = KickEmoteImageUrl(id);
		Http(url,
		     {{QByteArrayLiteral("User-Agent"),
		       QByteArrayLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
					 "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")},
		      {QByteArrayLiteral("Referer"), QByteArrayLiteral("https://kick.com/")}},
		     {}, "GET", [this, cacheKey, alive](int status, const QByteArray &body, const QString &) {
			     badgeFetchPending.remove(cacheKey);
			     if (status < 200 || status >= 300 || body.isEmpty())
				     return;
			     QPixmap pix;
			     if (!pix.loadFromData(body))
				     return;
			     badgePixmaps.insert(cacheKey,
						 pix.scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
			     if (alive)
				     RefreshKickMessageLabel(alive);
		     });
	}
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
	FetchBadgePixmap(badge.url, target);
}

void ChatDock::ApplyImageDataToLabel(QLabel *label, const QByteArray &body, int size)
{
	if (!label || body.isEmpty() || size <= 0)
		return;

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
			side += w->objectName() == QLatin1String("chatMsgIcon") ? 18 : 14;
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

	const int contentH = qMax(18, textH);
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
		if (auto *lab = qobject_cast<QLabel *>(body)) {
			QFont f = lab->font();
			f.setStrikeOut(true);
			lab->setFont(f);
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
		lay->addWidget(delIcon, 0, Qt::AlignTop);
	}

	if (messageList)
		LayoutMessageRow(row, item, messageList->viewport()->width());
}

void ChatDock::MarkMessageDeleted(const QString &messageId, const QString &tooltip)
{
	if (messageId.isEmpty() || !messageList)
		return;
	for (int i = 0; i < messageList->count(); ++i) {
		QListWidgetItem *item = messageList->item(i);
		if (!item || item->data(Qt::UserRole + 1).toString() != messageId)
			continue;
		ApplyDeletedStyleToRow(messageList->itemWidget(item), item, tooltip);
		return;
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

	/* Never paint letter-pill badges — image badges live in their own QLabels (Twitch/Kick).
	 * YouTube owner/mod without CDN URLs still get a short text chip in the name span. */
	QString badgesHtml;
	if (msg.platform == YouTube) {
		for (const ChatBadge &badge : msg.badges) {
			if (!badge.label.isEmpty() && badge.url.isEmpty()) {
				badgesHtml += QStringLiteral(
						      "<span style='background:#333;color:#ddd;border-radius:3px;padding:0 3px;"
						      "font-size:9px;font-weight:700;margin-right:3px;'>%1</span>")
						      .arg(EscapeHtml(badge.label.left(1).toUpper()));
			}
		}
	}

	const QString prefixHtml =
		QStringLiteral("<span style='color:%1;font-weight:700;'>%2%3</span><span style='color:#e6e6e6;'>: </span>")
			.arg(nameColor, badgesHtml, EscapeHtml(msg.user));
	const bool kickEmotes = !msg.system && msg.platform == Kick && msg.text.contains(QLatin1String("[emote:"));
	const bool twitchEmotes = !msg.system && msg.platform == Twitch && !msg.emotes.isEmpty();

	auto *item = new QListWidgetItem(messageList);
	item->setData(Qt::UserRole, msg.system);
	item->setData(Qt::UserRole + 1, msg.id);
	item->setData(Qt::UserRole + 2, false); /* deleted */
	item->setData(Qt::UserRole + 3,
		      msg.userLogin.isEmpty() ? msg.user.toLower() : msg.userLogin.toLower());
	item->setData(Qt::UserRole + 4, int(msg.platform));

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
	} else {
		auto *label = new QLabel(row);
		label->setObjectName(QStringLiteral("chatMsgBody"));
		label->setTextFormat(Qt::RichText);
		label->setWordWrap(true);
		label->setOpenExternalLinks(true);
		label->setTextInteractionFlags(Qt::TextBrowserInteraction);
		label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
		label->setStyleSheet(
			QStringLiteral("background: transparent; color: #e6e6e6; border: none; padding: 0; margin: 0;"));
		label->setText(prefixHtml + Linkify(EscapeHtml(msg.text)));
		body = label;
	}
	layout->addWidget(body, 1, Qt::AlignTop);

	messageList->addItem(item);
	messageList->setItemWidget(item, row);
	LayoutMessageRow(row, item, messageList->viewport()->width());

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
