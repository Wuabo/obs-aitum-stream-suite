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
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QAbstractItemView>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolBar>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <functional>
#include <memory>
#include <src/utils/icon.hpp>
#include "output-dock.hpp"
#include "chat-dock.hpp"

extern obs_data_t *current_profile_config;
extern OutputDock *output_dock;
extern ChatDock *chat_dock;
void save_current_profile_config(bool save_docks);

static const int kRedirectPort = 17654;
static const char *kTwitchScopes = "channel:manage:broadcast chat:read chat:edit";
static const QSize kCategoryIconSize(24, 32);

/* liveBroadcasts.snippet.liveChatId — NOT activeLiveChatId (that is videos.liveStreamingDetails). */
static QString YouTubeBroadcastChatId(const QJsonObject &snippet)
{
	QString id = snippet.value(QStringLiteral("liveChatId")).toString();
	if (id.isEmpty())
		id = snippet.value(QStringLiteral("activeLiveChatId")).toString();
	return id;
}

/* YouTube often returns categoryId as a JSON number — QJsonValue::toString() is empty then. */
static QString JsonCategoryId(const QJsonValue &value)
{
	if (value.isDouble())
		return QString::number(value.toInteger());
	if (value.isString())
		return value.toString();
	return QString();
}

static QJsonObject PickYouTubeBroadcast(const QJsonArray &items)
{
	auto score = [](const QString &life) {
		if (life == QLatin1String("complete") || life == QLatin1String("revoked") ||
		    life == QLatin1String("abandoned"))
			return -1;
		if (life == QLatin1String("live") || life == QLatin1String("liveStarting"))
			return 5;
		if (life == QLatin1String("ready") || life == QLatin1String("testing") ||
		    life == QLatin1String("testStarting"))
			return 4;
		if (life == QLatin1String("created"))
			return 3;
		return 1;
	};
	QJsonObject best;
	int bestScore = -1;
	for (const QJsonValue &row : items) {
		const QJsonObject item = row.toObject();
		const QString life = item.value("status").toObject().value("lifeCycleStatus").toString();
		const int s = score(life);
		if (s < 0)
			continue;
		if (s > bestScore) {
			bestScore = s;
			best = item;
		}
	}
	return best;
}

class CategoryComboBox : public QComboBox {
public:
	using QComboBox::QComboBox;

	std::function<void()> onAboutToPopup;
	bool popupOpen() const { return view() && view()->isVisible(); }

protected:
	void showPopup() override
	{
		if (onAboutToPopup)
			onAboutToPopup();
		if (!view() || !screen()) {
			QComboBox::showPopup();
			return;
		}
		const QRect area = screen()->availableGeometry();
		const QPoint globalTop = mapToGlobal(QPoint(0, 0));
		const QPoint globalBottom = mapToGlobal(QPoint(0, height()));
		const int below = area.bottom() - globalBottom.y() - 8;
		const int above = globalTop.y() - area.top() - 8;
		int maxHeight = qMin(qMax(below, above), 280);
		if (maxHeight < 120)
			maxHeight = 120;
		const int row = qMax(iconSize().height() + 8, fontMetrics().height() + 8);
		setMaxVisibleItems(qMax(4, maxHeight / row));
		view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		QComboBox::showPopup();
	}
};

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
	liveLockTimer = new QTimer(this);
	liveLockTimer->setInterval(2000);
	connect(liveLockTimer, &QTimer::timeout, this, &StreamInfoDock::UpdateDisconnectState);
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
	auto *categories = new CategoryComboBox(content);
	categoryCombo = categories;
	categories->setIconSize(kCategoryIconSize);
	categories->setMaxVisibleItems(8);
	categories->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	categories->setMinimumContentsLength(18);
	categories->onAboutToPopup = [this] { PrefetchVisibleCategoryIcons(); };
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
	connect(categoryCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
		if (index < 0)
			return;
		categoryId = categoryCombo->itemData(index).toString();
		const QString name = categoryCombo->itemText(index).trimmed();
		if (!name.isEmpty())
			lastCategoryName = name;
	});

	UpdateFieldVisibility();
	SetConnected(false);
	RefreshPlatformList();
}

void StreamInfoDock::showEvent(QShowEvent *event)
{
	QFrame::showEvent(event);
	RefreshAvailablePlatforms();
}

void StreamInfoDock::RefreshAvailablePlatforms()
{
	const Platform previous = platform;
	RefreshPlatformList();
	if (platformCombo->count() == 0) {
		authGeneration++;
		pollTimer->stop();
		SetStatus(Tr("StreamInfoNoOutputs"));
		connectButton->setEnabled(false);
		disconnectButton->setEnabled(false);
		applyButton->setEnabled(false);
		return;
	}
	platform = Platform(platformCombo->currentData().toInt());
	if (platform != previous) {
		titleEdit->clear();
		categorySearch->clear();
		categoryCombo->clear();
		categoryCatalog.clear();
		categoryIcons.clear();
		categoryIconRequested.clear();
		categoryIconQueue.clear();
		descriptionEdit->clear();
		tagsEdit->clear();
		categoryId.clear();
		lastCategoryName.clear();
		ytBroadcastId.clear();
		SetResult(QString());
		LoadPlatform();
		UpdateFieldVisibility();
		if (!categoryId.isEmpty() && !lastCategoryName.isEmpty())
			PopulateCategoryCombo();
		if (!accessToken.isEmpty())
			EnsureToken([this](bool ok) {
				if (ok)
					RefreshChannel();
			});
		else
			SetConnected(false);
	} else {
		UpdateFieldVisibility();
		if (!accessToken.isEmpty()) {
			EnsureToken([this](bool ok) {
				if (ok || !accessToken.isEmpty())
					RefreshChannel();
				else
					SetConnected(false);
			});
		} else {
			SetConnected(false);
		}
	}
}

