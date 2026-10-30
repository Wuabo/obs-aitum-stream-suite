#pragma once

#include <QByteArray>
#include <QFrame>
#include <QHash>
#include <QIcon>
#include <QSet>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QVector>
#include <functional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QTcpServer;
class QTimer;

/* Native stream title / category / tags. Tokens stay in the profile config.
 * Each platform still requires that platform's OAuth app (client id). */
class StreamInfoDock : public QFrame {
	Q_OBJECT

public:
	explicit StreamInfoDock(QWidget *parent = nullptr);
	~StreamInfoDock() override;

	void ReloadAccounts();
	void RefreshAvailablePlatforms();
	/* Start OAuth again for moderation scopes. Keeps the current token until the new one arrives. */
	void RequestModerationAuth(int platformId);
	void UpdateDisconnectState();
	void OpenSettings();
	/* Reload YouTube broadcast (if YouTube account linked) and push liveChatId to chat. */
	void RefreshYouTubeForChat();
	/* OBS went live — reload title/category and rediscover chat (YouTube is slow to flip ready→live). */
	void OnStreamingStarted();

protected:
	void showEvent(QShowEvent *event) override;

private:
	enum Platform { Twitch, YouTube, Kick };

	QTcpServer *authServer = nullptr;
	QTimer *pollTimer = nullptr;
	int authGeneration = 0;
	int pollIntervalMs = 5000;
	QString deviceCode;
	QByteArray codeVerifier;
	QString oauthState;

	Platform platform = Twitch;
	QString clientId;
	QString clientSecret;
	QString accessToken;
	QString refreshToken;
	qint64 expiresAt = 0;
	QString userId;
	QString userName;
	QString categoryId;
	QString lastCategoryName;
	QString lastTitle;
	QString lastTags;
	QString ytBroadcastId;
	QString ytCategoryId;

	QLabel *platformIcon = nullptr;
	QComboBox *platformCombo = nullptr;
	QPushButton *connectButton = nullptr;
	QPushButton *disconnectButton = nullptr;
	QLabel *statusLabel = nullptr;
	QLineEdit *titleEdit = nullptr;
	QLineEdit *categorySearch = nullptr;
	QComboBox *categoryCombo = nullptr;
	QPlainTextEdit *descriptionEdit = nullptr;
	QComboBox *privacyCombo = nullptr;
	QComboBox *latencyCombo = nullptr;
	QCheckBox *kidsCheck = nullptr;
	QPushButton *endYouTubeButton = nullptr;
	QLineEdit *tagsEdit = nullptr;
	QPushButton *applyButton = nullptr;
	QLabel *resultLabel = nullptr;
	QTimer *searchTimer = nullptr;
	QTimer *liveLockTimer = nullptr;
	bool accountConnected = false;
	bool authInProgress = false;

	struct CategoryEntry {
		QString id;
		QString name;
		QString boxArt;
	};
	QVector<CategoryEntry> categoryCatalog;
	QHash<QString, QIcon> categoryIcons;
	QSet<QString> categoryIconRequested;
	QStringList categoryIconQueue;
	int categoryIconInFlight = 0;

	static const char *PlatformKey(Platform platform);
	void BuildUi();
	void RefreshPlatformList();
	void SwitchPlatform(int index);
	void LoadPlatform();
	void SavePlatform();
	void SetStatus(const QString &text);
	void SetResult(const QString &text);
	void UpdateFieldVisibility();
	void SetConnected(bool connected);
	bool IsCurrentPlatformLive() const;
	bool TokenFresh() const;
	void EnsureToken(std::function<void(bool)> done);
	void BeginConnect();
	void BeginTwitchDevice();
	void PollTwitchDevice();
	void BeginAuthCode();
	void FinishAuthCode(const QString &code);
	void ExchangeAuthCode(const QString &code);
	void AcceptToken(const QJsonObject &json);
	void RefreshChannel();
	void LoadTwitchChannel();
	void LoadYouTubeBroadcast();
	void ApplyYouTubeBroadcast(const QJsonObject &item);
	void LoadKickChannel();
	void LoadCategoryCatalog();
	void AppendCategoriesFromJson(const QJsonArray &rows, const char *idKey, const char *nameKey, const char *artKey);
	void EnsureCategoryInCatalog(const QString &id, const QString &name, const QString &boxArt = QString());
	void PopulateCategoryCombo();
	void PrefetchVisibleCategoryIcons();
	void FetchCategoryIcon(const QString &id, const QString &boxArt);
	void PumpCategoryIconQueue();
	void SearchCategories();
	void Apply();
	void ApplyTwitch();
	void ApplyYouTube();
	void UpdateYouTubeBroadcastSettings(const QString &appliedStatus);
	void EndYouTubeBroadcast();
	void ApplyKick();
	void DisconnectAccount();
	QStringList Tags() const;

	void Http(const QNetworkRequest &request, const QByteArray &body, const char *verb,
		  std::function<void(int status, const QByteArray &body, const QString &error)> done);
};
