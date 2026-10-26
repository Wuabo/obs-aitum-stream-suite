#pragma once

#include <QFrame>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

struct ChatCurlWsSession {
	std::atomic<bool> stop{false};
	std::mutex mu;
	std::vector<QByteArray> outbound;

	void enqueue(QByteArray payload)
	{
		std::lock_guard<std::mutex> lock(mu);
		outbound.push_back(std::move(payload));
	}

	std::vector<QByteArray> takeOutbound()
	{
		std::lock_guard<std::mutex> lock(mu);
		std::vector<QByteArray> out;
		out.swap(outbound);
		return out;
	}
};

/* Unified multi-platform chat + per-platform viewer counts.
 * Uses the same stream_accounts OAuth tokens as Stream Info.
 * Twitch/Kick realtime uses libcurl WebSockets (OBS Qt has no SSL plugins). */
class ChatDock : public QFrame {
	Q_OBJECT

public:
	explicit ChatDock(QWidget *parent = nullptr);
	~ChatDock() override;

	void ReloadAccounts();
	void SyncAccountsFromConfig();
	void RefreshAvailablePlatforms();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	enum Platform { Twitch = 0, YouTube = 1, Kick = 2 };

	struct Account {
		QString clientId;
		QString clientSecret;
		QString accessToken;
		QString refreshToken;
		qint64 expiresAt = 0;
		QString userId;
		QString userName;
		QString login;
	};

	struct ChatBadge {
		QString key;
		QString url;
		QString label;
	};

	struct ChatMessage {
		Platform platform = Twitch;
		QString id;
		QString user;
		QString color;
		QString text;
		qint64 ts = 0;
		bool system = false;
		bool showPlatformIcon = true;
		QVector<ChatBadge> badges;
	};

	QSet<int> availablePlatforms;
	Account accounts[3];
	int sendPlatform = Twitch;
	QString ytLiveChatId;
	QString ytVideoId;
	QString ytNextPage;
	QString kickSlug;
	QString kickChatroomId;
	QString twitchLogin;

	QFrame *viewerBar = nullptr;
	QLabel *viewerLabels[3] = {};
	QFrame *viewerChips[3] = {};
	QPushButton *clearSystemButton = nullptr;
	QListWidget *messageList = nullptr;
	QLabel *sendPlatformIcon = nullptr;
	QLineEdit *messageEdit = nullptr;
	QLabel *sendStatus = nullptr;

	QTimer *viewerTimer = nullptr;
	QTimer *youtubeTimer = nullptr;
	QTimer *youtubeDiscoverTimer = nullptr;
	QTimer *kickReconnectTimer = nullptr;
	QTimer *twitchReconnectTimer = nullptr;

	std::shared_ptr<ChatCurlWsSession> twitchWs;
	std::shared_ptr<ChatCurlWsSession> kickWs;
	QByteArray twitchBuffer;
	bool twitchJoined = false;
	bool connectionsActive = false;
	bool kickSubscribed = false;
	bool twitchConnectedAnnounced = false;
	bool kickConnectedAnnounced = false;
	bool youtubeConnectedAnnounced = false;
	bool twitchConnectStarted = false;
	bool youtubeConnectStarted = false;
	bool kickConnectStarted = false;
	bool twitchNotLinkedAnnounced = false;
	bool youtubeNotLinkedAnnounced = false;
	bool kickNotLinkedAnnounced = false;
	int twitchWsGen = 0;
	int kickWsGen = 0;

	QSet<QString> seenMessageIds;
	int maxMessages = 400;
	QHash<QString, QString> twitchBadgeUrls;
	QHash<QString, QPixmap> badgePixmaps;
	QSet<QString> badgeFetchPending;

	void BuildUi();
	void RebuildViewerBar();
	void CycleSendPlatform();
	void UpdateSendStatus();
	void LoadAccount(Platform platform);
	void EnsureToken(Platform platform, std::function<void(bool)> done);
	void Http(const QString &url, const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body,
		  const char *verb, std::function<void(int, const QByteArray &, const QString &)> done);

	void StartConnections();
	void StopConnections();
	void EnsurePlatformConnections();
	void TryStartPlatform(Platform platform);

	void RefreshViewers();
	void FetchTwitchViewers();
	void FetchYouTubeViewers();
	void FetchKickViewers();
	void SetViewerCount(Platform platform, int count, bool live);

	void ConnectTwitchIrc();
	void DisconnectTwitchIrc();
	void HandleTwitchLine(const QString &line);
	void SendTwitchMessage(const QString &text);
	void FetchTwitchBadgeSets();
	void IngestTwitchBadgeSets(const QJsonArray &sets);
	QVector<ChatBadge> TwitchBadgesFromTags(const QString &badgesTag) const;

	void StartYouTubeChat();
	void PollYouTubeChat();
	void SendYouTubeMessage(const QString &text);

	void ConnectKickChat();
	void DisconnectKickChat();
	void ResolveKickChatroom();
	void FetchKickViewerToken(std::function<void(QString token)> done);
	void StartKickWs(const QString &wsUrl, const QString &chatroomId, bool allowFallback = false);
	void SendKickWsText(const QString &text);
	void OnKickWsMessage(const QString &message);
	void OnKickStreamOffline();
	void SendKickMessage(const QString &text);
	static QVector<ChatBadge> KickBadgesFromIdentity(const QJsonObject &identity);

	void AppendMessage(const ChatMessage &msg);
	void AppendSystemMessage(const QString &text, Platform platform, bool withIcon = true);
	void ClearSystemMessages();
	void RelayoutMessageItems();
	int LayoutMessageRow(QWidget *row, QListWidgetItem *item, int viewportWidth) const;
	void FetchBadgePixmap(const QString &url, QLabel *target);
	static int MessageTextHeight(const QString &html, int width, const QFont &font);
	bool DockIsOpen() const;
	void SendChatMessage();
	static const char *PlatformKey(Platform platform);
	static QString PlatformLabel(Platform platform);
	static QIcon PlatformIcon(Platform platform);
	static QColor PlatformAccent(Platform platform);
	static QColor PlatformChipAccent(Platform platform);
};