void StreamInfoDock::RefreshPlatformList()
{
	QSet<int> available;

	if (obs_service_t *service = obs_frontend_get_streaming_service()) {
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		const StreamPlatformKind kind = DetectStreamPlatformFromEndpoint(QString::fromUtf8(url ? url : ""));
		if (kind == StreamPlatformKind::Twitch)
			available.insert(int(Twitch));
		else if (kind == StreamPlatformKind::YouTube)
			available.insert(int(YouTube));
		else if (kind == StreamPlatformKind::Kick)
			available.insert(int(Kick));
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
					available.insert(int(Twitch));
				else if (kind == StreamPlatformKind::YouTube)
					available.insert(int(YouTube));
				else if (kind == StreamPlatformKind::Kick)
					available.insert(int(Kick));
			}
			obs_data_array_release(outputs);
		}
	}

	const int previous = platformCombo->count() > 0 ? platformCombo->currentData().toInt() : int(platform);
	platformCombo->blockSignals(true);
	platformCombo->clear();
	if (available.contains(int(Twitch)))
		platformCombo->addItem(QStringLiteral("Twitch"), int(Twitch));
	if (available.contains(int(YouTube)))
		platformCombo->addItem(QStringLiteral("YouTube"), int(YouTube));
	if (available.contains(int(Kick)))
		platformCombo->addItem(QStringLiteral("Kick"), int(Kick));

	int select = -1;
	for (int i = 0; i < platformCombo->count(); i++) {
		if (platformCombo->itemData(i).toInt() == previous) {
			select = i;
			break;
		}
	}
	if (select < 0 && platformCombo->count() > 0)
		select = 0;
	if (select >= 0)
		platformCombo->setCurrentIndex(select);
	platformCombo->blockSignals(false);
	platformCombo->setEnabled(platformCombo->count() > 0);
	if (select >= 0)
		platform = Platform(platformCombo->itemData(select).toInt());
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
	RefreshAvailablePlatforms();
	if (platformCombo->count() == 0)
		return;
	LoadPlatform();
	UpdateFieldVisibility();
	if (accessToken.isEmpty()) {
		SetConnected(false);
		return;
	}
	EnsureToken([this](bool ok) {
		if (ok || !accessToken.isEmpty())
			RefreshChannel();
		else
			SetConnected(false);
	});
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
	categoryCatalog.clear();
	categoryIcons.clear();
	categoryIconRequested.clear();
	categoryIconQueue.clear();
	descriptionEdit->clear();
	tagsEdit->clear();
	categoryId.clear();
	lastCategoryName.clear();
	ytBroadcastId.clear();
	SetResult(QString());
	LoadPlatform();
	UpdateFieldVisibility();
	if (!categoryId.isEmpty() && !lastCategoryName.isEmpty())
		PopulateCategoryCombo();
	if (accessToken.isEmpty()) {
		SetConnected(false);
		return;
	}
	EnsureToken([this](bool ok) {
		if (ok || !accessToken.isEmpty())
			RefreshChannel();
		else
			SetConnected(false);
	});
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
	lastCategoryName.clear();
	lastTitle.clear();
	lastTags.clear();
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
		lastCategoryName = QString::fromUtf8(obs_data_get_string(one, "category_name"));
		lastTitle = QString::fromUtf8(obs_data_get_string(one, "last_title"));
		lastTags = QString::fromUtf8(obs_data_get_string(one, "last_tags"));
		if (!categoryId.isEmpty() && !lastCategoryName.isEmpty())
			EnsureCategoryInCatalog(categoryId, lastCategoryName);
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
	if (platform == YouTube && !ytBroadcastId.isEmpty())
		obs_data_set_string(one, "youtube_video_id", ytBroadcastId.toUtf8().constData());
	obs_data_set_string(one, "category_id", categoryId.toUtf8().constData());
	if (!categoryId.isEmpty()) {
		if (lastCategoryName.isEmpty()) {
			for (const CategoryEntry &entry : categoryCatalog) {
				if (entry.id == categoryId && !entry.name.isEmpty()) {
					lastCategoryName = entry.name;
					break;
				}
			}
		}
		if (lastCategoryName.isEmpty() && categoryCombo && categoryCombo->currentIndex() >= 0 &&
		    categoryCombo->currentData().toString() == categoryId)
			lastCategoryName = categoryCombo->currentText().trimmed();
	} else {
		lastCategoryName.clear();
	}
	obs_data_set_string(one, "category_name", lastCategoryName.toUtf8().constData());
	if (titleEdit)
		obs_data_set_string(one, "last_title", titleEdit->text().trimmed().toUtf8().constData());
	if (tagsEdit)
		obs_data_set_string(one, "last_tags", tagsEdit->text().trimmed().toUtf8().constData());
	obs_data_set_obj(root, PlatformKey(platform), one);
	obs_data_set_obj(current_profile_config, "stream_accounts", root);
	obs_data_release(one);
	obs_data_release(root);
	save_current_profile_config(false);
	if (chat_dock)
		chat_dock->SyncAccountsFromConfig();
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
	if (auto *row = findChild<QFrame *>(QStringLiteral("streamInfoDescriptionRow")))
		row->setVisible(youtube);
	if (categorySearch)
		categorySearch->setPlaceholderText(Tr("StreamInfoCategorySearch"));
	QString endpoint = QStringLiteral("rtmp://live.twitch.tv/app");
	if (platform == YouTube)
		endpoint = QStringLiteral("rtmp://a.rtmp.youtube.com/live2");
	else if (platform == Kick)
		endpoint = QStringLiteral("rtmp://fa723fc1b171.global-contribute.live-video.net/app");
	if (platformIcon)
		platformIcon->setPixmap(getPlatformIconFromEndpoint(endpoint).pixmap(24, 24));
}

bool StreamInfoDock::IsCurrentPlatformLive() const
{
	auto matches = [this](const QString &endpoint) {
		const StreamPlatformKind kind = DetectStreamPlatformFromEndpoint(endpoint);
		if (platform == Twitch)
			return kind == StreamPlatformKind::Twitch;
		if (platform == YouTube)
			return kind == StreamPlatformKind::YouTube;
		return kind == StreamPlatformKind::Kick;
	};

	if (obs_frontend_streaming_active()) {
		if (obs_service_t *service = obs_frontend_get_streaming_service()) {
			const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
			if (matches(QString::fromUtf8(url ? url : "")))
				return true;
		}
	}
	return output_dock && output_dock->HasActiveStreamMatching(matches);
}

void StreamInfoDock::UpdateDisconnectState()
{
	const bool canConnect = !accountConnected;
	const bool canApply = accountConnected;
	const bool canDisconnect = accountConnected && !IsCurrentPlatformLive();
	if (connectButton->isEnabled() != canConnect)
		connectButton->setEnabled(canConnect);
	if (applyButton->isEnabled() != canApply)
		applyButton->setEnabled(canApply);
	if (disconnectButton->isEnabled() != canDisconnect)
		disconnectButton->setEnabled(canDisconnect);
}

