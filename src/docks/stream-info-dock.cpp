#include "stream-info-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QPointer>
#include <curl/curl.h>
#include <thread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <src/utils/icon.hpp>

extern obs_data_t *current_profile_config;
void save_current_profile_config(bool save_docks);

static const int kRedirectPort = 17654;
static const char *kTwitchScopes = "channel:manage:broadcast";

struct HttpResult {
	int status = 0;
	QByteArray body;
	QString error;
};

static size_t CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *out = static_cast<QByteArray *>(userdata);
	const size_t total = size * nmemb;
	out->append(ptr, static_cast<qsizetype>(total));
	return total;
}

static HttpResult CurlPerform(CURL *curl)
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

static void RunAsync(QObject *context, std::function<HttpResult()> work,
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

static void PostForm(const QUrl &url, const QList<QPair<QString, QString>> &fields, QObject *context,
		     const std::function<void(int, const QByteArray &, const QString &)> &done)
{
	const QByteArray urlBytes = url.toString(QUrl::FullyEncoded).toUtf8();
	QList<QPair<QByteArray, QByteArray>> encoded;
	for (const auto &field : fields)
		encoded.append({field.first.toUtf8(), field.second.toUtf8()});
	RunAsync(
		context,
		[urlBytes, encoded] {
			HttpResult result;
			CURL *curl = curl_easy_init();
			if (!curl) {
				result.error = QStringLiteral("curl_easy_init failed");
				return result;
			}
			curl_mime *mime = curl_mime_init(curl);
			for (const auto &field : encoded) {
				curl_mimepart *part = curl_mime_addpart(mime);
				curl_mime_name(part, field.first.constData());
				curl_mime_data(part, field.second.constData(), static_cast<curl_off_t>(field.second.size()));
			}
			curl_easy_setopt(curl, CURLOPT_URL, urlBytes.constData());
			curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
			result = CurlPerform(curl);
			curl_mime_free(mime);
			curl_easy_cleanup(curl);
			return result;
		},
		done);
}

static QString Tr(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString JsonError(const QByteArray &body)
{
	const QJsonObject obj = QJsonDocument::fromJson(body).object();
	const QString message = obj.value("message").toString();
	if (!message.isEmpty())
		return message;
	const QString error = obj.value("error").toString();
	const QString desc = obj.value("error_description").toString();
	if (!desc.isEmpty())
		return desc;
	if (obj.value("error").isObject()) {
		const QString msg = obj.value("error").toObject().value("message").toString();
		if (!msg.isEmpty())
			return msg;
	}
	if (!error.isEmpty())
		return error;
	const QString text = QString::fromUtf8(body).trimmed();
	return text.left(240);
}

static QByteArray PkceVerifier()
{
	QByteArray raw(32, 0);
	auto *rng = QRandomGenerator::system();
	for (int i = 0; i < raw.size(); i++)
		raw[i] = char(rng->bounded(256));
	return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

static QByteArray PkceChallenge(const QByteArray &verifier)
{
	return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
		.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

static QString RandomState()
{
	return QString::number(QRandomGenerator::system()->generate64(), 16);
}

const char *StreamInfoDock::PlatformKey(Platform platform)
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

StreamInfoDock::StreamInfoDock(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("AitumStreamSuiteStreamInfo"));
	curl_global_init(CURL_GLOBAL_DEFAULT);
	pollTimer = new QTimer(this);
	pollTimer->setSingleShot(true);
	connect(pollTimer, &QTimer::timeout, this, &StreamInfoDock::PollTwitchDevice);
	searchTimer = new QTimer(this);
	searchTimer->setSingleShot(true);
	searchTimer->setInterval(350);
	connect(searchTimer, &QTimer::timeout, this, &StreamInfoDock::SearchCategories);
	BuildUi();
}

StreamInfoDock::~StreamInfoDock()
{
	authGeneration++;
	if (pollTimer)
		pollTimer->stop();
	if (authServer)
		authServer->close();
}

static QFrame *InfoRow(QWidget *parent, const QString &labelText, QWidget *field, QLabel *icon = nullptr)
{
	auto *row = new QFrame(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(8, 4, 8, 4);
	layout->setSpacing(8);
	if (icon) {
		auto *slot = new QWidget(row);
		slot->setFixedWidth(24);
		auto *slotLayout = new QHBoxLayout(slot);
		slotLayout->setContentsMargins(0, 0, 0, 0);
		slotLayout->setSpacing(0);
		slotLayout->addWidget(icon, 0, Qt::AlignVCenter);
		layout->addWidget(slot, 0, Qt::AlignVCenter);
	}
	auto *label = new QLabel(labelText, row);
	layout->addWidget(label, 0, Qt::AlignVCenter);
	layout->addWidget(field, 1);
	return row;
}

static void StyleTextButton(QPushButton *button)
{
	button->setMinimumHeight(28);
	button->setMaximumHeight(32);
	button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void StreamInfoDock::BuildUi()
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	auto *content = new QWidget(this);
	auto *rows = new QVBoxLayout(content);
	rows->setContentsMargins(0, 0, 0, 0);
	rows->setSpacing(0);

	platformIcon = new QLabel(content);
	platformIcon->setFixedSize(24, 24);
	platformIcon->setScaledContents(true);
	platformCombo = new QComboBox(content);
	platformCombo->addItem(QStringLiteral("Twitch"), int(Twitch));
	platformCombo->addItem(QStringLiteral("YouTube"), int(YouTube));
	platformCombo->addItem(QStringLiteral("Kick"), int(Kick));
	rows->addWidget(InfoRow(content, Tr("StreamInfoPlatform"), platformCombo, platformIcon));

	connectButton = new QPushButton(Tr("StreamInfoConnect"), content);
	disconnectButton = new QPushButton(Tr("StreamInfoDisconnect"), content);
	StyleTextButton(connectButton);
	StyleTextButton(disconnectButton);
	auto *auth = new QFrame(content);
	auto *authLayout = new QHBoxLayout(auth);
	authLayout->setContentsMargins(8, 4, 8, 4);
	authLayout->setSpacing(8);
	authLayout->addWidget(connectButton);
	authLayout->addWidget(disconnectButton);
	rows->addWidget(auth);

	statusLabel = new QLabel(Tr("StreamInfoNotConnected"), content);
	statusLabel->setWordWrap(true);
	rows->addWidget(InfoRow(content, QString(), statusLabel));

	titleEdit = new QLineEdit(content);
	rows->addWidget(InfoRow(content, Tr("StreamInfoTitle"), titleEdit));

	categorySearch = new QLineEdit(content);
	categorySearch->setPlaceholderText(Tr("StreamInfoCategorySearch"));
	categoryCombo = new QComboBox(content);
	auto *categoryFields = new QWidget(content);
	auto *categoryLayout = new QVBoxLayout(categoryFields);
	categoryLayout->setContentsMargins(0, 0, 0, 0);
	categoryLayout->setSpacing(4);
	categoryLayout->addWidget(categorySearch);
	categoryLayout->addWidget(categoryCombo);
	auto *categoryRow = InfoRow(content, Tr("StreamInfoCategory"), categoryFields);
	categoryRow->setObjectName(QStringLiteral("streamInfoCategoryRow"));
	rows->addWidget(categoryRow);

	descriptionEdit = new QPlainTextEdit(content);
	descriptionEdit->setFixedHeight(72);
	auto *descriptionRow = InfoRow(content, Tr("StreamInfoDescription"), descriptionEdit);
	descriptionRow->setObjectName(QStringLiteral("streamInfoDescriptionRow"));
	rows->addWidget(descriptionRow);

	tagsEdit = new QLineEdit(content);
	tagsEdit->setPlaceholderText(Tr("StreamInfoTagsHint"));
	rows->addWidget(InfoRow(content, Tr("StreamInfoTags"), tagsEdit));

	applyButton = new QPushButton(Tr("StreamInfoApply"), content);
	StyleTextButton(applyButton);
	auto *applyRow = new QFrame(content);
	auto *applyLayout = new QHBoxLayout(applyRow);
	applyLayout->setContentsMargins(8, 4, 8, 4);
	applyLayout->setSpacing(8);
	applyLayout->addWidget(applyButton);
	rows->addWidget(applyRow);

	resultLabel = new QLabel(content);
	resultLabel->setWordWrap(true);
	rows->addWidget(InfoRow(content, QString(), resultLabel));
	rows->addStretch(1);

	auto *scroll = new QScrollArea(this);
	scroll->setWidget(content);
	scroll->setWidgetResizable(true);
	scroll->setLineWidth(0);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll, 1);

	auto *toolbar = new QToolBar(this);
	toolbar->setObjectName(QStringLiteral("outputsToolbar"));
	toolbar->setIconSize(QSize(16, 16));
	toolbar->setFloatable(false);
	auto *settingsAction = toolbar->addAction(QIcon(QString::fromUtf8(":/settings/images/settings/general.svg")),
						   QString::fromUtf8(obs_frontend_get_locale_string("Settings")), this,
						   &StreamInfoDock::OpenSettings);
	toolbar->widgetForAction(settingsAction)->setProperty("themeID", QVariant(QString::fromUtf8("propertiesIconSmall")));
	toolbar->widgetForAction(settingsAction)->setProperty("class", "icon-gear");
	outer->addWidget(toolbar);

	connect(platformCombo, &QComboBox::currentIndexChanged, this, [this](int index) { SwitchPlatform(index); });
	connect(connectButton, &QPushButton::clicked, this, &StreamInfoDock::BeginConnect);
	connect(disconnectButton, &QPushButton::clicked, this, &StreamInfoDock::DisconnectAccount);
	connect(applyButton, &QPushButton::clicked, this, &StreamInfoDock::Apply);
	connect(categorySearch, &QLineEdit::textEdited, this, [this] {
		searchTimer->start();
	});

	UpdateFieldVisibility();
	SetConnected(false);
}

static void ReadCredential(const char *key, QString &clientId, QString &clientSecret)
{
	clientId.clear();
	clientSecret.clear();
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, key) : nullptr;
	if (one) {
		clientId = QString::fromUtf8(obs_data_get_string(one, "client_id"));
		clientSecret = QString::fromUtf8(obs_data_get_string(one, "client_secret"));
	}
	obs_data_release(one);
	obs_data_release(root);
}

static void WriteCredential(const char *key, const QString &clientId, const QString &clientSecret, bool idChanged)
{
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	if (!root)
		root = obs_data_create();
	obs_data_t *one = obs_data_get_obj(root, key);
	if (!one)
		one = obs_data_create();
	obs_data_set_string(one, "client_id", clientId.toUtf8().constData());
	obs_data_set_string(one, "client_secret", clientSecret.toUtf8().constData());
	if (idChanged) {
		obs_data_erase(one, "access_token");
		obs_data_erase(one, "refresh_token");
		obs_data_set_int(one, "expires_at", 0);
		obs_data_erase(one, "user_id");
		obs_data_erase(one, "user_name");
	}
	obs_data_set_obj(root, key, one);
	obs_data_set_obj(current_profile_config, "stream_accounts", root);
	obs_data_release(one);
	obs_data_release(root);
}

static QLineEdit *SecretEdit(QWidget *parent, const QString &value)
{
	auto *edit = new QLineEdit(parent);
	edit->setEchoMode(QLineEdit::Password);
	edit->setText(value);
	auto *toggle = edit->addAction(QString::fromUtf8(obs_frontend_get_locale_string("Show")), QLineEdit::TrailingPosition);
	toggle->setCheckable(true);
	QObject::connect(toggle, &QAction::toggled, edit, [edit, toggle](bool shown) {
		edit->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
		toggle->setText(QString::fromUtf8(obs_frontend_get_locale_string(shown ? "Hide" : "Show")));
	});
	return edit;
}

void StreamInfoDock::OpenSettings()
{
	QString twitchId;
	QString twitchSecret;
	QString youtubeId;
	QString youtubeSecret;
	QString kickId;
	QString kickSecret;
	ReadCredential("twitch", twitchId, twitchSecret);
	ReadCredential("youtube", youtubeId, youtubeSecret);
	ReadCredential("kick", kickId, kickSecret);

	QDialog dialog(this);
	dialog.setWindowTitle(QString::fromUtf8(obs_frontend_get_locale_string("Settings")));
	auto *form = new QFormLayout;
	auto *twitchEdit = SecretEdit(&dialog, twitchId);
	auto *youtubeEdit = SecretEdit(&dialog, youtubeId);
	auto *youtubeSecretEdit = SecretEdit(&dialog, youtubeSecret);
	auto *kickEdit = SecretEdit(&dialog, kickId);
	auto *kickSecretEdit = SecretEdit(&dialog, kickSecret);
	form->addRow(QStringLiteral("Twitch"), twitchEdit);
	form->addRow(QStringLiteral("YouTube"), youtubeEdit);
	form->addRow(QStringLiteral("YouTube ") + Tr("StreamInfoClientSecret"), youtubeSecretEdit);
	form->addRow(QStringLiteral("Kick"), kickEdit);
	form->addRow(QStringLiteral("Kick ") + Tr("StreamInfoClientSecret"), kickSecretEdit);
	dialog.setMinimumWidth(420);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	auto *layout = new QVBoxLayout(&dialog);
	layout->addLayout(form);
	layout->addWidget(buttons);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const QString newTwitch = twitchEdit->text().trimmed();
	const QString newYoutube = youtubeEdit->text().trimmed();
	const QString newYoutubeSecret = youtubeSecretEdit->text().trimmed();
	const QString newKick = kickEdit->text().trimmed();
	const QString newKickSecret = kickSecretEdit->text().trimmed();
	WriteCredential("twitch", newTwitch, QString(), newTwitch != twitchId);
	WriteCredential("youtube", newYoutube, newYoutubeSecret, newYoutube != youtubeId || newYoutubeSecret != youtubeSecret);
	WriteCredential("kick", newKick, newKickSecret, newKick != kickId || newKickSecret != kickSecret);
	save_current_profile_config(false);
	LoadPlatform();
}

void StreamInfoDock::ReloadAccounts()
{
	LoadPlatform();
	UpdateFieldVisibility();
	if (TokenFresh())
		RefreshChannel();
	else if (!accessToken.isEmpty())
		EnsureToken([this](bool ok) {
			if (ok)
				RefreshChannel();
		});
	else
		SetConnected(false);
}

void StreamInfoDock::SwitchPlatform(int index)
{
	SavePlatform();
	authGeneration++;
	pollTimer->stop();
	platform = Platform(platformCombo->itemData(index).toInt());
	titleEdit->clear();
	categorySearch->clear();
	categoryCombo->clear();
	descriptionEdit->clear();
	tagsEdit->clear();
	categoryId.clear();
	ytBroadcastId.clear();
	SetResult(QString());
	LoadPlatform();
	UpdateFieldVisibility();
	if (!accessToken.isEmpty())
		EnsureToken([this](bool ok) {
			if (ok)
				RefreshChannel();
		});
	else
		SetConnected(false);
}

void StreamInfoDock::LoadPlatform()
{
	clientId.clear();
	clientSecret.clear();
	accessToken.clear();
	refreshToken.clear();
	expiresAt = 0;
	userId.clear();
	userName.clear();
	categoryId.clear();
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, PlatformKey(platform)) : nullptr;
	if (one) {
		clientId = QString::fromUtf8(obs_data_get_string(one, "client_id"));
		clientSecret = QString::fromUtf8(obs_data_get_string(one, "client_secret"));
		accessToken = QString::fromUtf8(obs_data_get_string(one, "access_token"));
		refreshToken = QString::fromUtf8(obs_data_get_string(one, "refresh_token"));
		expiresAt = obs_data_get_int(one, "expires_at");
		userId = QString::fromUtf8(obs_data_get_string(one, "user_id"));
		userName = QString::fromUtf8(obs_data_get_string(one, "user_name"));
		categoryId = QString::fromUtf8(obs_data_get_string(one, "category_id"));
	}
	obs_data_release(one);
	obs_data_release(root);
	SetConnected(!accessToken.isEmpty());
}

void StreamInfoDock::SavePlatform()
{
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	if (!root)
		root = obs_data_create();
	obs_data_t *one = obs_data_get_obj(root, PlatformKey(platform));
	if (!one)
		one = obs_data_create();
	obs_data_set_string(one, "client_id", clientId.toUtf8().constData());
	obs_data_set_string(one, "client_secret", clientSecret.toUtf8().constData());
	obs_data_set_string(one, "access_token", accessToken.toUtf8().constData());
	obs_data_set_string(one, "refresh_token", refreshToken.toUtf8().constData());
	obs_data_set_int(one, "expires_at", expiresAt);
	obs_data_set_string(one, "user_id", userId.toUtf8().constData());
	obs_data_set_string(one, "user_name", userName.toUtf8().constData());
	obs_data_set_string(one, "category_id", categoryId.toUtf8().constData());
	obs_data_set_obj(root, PlatformKey(platform), one);
	obs_data_set_obj(current_profile_config, "stream_accounts", root);
	obs_data_release(one);
	obs_data_release(root);
	save_current_profile_config(false);
}

void StreamInfoDock::SetStatus(const QString &text)
{
	statusLabel->setText(text);
}

void StreamInfoDock::SetResult(const QString &text)
{
	resultLabel->setText(text);
}

void StreamInfoDock::UpdateFieldVisibility()
{
	const bool youtube = platform == YouTube;
	if (auto *row = findChild<QFrame *>(QStringLiteral("streamInfoCategoryRow")))
		row->setVisible(!youtube);
	if (auto *row = findChild<QFrame *>(QStringLiteral("streamInfoDescriptionRow")))
		row->setVisible(youtube);
	QString endpoint = QStringLiteral("rtmp://live.twitch.tv/app");
	if (platform == YouTube)
		endpoint = QStringLiteral("rtmp://a.rtmp.youtube.com/live2");
	else if (platform == Kick)
		endpoint = QStringLiteral("rtmp://fa723fc1b171.global-contribute.live-video.net/app");
	if (platformIcon)
		platformIcon->setPixmap(getPlatformIconFromEndpoint(endpoint).pixmap(24, 24));
}

void StreamInfoDock::SetConnected(bool connected)
{
	if (connected && !userName.isEmpty())
		SetStatus(Tr("StreamInfoConnected").arg(userName));
	else if (connected)
		SetStatus(Tr("StreamInfoConnectedBare"));
	else
		SetStatus(Tr("StreamInfoNotConnected"));
	applyButton->setEnabled(connected);
	disconnectButton->setEnabled(connected);
}

bool StreamInfoDock::TokenFresh() const
{
	if (accessToken.isEmpty())
		return false;
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	return expiresAt == 0 || expiresAt > now + 60;
}

void StreamInfoDock::Http(const QNetworkRequest &request, const QByteArray &body, const char *verb,
			  std::function<void(int, const QByteArray &, const QString &)> done)
{
	const QByteArray urlBytes = request.url().toString(QUrl::FullyEncoded).toUtf8();
	const QByteArray verbBytes(verb);
	const QByteArray payload = body;
	QList<QPair<QByteArray, QByteArray>> headers;
	const QVariant type = request.header(QNetworkRequest::ContentTypeHeader);
	if (type.isValid())
		headers.append({QByteArrayLiteral("Content-Type"), type.toString().toUtf8()});
	for (const QByteArray &name : request.rawHeaderList())
		headers.append({name, request.rawHeader(name)});
	RunAsync(
		this,
		[urlBytes, verbBytes, payload, headers] {
			HttpResult result;
			CURL *curl = curl_easy_init();
			if (!curl) {
				result.error = QStringLiteral("curl_easy_init failed");
				return result;
			}
			curl_slist *list = nullptr;
			for (const auto &header : headers) {
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

void StreamInfoDock::EnsureToken(std::function<void(bool)> done)
{
	if (TokenFresh()) {
		done(true);
		return;
	}
	if (refreshToken.isEmpty() || clientId.isEmpty()) {
		SetConnected(false);
		done(false);
		return;
	}
	QUrl url;
	QUrlQuery query;
	query.addQueryItem("grant_type", "refresh_token");
	query.addQueryItem("refresh_token", refreshToken);
	query.addQueryItem("client_id", clientId);
	if (platform == Twitch) {
		url = QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token"));
	} else if (platform == YouTube) {
		url = QUrl(QStringLiteral("https://oauth2.googleapis.com/token"));
		query.addQueryItem("client_secret", clientSecret);
	} else {
		url = QUrl(QStringLiteral("https://id.kick.com/oauth/token"));
		query.addQueryItem("client_secret", clientSecret);
	}
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	Http(req, query.query(QUrl::FullyEncoded).toUtf8(), "POST", [this, done](int status, const QByteArray &body, const QString &) {
		const QJsonObject json = QJsonDocument::fromJson(body).object();
		if (status < 200 || status >= 300 || !json.contains("access_token")) {
			accessToken.clear();
			SavePlatform();
			SetConnected(false);
			SetResult(JsonError(body));
			done(false);
			return;
		}
		AcceptToken(json);
		done(true);
	});
}

void StreamInfoDock::BeginConnect()
{
	SavePlatform();
	if (!current_profile_config) {
		SetStatus(Tr("StreamInfoNoProfile"));
		return;
	}
	if (clientId.isEmpty() || (platform != Twitch && clientSecret.isEmpty())) {
		SetStatus(Tr("StreamInfoUnavailable"));
		return;
	}
	authGeneration++;
	pollTimer->stop();
	connectButton->setEnabled(false);
	SetResult(QString());
	SetStatus(Tr("StreamInfoConnecting"));
	if (platform == Twitch)
		BeginTwitchDevice();
	else
		BeginAuthCode();
}

void StreamInfoDock::BeginTwitchDevice()
{
	const int gen = authGeneration;
	PostForm(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/device")),
		 {{QStringLiteral("client_id"), clientId}, {QStringLiteral("scopes"), QString::fromUtf8(kTwitchScopes)}}, this,
		 [this, gen](int status, const QByteArray &body, const QString &err) {
			 connectButton->setEnabled(true);
			 if (gen != authGeneration)
				 return;
			 const QJsonObject json = QJsonDocument::fromJson(body).object();
			 if (status < 200 || status >= 300 || !json.contains("device_code")) {
				 const QString message = JsonError(body);
				 SetStatus(message.isEmpty() ? err : message);
				 blog(LOG_WARNING, "[Stream Suite] Twitch device login failed (%d)", status);
				 return;
			 }
			 deviceCode = json.value("device_code").toString();
			 pollIntervalMs = qMax(5, json.value("interval").toInt(5)) * 1000;
			 const QString userCode = json.value("user_code").toString();
			 QString verify = json.value("verification_uri").toString();
			 if (verify.isEmpty())
				 verify = QStringLiteral("https://www.twitch.tv/activate?public=true&device-code=") + userCode;
			 SetStatus(Tr("StreamInfoTwitchCode").arg(userCode));
			 QDesktopServices::openUrl(QUrl(verify));
			 pollTimer->start(pollIntervalMs);
		 });
}

void StreamInfoDock::PollTwitchDevice()
{
	const int gen = authGeneration;
	PostForm(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")),
		 {{QStringLiteral("client_id"), clientId},
		  {QStringLiteral("scopes"), QString::fromUtf8(kTwitchScopes)},
		  {QStringLiteral("device_code"), deviceCode},
		  {QStringLiteral("grant_type"), QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")}},
		 this, [this, gen](int status, const QByteArray &body, const QString &err) {
		     if (gen != authGeneration)
			     return;
		     const QJsonObject json = QJsonDocument::fromJson(body).object();
		     const QString error = json.value("error").toString();
		     const QString message = json.value("message").toString();
		     if (error == "authorization_pending" || message == "authorization_pending" || error == "slow_down" ||
			 message == "slow_down") {
			     if (error == "slow_down" || message == "slow_down")
				     pollIntervalMs += 2000;
			     pollTimer->start(pollIntervalMs);
			     return;
		     }
		     if (status < 200 || status >= 300 || !json.contains("access_token")) {
			     const QString message = JsonError(body);
			     SetStatus(message.isEmpty() ? err : message);
			     SetConnected(false);
			     return;
		     }
		     AcceptToken(json);
		     RefreshChannel();
	     });
}

void StreamInfoDock::BeginAuthCode()
{
	if (!authServer) {
		authServer = new QTcpServer(this);
		connect(authServer, &QTcpServer::newConnection, this, [this] {
			QTcpSocket *socket = authServer->nextPendingConnection();
			connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
				const QByteArray raw = socket->readAll();
				const QString req = QString::fromUtf8(raw);
				const int lineEnd = req.indexOf("\r\n");
				const QString line = lineEnd > 0 ? req.left(lineEnd) : req;
				const QUrl url(QStringLiteral("http://127.0.0.1") + line.section(' ', 1, 1));
				const QUrlQuery q(url);
				const QByteArray page = QByteArrayLiteral(
					"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
					"<html><body><p>Login received. You can close this window.</p></body></html>");
				socket->write(page);
				socket->disconnectFromHost();
				if (q.queryItemValue("state") == oauthState)
					FinishAuthCode(q.queryItemValue("code"));
				else
					SetResult(Tr("StreamInfoAuthFailed"));
				connectButton->setEnabled(true);
			});
		});
	}
	if (authServer->isListening())
		authServer->close();
	if (!authServer->listen(QHostAddress::LocalHost, kRedirectPort)) {
		connectButton->setEnabled(true);
		SetResult(Tr("StreamInfoAuthFailed"));
		return;
	}
	codeVerifier = PkceVerifier();
	oauthState = RandomState();
	const QString redirect = QStringLiteral("http://127.0.0.1:%1/callback").arg(kRedirectPort);
	QUrl url;
	QUrlQuery query;
	query.addQueryItem("response_type", "code");
	query.addQueryItem("client_id", clientId);
	query.addQueryItem("redirect_uri", redirect);
	query.addQueryItem("state", oauthState);
	query.addQueryItem("code_challenge", QString::fromUtf8(PkceChallenge(codeVerifier)));
	query.addQueryItem("code_challenge_method", "S256");
	if (platform == YouTube) {
		url = QUrl(QStringLiteral("https://accounts.google.com/o/oauth2/v2/auth"));
		query.addQueryItem("scope", "https://www.googleapis.com/auth/youtube");
		query.addQueryItem("access_type", "offline");
		query.addQueryItem("prompt", "consent");
	} else {
		url = QUrl(QStringLiteral("https://id.kick.com/oauth/authorize"));
		query.addQueryItem("scope", "user:read channel:read channel:write");
	}
	url.setQuery(query);
	QDesktopServices::openUrl(url);
	SetStatus(Tr("StreamInfoBrowser"));
}

void StreamInfoDock::FinishAuthCode(const QString &code)
{
	if (authServer)
		authServer->close();
	if (code.isEmpty()) {
		SetResult(Tr("StreamInfoAuthFailed"));
		return;
	}
	ExchangeAuthCode(code);
}

void StreamInfoDock::ExchangeAuthCode(const QString &code)
{
	const int gen = authGeneration;
	const QString redirect = QStringLiteral("http://127.0.0.1:%1/callback").arg(kRedirectPort);
	QUrlQuery query;
	query.addQueryItem("grant_type", "authorization_code");
	query.addQueryItem("client_id", clientId);
	query.addQueryItem("client_secret", clientSecret);
	query.addQueryItem("code", code);
	query.addQueryItem("redirect_uri", redirect);
	query.addQueryItem("code_verifier", QString::fromUtf8(codeVerifier));
	const QUrl url = platform == YouTube ? QUrl(QStringLiteral("https://oauth2.googleapis.com/token"))
					      : QUrl(QStringLiteral("https://id.kick.com/oauth/token"));
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
	Http(req, query.query(QUrl::FullyEncoded).toUtf8(), "POST",
	     [this, gen](int status, const QByteArray &body, const QString &) {
		     connectButton->setEnabled(true);
		     if (gen != authGeneration)
			     return;
		     const QJsonObject json = QJsonDocument::fromJson(body).object();
		     if (status < 200 || status >= 300 || !json.contains("access_token")) {
			     SetResult(JsonError(body));
			     return;
		     }
		     AcceptToken(json);
		     RefreshChannel();
	     });
}

void StreamInfoDock::AcceptToken(const QJsonObject &json)
{
	accessToken = json.value("access_token").toString();
	if (json.contains("refresh_token"))
		refreshToken = json.value("refresh_token").toString();
	const int expiresIn = json.value("expires_in").toInt(0);
	expiresAt = expiresIn > 0 ? QDateTime::currentSecsSinceEpoch() + expiresIn : 0;
	SavePlatform();
	SetConnected(true);
}

QNetworkRequest StreamInfoDock_Helix(const QString &url, const QString &clientId, const QString &token)
{
	QNetworkRequest req{QUrl(url)};
	req.setRawHeader("Client-Id", clientId.toUtf8());
	req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
	return req;
}

void StreamInfoDock::RefreshChannel()
{
	SetConnected(true);
	if (platform == Twitch)
		LoadTwitchChannel();
	else if (platform == YouTube)
		LoadYouTubeBroadcast();
	else
		LoadKickChannel();
}

void StreamInfoDock::LoadTwitchChannel()
{
	const int gen = authGeneration;
	auto req = StreamInfoDock_Helix("https://api.twitch.tv/helix/users", clientId, accessToken);
	Http(req, {}, "GET", [this, gen](int status, const QByteArray &body, const QString &) {
		if (gen != authGeneration)
			return;
		const QJsonArray data = QJsonDocument::fromJson(body).object().value("data").toArray();
		if (status < 200 || status >= 300 || data.isEmpty()) {
			SetResult(JsonError(body));
			return;
		}
		const QJsonObject user = data.at(0).toObject();
		userId = user.value("id").toString();
		userName = user.value("display_name").toString(user.value("login").toString());
		SavePlatform();
		SetConnected(true);
		auto channel = StreamInfoDock_Helix(
			"https://api.twitch.tv/helix/channels?broadcaster_id=" + userId, clientId, accessToken);
		Http(channel, {}, "GET", [this, gen](int status2, const QByteArray &body2, const QString &) {
			if (gen != authGeneration || status2 < 200 || status2 >= 300)
				return;
			const QJsonArray rows = QJsonDocument::fromJson(body2).object().value("data").toArray();
			if (rows.isEmpty())
				return;
			const QJsonObject row = rows.at(0).toObject();
			titleEdit->setText(row.value("title").toString());
			categoryId = row.value("game_id").toString();
			categorySearch->setText(row.value("game_name").toString());
			categoryCombo->clear();
			if (!categoryId.isEmpty())
				categoryCombo->addItem(row.value("game_name").toString(), categoryId);
			QStringList tags;
			for (const QJsonValue &tag : row.value("tags").toArray())
				tags.append(tag.toString());
			tagsEdit->setText(tags.join(", "));
		});
	});
}

void StreamInfoDock::ApplyYouTubeBroadcast(const QJsonObject &item)
{
	ytBroadcastId = item.value("id").toString();
	const QJsonObject snippet = item.value("snippet").toObject();
	titleEdit->setText(snippet.value("title").toString());
	descriptionEdit->setPlainText(snippet.value("description").toString());
	ytScheduledStart = snippet.value("scheduledStartTime").toString();
	const int gen = authGeneration;
	QUrl videoUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"));
	QUrlQuery vq;
	vq.addQueryItem("part", "snippet");
	vq.addQueryItem("id", ytBroadcastId);
	videoUrl.setQuery(vq);
	QNetworkRequest vreq(videoUrl);
	vreq.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
	Http(vreq, {}, "GET", [this, gen](int, const QByteArray &vbody, const QString &) {
		if (gen != authGeneration)
			return;
		const QJsonArray videos = QJsonDocument::fromJson(vbody).object().value("items").toArray();
		if (videos.isEmpty())
			return;
		const QJsonObject snip = videos.at(0).toObject().value("snippet").toObject();
		ytCategoryId = snip.value("categoryId").toString();
		QStringList tags;
		for (const QJsonValue &tag : snip.value("tags").toArray())
			tags.append(tag.toString());
		tagsEdit->setText(tags.join(", "));
	});
}

void StreamInfoDock::LoadYouTubeBroadcast()
{
	const int gen = authGeneration;
	auto requestStatus = [this](const QString &status) {
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "snippet,status");
		query.addQueryItem("broadcastStatus", status);
		query.addQueryItem("mine", "true");
		query.addQueryItem("maxResults", "5");
		url.setQuery(query);
		QNetworkRequest req(url);
		req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		return req;
	};
	Http(requestStatus("active"), {}, "GET", [this, gen, requestStatus](int code, const QByteArray &body, const QString &) {
		if (gen != authGeneration)
			return;
		const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
		if (code < 200 || code >= 300) {
			SetResult(JsonError(body));
			return;
		}
		if (!items.isEmpty()) {
			ApplyYouTubeBroadcast(items.at(0).toObject());
			return;
		}
		Http(requestStatus("upcoming"), {}, "GET", [this, gen](int code2, const QByteArray &body2, const QString &) {
			if (gen != authGeneration)
				return;
			const QJsonArray upcoming = QJsonDocument::fromJson(body2).object().value("items").toArray();
			if (code2 < 200 || code2 >= 300) {
				SetResult(JsonError(body2));
				return;
			}
			if (!upcoming.isEmpty()) {
				ApplyYouTubeBroadcast(upcoming.at(0).toObject());
				return;
			}
			ytBroadcastId.clear();
			SetResult(Tr("StreamInfoNoBroadcast"));
		});
	});
}

void StreamInfoDock::LoadKickChannel()
{
	const int gen = authGeneration;
	QNetworkRequest req(QUrl(QStringLiteral("https://api.kick.com/public/v1/users")));
	req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
	Http(req, {}, "GET", [this, gen](int status, const QByteArray &body, const QString &) {
		if (gen != authGeneration)
			return;
		if (status < 200 || status >= 300) {
			SetResult(JsonError(body));
			return;
		}
		QJsonObject root = QJsonDocument::fromJson(body).object();
		QJsonObject user = root.value("data").isArray() ? root.value("data").toArray().at(0).toObject()
								 : root.value("data").toObject();
		if (user.isEmpty())
			user = root;
		userId = QString::number(user.value("user_id").toInt(user.value("id").toInt()));
		if (userId == "0")
			userId = user.value("user_id").toString(user.value("id").toString());
		userName = user.value("name").toString(user.value("username").toString());
		SavePlatform();
		SetConnected(true);
		const QString title = user.value("stream_title").toString();
		if (!title.isEmpty())
			titleEdit->setText(title);
	});
}

void StreamInfoDock::SearchCategories()
{
	const QString queryText = categorySearch->text().trimmed();
	if (queryText.size() < 2 || accessToken.isEmpty() || platform == YouTube)
		return;
	EnsureToken([this, queryText](bool ok) {
		if (!ok)
			return;
		if (platform == Twitch) {
			auto req = StreamInfoDock_Helix(
				"https://api.twitch.tv/helix/search/categories?first=10&query=" +
					QString::fromUtf8(QUrl::toPercentEncoding(queryText)),
				clientId, accessToken);
			Http(req, {}, "GET", [this](int status, const QByteArray &body, const QString &) {
				if (status < 200 || status >= 300)
					return;
				categoryCombo->clear();
				for (const QJsonValue &row : QJsonDocument::fromJson(body).object().value("data").toArray()) {
					const QJsonObject obj = row.toObject();
					categoryCombo->addItem(obj.value("name").toString(), obj.value("id").toString());
				}
			});
			return;
		}
		QUrl url(QStringLiteral("https://api.kick.com/public/v1/categories"));
		QUrlQuery query;
		query.addQueryItem("q", queryText);
		url.setQuery(query);
		QNetworkRequest req(url);
		req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		Http(req, {}, "GET", [this](int status, const QByteArray &body, const QString &) {
			if (status < 200 || status >= 300)
				return;
			categoryCombo->clear();
			const QJsonDocument doc = QJsonDocument::fromJson(body);
			const QJsonArray rows = doc.object().value("data").isArray() ? doc.object().value("data").toArray()
										       : doc.array();
			for (const QJsonValue &row : rows) {
				const QJsonObject obj = row.toObject();
				const QString id = obj.value("id").isDouble() ? QString::number(obj.value("id").toInt())
									       : obj.value("id").toString();
				const QString name = obj.value("name").toString(obj.value("category").toString());
				if (!id.isEmpty() && !name.isEmpty())
					categoryCombo->addItem(name, id);
			}
		});
	});
}

QStringList StreamInfoDock::Tags() const
{
	QStringList tags;
	for (const QString &part : tagsEdit->text().split(',')) {
		const QString tag = part.trimmed();
		if (!tag.isEmpty())
			tags.append(tag);
	}
	while (tags.size() > 10)
		tags.removeLast();
	return tags;
}

void StreamInfoDock::Apply()
{
	SavePlatform();
	categoryId = categoryCombo->currentData().toString();
	if (categoryId.isEmpty())
		categoryId = categoryCombo->currentData(Qt::UserRole).toString();
	SetResult(QString());
	EnsureToken([this](bool ok) {
		if (!ok) {
			SetResult(Tr("StreamInfoNotConnected"));
			return;
		}
		if (platform == Twitch)
			ApplyTwitch();
		else if (platform == YouTube)
			ApplyYouTube();
		else
			ApplyKick();
	});
}

void StreamInfoDock::ApplyTwitch()
{
	if (userId.isEmpty()) {
		SetResult(Tr("StreamInfoNotConnected"));
		return;
	}
	QJsonObject body;
	body.insert("title", titleEdit->text());
	if (!categoryId.isEmpty())
		body.insert("game_id", categoryId);
	const QStringList tags = Tags();
	if (!tags.isEmpty()) {
		QJsonArray arr;
		for (const QString &tag : tags)
			arr.append(tag);
		body.insert("tags", arr);
	}
	auto req = StreamInfoDock_Helix("https://api.twitch.tv/helix/channels?broadcaster_id=" + userId, clientId,
					accessToken);
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	Http(req, QJsonDocument(body).toJson(QJsonDocument::Compact), "PATCH",
	     [this](int status, const QByteArray &payload, const QString &) {
		     if (status >= 200 && status < 300)
			     SetResult(Tr("StreamInfoApplied"));
		     else
			     SetResult(JsonError(payload));
	     });
}

void StreamInfoDock::ApplyYouTube()
{
	if (ytBroadcastId.isEmpty()) {
		SetResult(Tr("StreamInfoNoBroadcast"));
		return;
	}
	QJsonObject snippet;
	snippet.insert("title", titleEdit->text());
	snippet.insert("description", descriptionEdit->toPlainText());
	if (!ytScheduledStart.isEmpty())
		snippet.insert("scheduledStartTime", ytScheduledStart);
	QJsonObject body;
	body.insert("id", ytBroadcastId);
	body.insert("snippet", snippet);
	QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
	QUrlQuery query;
	query.addQueryItem("part", "snippet");
	url.setQuery(query);
	QNetworkRequest req(url);
	req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	Http(req, QJsonDocument(body).toJson(QJsonDocument::Compact), "PUT",
	     [this](int status, const QByteArray &payload, const QString &) {
		     if (status < 200 || status >= 300) {
			     SetResult(JsonError(payload));
			     return;
		     }
		     QJsonObject videoSnippet;
		     videoSnippet.insert("title", titleEdit->text());
		     videoSnippet.insert("description", descriptionEdit->toPlainText());
		     if (!ytCategoryId.isEmpty())
			     videoSnippet.insert("categoryId", ytCategoryId);
		     else
			     videoSnippet.insert("categoryId", "20");
		     QJsonArray tags;
		     for (const QString &tag : Tags())
			     tags.append(tag);
		     videoSnippet.insert("tags", tags);
		     QJsonObject video;
		     video.insert("id", ytBroadcastId);
		     video.insert("snippet", videoSnippet);
		     QUrl vurl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"));
		     QUrlQuery vq;
		     vq.addQueryItem("part", "snippet");
		     vurl.setQuery(vq);
		     QNetworkRequest vreq(vurl);
		     vreq.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		     vreq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
		     Http(vreq, QJsonDocument(video).toJson(QJsonDocument::Compact), "PUT",
			  [this](int vstatus, const QByteArray &vbody, const QString &) {
				  if (vstatus >= 200 && vstatus < 300)
					  SetResult(Tr("StreamInfoApplied"));
				  else
					  SetResult(JsonError(vbody));
			  });
	     });
}

void StreamInfoDock::ApplyKick()
{
	QJsonObject body;
	body.insert("stream_title", titleEdit->text());
	if (!categoryId.isEmpty())
		body.insert("category_id", categoryId.toInt());
	const QStringList tags = Tags();
	if (!tags.isEmpty()) {
		QJsonArray arr;
		for (const QString &tag : tags)
			arr.append(tag);
		body.insert("custom_tags", arr);
	}
	QNetworkRequest req(QUrl(QStringLiteral("https://api.kick.com/public/v1/channels")));
	req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
	req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	Http(req, QJsonDocument(body).toJson(QJsonDocument::Compact), "PATCH",
	     [this](int status, const QByteArray &payload, const QString &) {
		     if (status >= 200 && status < 300) {
			     SetResult(Tr("StreamInfoApplied"));
			     return;
		     }
		     /* Kick's public API has rejected custom_tags. Retry title + category. */
		     if (!Tags().isEmpty()) {
			     QJsonObject retry;
			     retry.insert("stream_title", titleEdit->text());
			     if (!categoryId.isEmpty())
				     retry.insert("category_id", categoryId.toInt());
			     QNetworkRequest req2(QUrl(QStringLiteral("https://api.kick.com/public/v1/channels")));
			     req2.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
			     req2.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
			     Http(req2, QJsonDocument(retry).toJson(QJsonDocument::Compact), "PATCH",
				  [this](int status2, const QByteArray &body2, const QString &) {
					  if (status2 >= 200 && status2 < 300)
						  SetResult(Tr("StreamInfoAppliedNoTags"));
					  else
						  SetResult(JsonError(body2));
				  });
			     return;
		     }
		     SetResult(JsonError(payload));
	     });
}

void StreamInfoDock::DisconnectAccount()
{
	authGeneration++;
	pollTimer->stop();
	accessToken.clear();
	refreshToken.clear();
	expiresAt = 0;
	userId.clear();
	userName.clear();
	SavePlatform();
	SetConnected(false);
	SetResult(QString());
	connectButton->setEnabled(true);
}