void StreamInfoDock::SetConnected(bool connected)
{
	accountConnected = connected;
	if (connected && !userName.isEmpty())
		SetStatus(Tr("StreamInfoConnected").arg(userName));
	else if (connected)
		SetStatus(Tr("StreamInfoConnectedBare"));
	else
		SetStatus(Tr("StreamInfoNotConnected"));
	UpdateDisconnectState();
	if (connected)
		liveLockTimer->start();
	else
		liveLockTimer->stop();
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
		query.addQueryItem("scope", "user:read channel:read channel:write chat:write");
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

void StreamInfoDock::OnStreamingStarted()
{
	RefreshYouTubeForChat();
	/* Title/category only refresh when the Stream Info UI is on YouTube (uses that token). */
	if (platform == YouTube && !accessToken.isEmpty()) {
		LoadYouTubeBroadcast();
		QTimer::singleShot(3000, this, [this] {
			if (platform == YouTube && !accessToken.isEmpty())
				LoadYouTubeBroadcast();
		});
		QTimer::singleShot(10000, this, [this] {
			if (platform == YouTube && !accessToken.isEmpty())
				LoadYouTubeBroadcast();
		});
	}
}

void StreamInfoDock::RefreshYouTubeForChat()
{
	/* Use saved YouTube tokens even if the dock UI is on Twitch/Kick. */
	if (!current_profile_config)
		return;
	obs_data_t *root = obs_data_get_obj(current_profile_config, "stream_accounts");
	obs_data_t *one = root ? obs_data_get_obj(root, "youtube") : nullptr;
	const QString token = one ? QString::fromUtf8(obs_data_get_string(one, "access_token")) : QString();
	obs_data_release(one);
	obs_data_release(root);
	if (token.isEmpty())
		return;

	auto ytReq = [token](const QUrl &url) {
		QNetworkRequest req(url);
		req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
		return req;
	};

	auto acceptBroadcast = [this, ytReq](const QJsonObject &item) {
		const QString id = item.value("id").toString();
		if (id.isEmpty())
			return;
		const QString life = item.value("status").toObject().value("lifeCycleStatus").toString();
		if (life == QLatin1String("complete") || life == QLatin1String("revoked") ||
		    life == QLatin1String("abandoned"))
			return;
		if (platform == YouTube)
			ApplyYouTubeBroadcast(item);

		const QString snippetChat = YouTubeBroadcastChatId(item.value("snippet").toObject());

		QUrl vurl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"));
		QUrlQuery vq;
		vq.addQueryItem("part", "liveStreamingDetails");
		vq.addQueryItem("id", id);
		vurl.setQuery(vq);
		Http(ytReq(vurl), {}, "GET",
		     [id, snippetChat](int st, const QByteArray &payload, const QString &) {
			     if (!chat_dock)
				     return;
			     QString chat = snippetChat;
			     if (st >= 200 && st < 300) {
				     const QJsonArray vids =
					     QJsonDocument::fromJson(payload).object().value("items").toArray();
				     if (!vids.isEmpty()) {
					     const QString active = vids.at(0)
									    .toObject()
									    .value("liveStreamingDetails")
									    .toObject()
									    .value("activeLiveChatId")
									    .toString();
					     if (!active.isEmpty())
						     chat = active;
				     }
			     }
			     if (!chat.isEmpty())
				     chat_dock->NotifyYouTubeVideo(id, chat);
			     else
				     chat_dock->NotifyYouTubeVideo(id);
		     });
	};

	auto tryList = std::make_shared<std::function<void(const QString &)>>();
	*tryList = [this, ytReq, acceptBroadcast, tryList](const QString &status) {
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "snippet,status");
		if (status == QLatin1String("mine"))
			query.addQueryItem("mine", "true");
		else
			query.addQueryItem("broadcastStatus", status);
		query.addQueryItem("broadcastType", "all");
		query.addQueryItem("maxResults", "25");
		url.setQuery(query);
		Http(ytReq(url), {}, "GET", [this, status, acceptBroadcast, tryList](int code, const QByteArray &body,
										     const QString &) {
			if (code >= 200 && code < 300) {
				const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
				if (status == QLatin1String("mine")) {
					const QJsonObject best = PickYouTubeBroadcast(items);
					if (!best.isEmpty()) {
						acceptBroadcast(best);
						return;
					}
				} else if (!items.isEmpty()) {
					acceptBroadcast(items.at(0).toObject());
					return;
				}
			}
			if (status == QLatin1String("active"))
				(*tryList)(QStringLiteral("upcoming"));
			else if (status == QLatin1String("upcoming"))
				(*tryList)(QStringLiteral("mine"));
		});
	};
	(*tryList)(QStringLiteral("active"));
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
			if (gen != authGeneration)
				return;
			if (status2 >= 200 && status2 < 300) {
				const QJsonArray rows = QJsonDocument::fromJson(body2).object().value("data").toArray();
				if (!rows.isEmpty()) {
					const QJsonObject row = rows.at(0).toObject();
					titleEdit->setText(row.value("title").toString());
					categoryId = row.value("game_id").toString();
					const QString gameName = row.value("game_name").toString();
					QStringList tags;
					for (const QJsonValue &tag : row.value("tags").toArray())
						tags.append(tag.toString());
					tagsEdit->setText(tags.join(", "));
					if (!categoryId.isEmpty()) {
						if (!gameName.isEmpty())
							lastCategoryName = gameName;
						EnsureCategoryInCatalog(categoryId, lastCategoryName);
						PopulateCategoryCombo();
					}
				}
			}
			categorySearch->clear();
			LoadCategoryCatalog();
		});
	});
}

void StreamInfoDock::ApplyYouTubeBroadcast(const QJsonObject &item)
{
	ytBroadcastId = item.value("id").toString();
	const QJsonObject snippet = item.value("snippet").toObject();
	titleEdit->setText(snippet.value("title").toString());
	descriptionEdit->setPlainText(snippet.value("description").toString());
	SetResult(QString());
	const QString life = item.value("status").toObject().value("lifeCycleStatus").toString();
	const QString snippetChatId = YouTubeBroadcastChatId(snippet);
	const bool dead = life == QLatin1String("complete") || life == QLatin1String("revoked") ||
			  life == QLatin1String("abandoned");
	/* Pass snippet.liveChatId as soon as it exists (including ready). Chat retries until it opens. */
	if (!ytBroadcastId.isEmpty() && chat_dock && !dead) {
		if (!snippetChatId.isEmpty())
			chat_dock->NotifyYouTubeVideo(ytBroadcastId, snippetChatId);
		else
			chat_dock->NotifyYouTubeVideo(ytBroadcastId);
	}
	const int gen = authGeneration;
	QUrl videoUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"));
	QUrlQuery vq;
	vq.addQueryItem("part", "snippet,liveStreamingDetails");
	vq.addQueryItem("id", ytBroadcastId);
	videoUrl.setQuery(vq);
	QNetworkRequest vreq(videoUrl);
	vreq.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
	Http(vreq, {}, "GET", [this, gen](int, const QByteArray &vbody, const QString &) {
		if (gen != authGeneration)
			return;
		const QJsonArray videos = QJsonDocument::fromJson(vbody).object().value("items").toArray();
		if (!videos.isEmpty()) {
			const QJsonObject video = videos.at(0).toObject();
			const QJsonObject snip = video.value("snippet").toObject();
			ytCategoryId = JsonCategoryId(snip.value("categoryId"));
			categoryId = ytCategoryId;
			/* Do not reuse a stale display name (e.g. "Entretenimento" stuck on id 20). */
			lastCategoryName.clear();
			QStringList tags;
			for (const QJsonValue &tag : snip.value("tags").toArray())
				tags.append(tag.toString());
			tagsEdit->setText(tags.join(", "));
			if (!categoryId.isEmpty()) {
				EnsureCategoryInCatalog(categoryId, QString());
				PopulateCategoryCombo();
			}
			const QJsonObject details = video.value("liveStreamingDetails").toObject();
			const QString chatId = details.value("activeLiveChatId").toString();
			const bool ended = !details.value("actualEndTime").toString().isEmpty();
			if (!chatId.isEmpty() && chat_dock && !ended)
				chat_dock->NotifyYouTubeVideo(ytBroadcastId, chatId);
		}
		categorySearch->clear();
		LoadCategoryCatalog();
	});
}

static QJsonObject FirstDataObject(const QByteArray &body)
{
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (doc.isArray() && !doc.array().isEmpty())
		return doc.array().at(0).toObject();
	const QJsonObject root = doc.object();
	const QJsonValue data = root.value("data");
	if (data.isArray() && !data.toArray().isEmpty())
		return data.toArray().at(0).toObject();
	if (data.isObject())
		return data.toObject();
	return root;
}

static QString JsonId(const QJsonValue &value)
{
	if (value.isDouble())
		return QString::number(value.toInteger());
	return value.toString();
}

void StreamInfoDock::LoadYouTubeBroadcast()
{
	const int gen = authGeneration;
	auto ytGet = [this](const QUrl &url) {
		QNetworkRequest req(url);
		req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		return req;
	};
	auto loadChannelName = [this, gen, ytGet](std::function<void()> next) {
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/channels"));
		QUrlQuery query;
		query.addQueryItem("part", "snippet");
		query.addQueryItem("mine", "true");
		url.setQuery(query);
		Http(ytGet(url), {}, "GET", [this, gen, next](int code, const QByteArray &body, const QString &) {
			if (gen != authGeneration)
				return;
			if (code >= 200 && code < 300) {
				const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
				if (!items.isEmpty()) {
					userName = items.at(0).toObject().value("snippet").toObject().value("title").toString();
					userId = items.at(0).toObject().value("id").toString();
					SavePlatform();
					SetConnected(true);
				}
			}
			next();
		});
	};
	auto finishWithoutBroadcast = [this] {
		ytBroadcastId.clear();
		SetResult(Tr("StreamInfoNoBroadcast"));
		categorySearch->clear();
		LoadCategoryCatalog();
	};
	auto tryMine = [this, gen, ytGet, finishWithoutBroadcast] {
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "snippet,status");
		query.addQueryItem("mine", "true");
		query.addQueryItem("broadcastType", "all");
		query.addQueryItem("maxResults", "50");
		url.setQuery(query);
		Http(ytGet(url), {}, "GET", [this, gen, finishWithoutBroadcast](int code, const QByteArray &body, const QString &) {
			if (gen != authGeneration)
				return;
			if (code >= 200 && code < 300) {
				const QJsonObject best =
					PickYouTubeBroadcast(QJsonDocument::fromJson(body).object().value("items").toArray());
				if (!best.isEmpty()) {
					ApplyYouTubeBroadcast(best);
					return;
				}
			}
			finishWithoutBroadcast();
		});
	};
	auto requestStatus = [this, ytGet](const QString &status) {
		QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/liveBroadcasts"));
		QUrlQuery query;
		query.addQueryItem("part", "snippet,status");
		query.addQueryItem("broadcastStatus", status);
		query.addQueryItem("broadcastType", "all");
		query.addQueryItem("maxResults", "25");
		url.setQuery(query);
		return ytGet(url);
	};

	loadChannelName([this, gen, requestStatus, tryMine] {
		Http(requestStatus("active"), {}, "GET", [this, gen, requestStatus, tryMine](int code, const QByteArray &body,
											     const QString &) {
			if (gen != authGeneration)
				return;
			if (code >= 200 && code < 300) {
				const QJsonArray items = QJsonDocument::fromJson(body).object().value("items").toArray();
				if (!items.isEmpty()) {
					ApplyYouTubeBroadcast(items.at(0).toObject());
					return;
				}
			}
			Http(requestStatus("upcoming"), {}, "GET", [this, gen, tryMine](int code2, const QByteArray &body2,
											const QString &) {
				if (gen != authGeneration)
					return;
				if (code2 >= 200 && code2 < 300) {
					const QJsonArray upcoming =
						QJsonDocument::fromJson(body2).object().value("items").toArray();
					if (!upcoming.isEmpty()) {
						ApplyYouTubeBroadcast(upcoming.at(0).toObject());
						return;
					}
				}
				tryMine();
			});
		});
	});
}

static QNetworkRequest KickApiRequest(const QUrl &url, const QString &token)
{
	QNetworkRequest req(url);
	req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
	req.setRawHeader("Accept", "application/json");
	return req;
}

void StreamInfoDock::LoadKickChannel()
{
	const int gen = authGeneration;
	/* Kick often returns empty title/category while offline. Restore last applied values first. */
	if (!lastTitle.isEmpty() && titleEdit->text().isEmpty())
		titleEdit->setText(lastTitle);
	if (!lastTags.isEmpty() && tagsEdit->text().isEmpty())
		tagsEdit->setText(lastTags);
	if (!categoryId.isEmpty() && !lastCategoryName.isEmpty()) {
		EnsureCategoryInCatalog(categoryId, lastCategoryName);
		PopulateCategoryCombo();
	}

	Http(KickApiRequest(QUrl(QStringLiteral("https://api.kick.com/public/v1/users")), accessToken), {}, "GET",
	     [this, gen](int status, const QByteArray &body, const QString &) {
		     if (gen != authGeneration)
			     return;
		     if (status < 200 || status >= 300) {
			     SetResult(JsonError(body));
			     categorySearch->clear();
			     LoadCategoryCatalog();
			     return;
		     }
		     const QJsonObject user = FirstDataObject(body);
		     userId = JsonId(user.value("user_id").isUndefined() ? user.value("id") : user.value("user_id"));
		     userName = user.value("name").toString(user.value("username").toString());
		     SavePlatform();
		     SetConnected(true);

		     auto fillFromChannel = [this](const QJsonObject &channel) {
			     const QString title =
				     channel.value("stream_title").toString(channel.value("title").toString()).trimmed();
			     if (!title.isEmpty()) {
				     titleEdit->setText(title);
				     lastTitle = title;
			     }
			     const QJsonObject category = channel.value("category").toObject();
			     const QString id = JsonId(category.value("id"));
			     const QString name = category.value("name").toString().trimmed();
			     if (!id.isEmpty() && id != QLatin1String("0")) {
				     categoryId = id;
				     if (!name.isEmpty())
					     lastCategoryName = name;
				     EnsureCategoryInCatalog(id, lastCategoryName, category.value("thumbnail").toString());
				     PopulateCategoryCombo();
			     }
			     QStringList tags;
			     for (const QJsonValue &tag : channel.value("stream").toObject().value("custom_tags").toArray())
				     tags.append(tag.toString());
			     if (tags.isEmpty()) {
				     for (const QJsonValue &tag : channel.value("custom_tags").toArray())
					     tags.append(tag.toString());
			     }
			     if (tags.isEmpty()) {
				     for (const QJsonValue &tag : channel.value("tags").toArray())
					     tags.append(tag.toString());
			     }
			     if (!tags.isEmpty()) {
				     tagsEdit->setText(tags.join(", "));
				     lastTags = tagsEdit->text();
			     }
		     };

		     auto finishLoad = [this] {
			     categorySearch->clear();
			     LoadCategoryCatalog();
		     };

		     auto loadLivestream = [this, gen, fillFromChannel, finishLoad] {
			     if (userId.isEmpty()) {
				     finishLoad();
				     return;
			     }
			     QUrl liveUrl(QStringLiteral("https://api.kick.com/public/v1/users/livestreams"));
			     QUrlQuery lq;
			     lq.addQueryItem("user_id", userId);
			     liveUrl.setQuery(lq);
			     Http(KickApiRequest(liveUrl, accessToken), {}, "GET",
				  [this, gen, fillFromChannel, finishLoad](int liveCode, const QByteArray &liveBody,
									   const QString &) {
					  if (gen != authGeneration)
						  return;
					  if (liveCode >= 200 && liveCode < 300) {
						  const QJsonObject live = FirstDataObject(liveBody);
						  if (!live.isEmpty())
							  fillFromChannel(live);
					  }
					  finishLoad();
				  });
		     };

		     auto requestChannel = [this, gen, fillFromChannel, loadLivestream](const QUrl &url, bool thenLivestream) {
			     Http(KickApiRequest(url, accessToken), {}, "GET",
				  [this, gen, fillFromChannel, loadLivestream, thenLivestream](int code, const QByteArray &payload,
											      const QString &) {
					  if (gen != authGeneration)
						  return;
					  if (code >= 200 && code < 300)
						  fillFromChannel(FirstDataObject(payload));
					  else if (!thenLivestream)
						  SetResult(JsonError(payload));
					  if (thenLivestream)
						  loadLivestream();
				  });
		     };

		     /* Prefer authenticated channel (no params), then explicit broadcaster id. */
		     Http(KickApiRequest(QUrl(QStringLiteral("https://api.kick.com/public/v1/channels")), accessToken), {}, "GET",
			  [this, gen, fillFromChannel, loadLivestream, requestChannel](int code, const QByteArray &payload,
										       const QString &) {
				  if (gen != authGeneration)
					  return;
				  if (code >= 200 && code < 300) {
					  fillFromChannel(FirstDataObject(payload));
					  loadLivestream();
					  return;
				  }
				  if (!userId.isEmpty()) {
					  QUrl channelUrl(QStringLiteral("https://api.kick.com/public/v1/channels"));
					  QUrlQuery cq;
					  cq.addQueryItem("broadcaster_user_id", userId);
					  channelUrl.setQuery(cq);
					  requestChannel(channelUrl, true);
					  return;
				  }
				  SetResult(JsonError(payload));
				  loadLivestream();
			  });
	     });
}

static QString NormalizeBoxArt(QString url)
{
	url.replace(QStringLiteral("{width}"), QStringLiteral("52"));
	url.replace(QStringLiteral("{height}"), QStringLiteral("72"));
	return url;
}

void StreamInfoDock::EnsureCategoryInCatalog(const QString &id, const QString &name, const QString &boxArt)
{
	if (id.isEmpty())
		return;
	for (CategoryEntry &entry : categoryCatalog) {
		if (entry.id != id)
			continue;
		if (!name.isEmpty())
			entry.name = name;
		if (!boxArt.isEmpty())
			entry.boxArt = boxArt;
		return;
	}
	categoryCatalog.prepend({id, name, boxArt});
}

void StreamInfoDock::PumpCategoryIconQueue()
{
	static const int kMaxInFlight = 2;
	while (categoryIconInFlight < kMaxInFlight && !categoryIconQueue.isEmpty()) {
		const QString id = categoryIconQueue.takeFirst();
		QString boxArt;
		for (const CategoryEntry &entry : categoryCatalog) {
			if (entry.id == id) {
				boxArt = entry.boxArt;
				break;
			}
		}
		if (id.isEmpty() || boxArt.isEmpty() || categoryIcons.contains(id))
			continue;
		const QUrl url(NormalizeBoxArt(boxArt));
		if (!url.isValid())
			continue;
		categoryIconInFlight++;
		Http(QNetworkRequest(url), {}, "GET", [this, id](int status, const QByteArray &body, const QString &) {
			categoryIconInFlight = qMax(0, categoryIconInFlight - 1);
			if (status >= 200 && status < 300 && !body.isEmpty()) {
				QPixmap pixmap;
				if (pixmap.loadFromData(body)) {
					const QIcon icon(
						pixmap.scaled(kCategoryIconSize, Qt::KeepAspectRatio, Qt::FastTransformation));
					categoryIcons.insert(id, icon);
					for (int i = 0; i < categoryCombo->count(); ++i) {
						if (categoryCombo->itemData(i).toString() == id) {
							categoryCombo->setItemIcon(i, icon);
							break;
						}
					}
				}
			}
			PumpCategoryIconQueue();
		});
	}
}

void StreamInfoDock::FetchCategoryIcon(const QString &id, const QString &boxArt)
{
	if (id.isEmpty() || boxArt.isEmpty() || categoryIcons.contains(id) || categoryIconRequested.contains(id))
		return;
	categoryIconRequested.insert(id);
	categoryIconQueue.append(id);
	PumpCategoryIconQueue();
}

void StreamInfoDock::PrefetchVisibleCategoryIcons()
{
	const int limit = qMin(categoryCombo->count(), qMax(8, categoryCombo->maxVisibleItems() + 4));
	int queued = 0;
	for (int i = 0; i < categoryCombo->count() && queued < limit; ++i) {
		const QString id = categoryCombo->itemData(i).toString();
		if (id.isEmpty() || categoryIcons.contains(id) || categoryIconRequested.contains(id))
			continue;
		QString boxArt;
		for (const CategoryEntry &entry : categoryCatalog) {
			if (entry.id == id) {
				boxArt = entry.boxArt;
				break;
			}
		}
		if (boxArt.isEmpty())
			continue;
		FetchCategoryIcon(id, boxArt);
		queued++;
	}
}

void StreamInfoDock::PopulateCategoryCombo()
{
	auto *combo = static_cast<CategoryComboBox *>(categoryCombo);
	if (combo && combo->popupOpen())
		return;

	static const int kMaxComboItems = 80;
	const QString filter = categorySearch->text().trimmed();
	const QString selected = categoryId;
	QString selectedName = lastCategoryName;
	QString selectedArt;
	for (const CategoryEntry &entry : categoryCatalog) {
		if (entry.id != selected)
			continue;
		if (!entry.name.isEmpty())
			selectedName = entry.name;
		selectedArt = entry.boxArt;
		break;
	}
	if (!selected.isEmpty() && !selectedName.isEmpty())
		lastCategoryName = selectedName;

	categoryCombo->setUpdatesEnabled(false);
	categoryCombo->blockSignals(true);
	categoryCombo->clear();
	int selectIndex = -1;
	/* Selected category always first so the field shows the current value. */
	if (!selected.isEmpty() && (filter.isEmpty() || selectedName.contains(filter, Qt::CaseInsensitive))) {
		categoryCombo->addItem(categoryIcons.value(selected),
				       selectedName.isEmpty() ? selected : selectedName, selected);
		selectIndex = 0;
	}
	for (const CategoryEntry &entry : categoryCatalog) {
		if (categoryCombo->count() >= kMaxComboItems)
			break;
		if (entry.id == selected)
			continue;
		if (!filter.isEmpty() && !entry.name.contains(filter, Qt::CaseInsensitive))
			continue;
		if (entry.name.isEmpty())
			continue;
		categoryCombo->addItem(categoryIcons.value(entry.id), entry.name, entry.id);
	}
	if (selectIndex >= 0)
		categoryCombo->setCurrentIndex(selectIndex);
	categoryCombo->blockSignals(false);
	categoryCombo->setUpdatesEnabled(true);
	if (categoryCombo->currentIndex() >= 0)
		categoryId = categoryCombo->currentData().toString();
	if (!selectedArt.isEmpty())
		FetchCategoryIcon(selected, selectedArt);
	else {
		for (const CategoryEntry &entry : categoryCatalog) {
			if (entry.id == categoryId) {
				FetchCategoryIcon(entry.id, entry.boxArt);
				break;
			}
		}
	}
}

void StreamInfoDock::AppendCategoriesFromJson(const QJsonArray &rows, const char *idKey, const char *nameKey,
					      const char *artKey)
{
	static const int kMaxCatalog = 200;
	QHash<QString, int> indexById;
	indexById.reserve(categoryCatalog.size() + rows.size());
	for (int i = 0; i < categoryCatalog.size(); ++i)
		indexById.insert(categoryCatalog.at(i).id, i);

	for (const QJsonValue &row : rows) {
		const QJsonObject obj = row.toObject();
		const QJsonValue idValue = obj.value(QLatin1String(idKey));
		const QString id = idValue.isDouble() ? QString::number(idValue.toInteger()) : idValue.toString();
		QString name = obj.value(QLatin1String(nameKey)).toString();
		if (name.isEmpty() && obj.value("snippet").isObject())
			name = obj.value("snippet").toObject().value("title").toString();
		QString art = artKey ? obj.value(QLatin1String(artKey)).toString() : QString();
		if (art.isEmpty() && obj.value("snippet").isObject()) {
			const QJsonObject thumbs = obj.value("snippet").toObject().value("thumbnails").toObject();
			art = thumbs.value("default").toObject().value("url").toString();
		}
		if (platform == YouTube && obj.value("snippet").isObject()) {
			const QJsonValue assignable = obj.value("snippet").toObject().value("assignable");
			if (assignable.isBool() && !assignable.toBool())
				continue;
		}
		if (id.isEmpty() || name.isEmpty())
			continue;
		const auto it = indexById.constFind(id);
		if (it != indexById.cend()) {
			CategoryEntry &entry = categoryCatalog[it.value()];
			entry.name = name;
			if (!art.isEmpty())
				entry.boxArt = art;
			if (id == categoryId)
				lastCategoryName = name;
			continue;
		}
		if (categoryCatalog.size() >= kMaxCatalog)
			continue;
		indexById.insert(id, categoryCatalog.size());
		categoryCatalog.append({id, name, art});
		if (id == categoryId)
			lastCategoryName = name;
	}
}

void StreamInfoDock::LoadCategoryCatalog()
{
	if (accessToken.isEmpty())
		return;
	EnsureToken([this](bool ok) {
		if (!ok)
			return;
		const QString keepId = categoryId;
		QString keepName = lastCategoryName;
		QString keepArt;
		for (const CategoryEntry &entry : categoryCatalog) {
			if (entry.id == keepId) {
				if (!entry.name.isEmpty())
					keepName = entry.name;
				keepArt = entry.boxArt;
				break;
			}
		}
		auto finish = [this, keepId, keepName, keepArt] {
			if (!keepId.isEmpty()) {
				/* Prefer the official catalog title over a stale/wrong keepName. */
				QString catalogName;
				for (const CategoryEntry &entry : categoryCatalog) {
					if (entry.id == keepId && !entry.name.isEmpty()) {
						catalogName = entry.name;
						break;
					}
				}
				const QString useName = !catalogName.isEmpty()
								 ? catalogName
								 : (!keepName.isEmpty() ? keepName : lastCategoryName);
				EnsureCategoryInCatalog(keepId, useName, keepArt);
				if (!useName.isEmpty())
					lastCategoryName = useName;
			}
			PopulateCategoryCombo();
		};
		if (platform == Twitch) {
			auto req = StreamInfoDock_Helix("https://api.twitch.tv/helix/games/top?first=50", clientId, accessToken);
			Http(req, {}, "GET", [this, keepId, keepName, keepArt, finish](int status, const QByteArray &body,
										       const QString &) {
				if (status < 200 || status >= 300) {
					finish();
					return;
				}
				categoryCatalog.clear();
				if (!keepId.isEmpty())
					EnsureCategoryInCatalog(keepId, keepName, keepArt);
				AppendCategoriesFromJson(QJsonDocument::fromJson(body).object().value("data").toArray(), "id",
							 "name", "box_art_url");
				const bool needLookup = !keepId.isEmpty() && (keepName.isEmpty() || keepArt.isEmpty());
				if (needLookup) {
					auto gameReq = StreamInfoDock_Helix(
						"https://api.twitch.tv/helix/games?id=" +
							QString::fromUtf8(QUrl::toPercentEncoding(keepId)),
						clientId, accessToken);
					Http(gameReq, {}, "GET", [this, keepId, finish](int code, const QByteArray &payload,
											const QString &) {
						if (code >= 200 && code < 300) {
							const QJsonArray data = QJsonDocument::fromJson(payload)
											.object()
											.value("data")
											.toArray();
							if (!data.isEmpty()) {
								const QJsonObject obj = data.at(0).toObject();
								const QString name = obj.value("name").toString();
								if (!name.isEmpty())
									lastCategoryName = name;
								EnsureCategoryInCatalog(keepId, name,
											obj.value("box_art_url").toString());
							}
						}
						finish();
					});
					return;
				}
				finish();
			});
			return;
		}
		if (platform == YouTube) {
			QUrl url(QStringLiteral("https://www.googleapis.com/youtube/v3/videoCategories"));
			QUrlQuery query;
			query.addQueryItem("part", "snippet");
			const QString locale = QLocale::system().name();
			QString region = locale.section(QLatin1Char('_'), 1, 1);
			if (region.size() != 2)
				region = QStringLiteral("PT");
			QString hl = locale.section(QLatin1Char('_'), 0, 0);
			if (hl.isEmpty())
				hl = QStringLiteral("pt");
			query.addQueryItem("regionCode", region.toUpper());
			query.addQueryItem("hl", hl);
			url.setQuery(query);
			QNetworkRequest req(url);
			req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
			Http(req, {}, "GET", [this, finish, region, hl](int status, const QByteArray &body, const QString &) {
				if (status < 200 || status >= 300) {
					/* Retry with US if the system region is rejected. */
					if (region.toUpper() != QStringLiteral("US")) {
						QUrl retry(QStringLiteral("https://www.googleapis.com/youtube/v3/videoCategories"));
						QUrlQuery rq;
						rq.addQueryItem("part", "snippet");
						rq.addQueryItem("regionCode", QStringLiteral("US"));
						rq.addQueryItem("hl", hl);
						retry.setQuery(rq);
						QNetworkRequest rreq(retry);
						rreq.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
						Http(rreq, {}, "GET", [this, finish](int code, const QByteArray &payload, const QString &) {
							if (code < 200 || code >= 300) {
								SetResult(JsonError(payload));
								return;
							}
							categoryCatalog.clear();
							AppendCategoriesFromJson(
								QJsonDocument::fromJson(payload).object().value("items").toArray(),
								"id", "name", nullptr);
							finish();
						});
						return;
					}
					SetResult(JsonError(body));
					return;
				}
				categoryCatalog.clear();
				AppendCategoriesFromJson(QJsonDocument::fromJson(body).object().value("items").toArray(), "id",
							 "name", nullptr);
				finish();
			});
			return;
		}
		/* Small seed only — full Kick catalogs freeze the dock; search loads more on demand. */
		categoryCatalog.clear();
		if (!keepId.isEmpty())
			EnsureCategoryInCatalog(keepId, keepName, keepArt);
		PopulateCategoryCombo();
		auto seedPage = [this, finish](const QString &q) {
			QUrl url(QStringLiteral("https://api.kick.com/public/v1/categories"));
			QUrlQuery query;
			query.addQueryItem("q", q);
			query.addQueryItem("page", QStringLiteral("1"));
			url.setQuery(query);
			Http(KickApiRequest(url, accessToken), {}, "GET",
			     [this, finish](int status, const QByteArray &body, const QString &) {
				     if (status >= 200 && status < 300) {
					     const QJsonDocument doc = QJsonDocument::fromJson(body);
					     const QJsonArray rows = doc.object().value("data").isArray()
									     ? doc.object().value("data").toArray()
									     : doc.array();
					     AppendCategoriesFromJson(rows, "id", "name", "thumbnail");
				     }
				     if (categoryCatalog.isEmpty())
					     SetResult(Tr("StreamInfoKickCategoriesFailed"));
				     finish();
			     });
		};
		if (!keepId.isEmpty() && keepName.isEmpty()) {
			QUrl byId(QStringLiteral("https://api.kick.com/public/v1/categories/") +
				  QString::fromUtf8(QUrl::toPercentEncoding(keepId)));
			Http(KickApiRequest(byId, accessToken), {}, "GET",
			     [this, keepId, seedPage, finish](int status, const QByteArray &body, const QString &) {
				     if (status >= 200 && status < 300) {
					     const QJsonObject obj = FirstDataObject(body);
					     const QString name = obj.value("name").toString();
					     if (!name.isEmpty()) {
						     lastCategoryName = name;
						     EnsureCategoryInCatalog(keepId, name, obj.value("thumbnail").toString());
						     PopulateCategoryCombo();
					     }
				     }
				     seedPage(lastCategoryName.isEmpty() ? QStringLiteral("a") : lastCategoryName.left(1));
			     });
			return;
		}
		seedPage(keepName.isEmpty() ? QStringLiteral("a") : keepName.left(1));
	});
}

void StreamInfoDock::SearchCategories()
{
	PopulateCategoryCombo();
	const QString queryText = categorySearch->text().trimmed();
	if (queryText.size() < 2 || accessToken.isEmpty() || platform == YouTube)
		return;
	EnsureToken([this, queryText](bool ok) {
		if (!ok || categorySearch->text().trimmed() != queryText)
			return;
		if (platform == Twitch) {
			auto req = StreamInfoDock_Helix(
				"https://api.twitch.tv/helix/search/categories?first=20&query=" +
					QString::fromUtf8(QUrl::toPercentEncoding(queryText)),
				clientId, accessToken);
			Http(req, {}, "GET", [this, queryText](int status, const QByteArray &body, const QString &) {
				if (status < 200 || status >= 300 || categorySearch->text().trimmed() != queryText)
					return;
				AppendCategoriesFromJson(QJsonDocument::fromJson(body).object().value("data").toArray(), "id",
							 "name", "box_art_url");
				PopulateCategoryCombo();
			});
			return;
		}
		QUrl url(QStringLiteral("https://api.kick.com/public/v1/categories"));
		QUrlQuery query;
		query.addQueryItem("q", queryText);
		query.addQueryItem("page", QStringLiteral("1"));
		url.setQuery(query);
		QNetworkRequest req(url);
		req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		Http(req, {}, "GET", [this, queryText](int status, const QByteArray &body, const QString &) {
			if (status < 200 || status >= 300 || categorySearch->text().trimmed() != queryText)
				return;
			const QJsonDocument doc = QJsonDocument::fromJson(body);
			const QJsonArray rows =
				doc.object().value("data").isArray() ? doc.object().value("data").toArray() : doc.array();
			AppendCategoriesFromJson(rows, "id", "name", "thumbnail");
			PopulateCategoryCombo();
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
	/* Always take the combo selection at apply time. */
	QString wantedCategory;
	QString wantedName;
	if (categoryCombo && categoryCombo->currentIndex() >= 0) {
		wantedCategory = categoryCombo->itemData(categoryCombo->currentIndex()).toString();
		if (wantedCategory.isEmpty())
			wantedCategory = categoryCombo->itemData(categoryCombo->currentIndex(), Qt::UserRole).toString();
		wantedName = categoryCombo->currentText().trimmed();
	}
	if (wantedCategory.isEmpty())
		wantedCategory = categoryId;

	blog(LOG_INFO, "[Stream Suite] YT-apply video=%s category=%s (%s)", ytBroadcastId.toUtf8().constData(),
	     wantedCategory.toUtf8().constData(), wantedName.toUtf8().constData());

	/* GET current snippet so we preserve tags/description fields YouTube would otherwise clear. */
	QUrl getUrl(QStringLiteral("https://www.googleapis.com/youtube/v3/videos"));
	QUrlQuery gq;
	gq.addQueryItem("part", "snippet,liveStreamingDetails");
	gq.addQueryItem("id", ytBroadcastId);
	getUrl.setQuery(gq);
	QNetworkRequest greq(getUrl);
	greq.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
	Http(greq, {}, "GET", [this, wantedCategory, wantedName](int gstatus, const QByteArray &gbody, const QString &) {
		if (gstatus < 200 || gstatus >= 300) {
			blog(LOG_WARNING, "[Stream Suite] YouTube videos.list before update HTTP %d: %s", gstatus,
			     gbody.left(300).constData());
			SetResult(JsonError(gbody));
			return;
		}
		const QJsonArray items = QJsonDocument::fromJson(gbody).object().value("items").toArray();
		if (items.isEmpty()) {
			SetResult(Tr("StreamInfoNoBroadcast"));
			return;
		}
		const QJsonObject current = items.at(0).toObject();
		const QJsonObject curSnip = current.value("snippet").toObject();
		const QString existingCategory = JsonCategoryId(curSnip.value("categoryId"));
		const QString activeChat =
			current.value("liveStreamingDetails").toObject().value("activeLiveChatId").toString();
		if (chat_dock) {
			if (!activeChat.isEmpty())
				chat_dock->NotifyYouTubeVideo(ytBroadcastId, activeChat);
			else
				chat_dock->NotifyYouTubeVideo(ytBroadcastId);
		}

		auto looksLikeYouTubeCategory = [](const QString &id) {
			if (id.isEmpty())
				return false;
			bool ok = false;
			const int n = id.toInt(&ok);
			/* Real YouTube video category ids are small (1–44). Twitch game ids are huge. */
			return ok && n > 0 && n < 100;
		};

		QString useCategory = wantedCategory;
		if (!looksLikeYouTubeCategory(useCategory)) {
			SetResult(QStringLiteral("Invalid YouTube category '%1' — pick one from the list")
					  .arg(wantedCategory.isEmpty() ? QStringLiteral("(empty)") : wantedCategory));
			return;
		}
		ytCategoryId = useCategory;
		categoryId = useCategory;
		if (!wantedName.isEmpty())
			lastCategoryName = wantedName;

		QJsonObject videoSnippet;
		videoSnippet.insert("title", titleEdit->text().trimmed().isEmpty() ? curSnip.value("title").toString()
										  : titleEdit->text());
		videoSnippet.insert("description", descriptionEdit->toPlainText());
		videoSnippet.insert("categoryId", useCategory);
		QJsonArray tags;
		for (const QString &tag : Tags())
			tags.append(tag);
		if (!tags.isEmpty())
			videoSnippet.insert("tags", tags);
		else if (curSnip.value("tags").isArray())
			videoSnippet.insert("tags", curSnip.value("tags").toArray());

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
		     [this, useCategory, wantedName, existingCategory](int vstatus, const QByteArray &vbody, const QString &) {
			     if (vstatus >= 200 && vstatus < 300) {
				     QString confirmed = JsonCategoryId(QJsonDocument::fromJson(vbody)
										.object()
										.value("snippet")
										.toObject()
										.value("categoryId"));
				     if (confirmed.isEmpty())
					     confirmed = useCategory;
				     categoryId = confirmed;
				     ytCategoryId = confirmed;
				     /* Refresh label from catalog so we never show a wrong name for the id. */
				     QString name = wantedName;
				     for (const CategoryEntry &entry : categoryCatalog) {
					     if (entry.id == confirmed && !entry.name.isEmpty()) {
						     name = entry.name;
						     break;
					     }
				     }
				     if (!name.isEmpty())
					     lastCategoryName = name;
				     EnsureCategoryInCatalog(confirmed, name);
				     PopulateCategoryCombo();
				     QString status = Tr("StreamInfoApplied");
				     status += QStringLiteral(" · %1").arg(name.isEmpty() ? confirmed : name);
				     if (confirmed != existingCategory)
					     status += QStringLiteral(" (%1 → %2)").arg(existingCategory, confirmed);
				     SetResult(status);
			     } else {
				     blog(LOG_WARNING, "[Stream Suite] YouTube videos.update HTTP %d: %s", vstatus,
					  vbody.left(300).constData());
				     SetResult(JsonError(vbody));
			     }
		     });
	});
}

void StreamInfoDock::ApplyKick()
{
	const QString title = titleEdit->text().trimmed();
	const qint64 category = categoryId.toLongLong();
	const QStringList tags = Tags();
	auto buildBody = [&](bool includeTags) {
		QJsonObject body;
		/* Kick rejects empty stream_title (minLength 1). Omit when blank. */
		if (!title.isEmpty())
			body.insert("stream_title", title);
		if (category > 0)
			body.insert("category_id", category);
		if (includeTags && !tags.isEmpty()) {
			QJsonArray arr;
			for (const QString &tag : tags)
				arr.append(tag);
			body.insert("custom_tags", arr);
		}
		return body;
	};
	QJsonObject body = buildBody(true);
	if (body.isEmpty()) {
		SetResult(Tr("StreamInfoKickEmpty"));
		return;
	}
	auto sendPatch = [this](const QJsonObject &payloadBody,
				std::function<void(int, const QByteArray &)> done) {
		QNetworkRequest req(QUrl(QStringLiteral("https://api.kick.com/public/v1/channels")));
		req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
		req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
		Http(req, QJsonDocument(payloadBody).toJson(QJsonDocument::Compact), "PATCH",
		     [done](int status, const QByteArray &payload, const QString &) { done(status, payload); });
	};
	sendPatch(body, [this, buildBody, sendPatch, tags](int status, const QByteArray &payload) {
		if (status >= 200 && status < 300) {
			SetResult(Tr("StreamInfoApplied"));
			RefreshChannel();
			return;
		}
		if (!tags.isEmpty()) {
			const QJsonObject retry = buildBody(false);
			if (!retry.isEmpty()) {
				sendPatch(retry, [this](int status2, const QByteArray &body2) {
					if (status2 >= 200 && status2 < 300) {
						SetResult(Tr("StreamInfoAppliedNoTags"));
						RefreshChannel();
					} else {
						SetResult(JsonError(body2));
					}
				});
				return;
			}
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
	categoryCatalog.clear();
	categoryIcons.clear();
	categoryIconRequested.clear();
	categoryIconQueue.clear();
	categoryCombo->clear();
	categorySearch->clear();
	SavePlatform();
	SetConnected(false);
	SetResult(QString());
}
