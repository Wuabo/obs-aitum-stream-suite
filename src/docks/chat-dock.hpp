#pragma once

#include <QFrame>
#include <QHash>
#include <QList>
#include <QPointer>
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

	struct ChatEmoteSpan {
		QString id;
		int start = 0;
		int end = 0; /* inclusive char index in message text */
	};

	struct ChatMessage {
		Platform platform = Twitch;
		QString id;
		QString user;
		QString userLogin; /* Twitch login / Kick slug for delete-by-user matching */
		QString color;
		QString text;
		qint64 ts = 0;
		bool system = false;
		bool showPlatformIcon = true;
		QVector<ChatBadge> badges;
		QVector<ChatEmoteSpan> emotes;
	};

	QSet<int> availablePlatforms;
	Account accounts[3];
	int sendPlatform = Twitch;
	QString ytLiveChatId;
	QString ytVideoId;
	QString ytNextPage;
	QString kickSlug;
	QString kickChatroomId;
	QString kickChannelId;
	QString kickRealtimeClientId;
	struct KickSubBadgeTier {
		int months = 0;
		QString url;
	};
	QVector<KickSubBadgeTier> kickSubBadges;
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
	QHash<QString, QString> twitchBadgeTitles;
	QHash<QString, QPixmap> badgePixmaps;
	QHash<QString, QByteArray> badgeRawBytes;
	QHash<QString, QList<QPointer<QLabel>>> badgeWaiters;
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
	static QVector<ChatEmoteSpan> TwitchEmotesFromTag(const QString &emotesTag);
	static QString TwitchEmoteImageUrl(const QString &emoteId);
	QString FormatTwitchContentHtml(const QString &raw, const QVector<ChatEmoteSpan> &emotes) const;
	void PrefetchTwitchEmotes(const QVector<ChatEmoteSpan> &emotes, QLabel *label);
	void RefreshTwitchMessageLabel(QLabel *label);
	QWidget *BuildTwitchEmoteBody(const ChatMessage &msg, const QString &prefixHtml, QWidget *parent);

	void StartYouTubeChat();
	void PollYouTubeChat();
	void SendYouTubeMessage(const QString &text);

	void ConnectKickChat();
	void DisconnectKickChat();
	void ResolveKickChatroom();
	void StartKickCentrifugo();
	void StartKickCentrifugoWs(const QString &wsUrl, const QString &token, const QStringList &channels);
	void HandleKickCentrifugoFrame(const QByteArray &text);
	void HandleKickChatPayload(const QJsonObject &data, const QString &eventName = QString());
	void OnKickWsMessage(const QString &message);
	void OnKickStreamOffline();
	void SendKickMessage(const QString &text);
	QVector<ChatBadge> KickBadgesFromIdentity(const QJsonObject &identity) const;
	static QString KickRoleBadgeKey(const QString &type, int count = 0);
	static QPixmap KickRoleBadgePixmap(const QString &role, int size = 14);
	static QPixmap YouTubeRoleBadgePixmap(const QString &role, int size = 14);
	QString KickSubscriberBadgeUrl(int months) const;
	void IngestKickSubscriberBadges(const QJsonArray &badges);
	static QString KickEmoteImageUrl(const QString &emoteId);
	static QVector<ChatEmoteSpan> KickEmotesFromText(const QString &raw);
	QString FormatKickContentHtml(const QString &raw) const;
	void PrefetchKickEmotes(const QString &raw, QLabel *label);
	void RefreshKickMessageLabel(QLabel *label);
	QWidget *BuildKickEmoteBody(const ChatMessage &msg, const QString &prefixHtml, QWidget *parent);
	void ApplyBadgeToLabel(const ChatBadge &badge, QLabel *target);

	void AppendMessage(const ChatMessage &msg);
	void AppendSystemMessage(const QString &text, Platform platform, bool withIcon = true);
	void ClearSystemMessages();
	void MarkMessageDeleted(const QString &messageId, const QString &tooltip = QString());
	void MarkMessagesDeletedByUser(Platform platform, const QString &userLogin, const QString &tooltip = QString());
	void MarkPlatformMessagesDeleted(Platform platform, const QString &tooltip = QString());
	void ApplyDeletedStyleToRow(QWidget *row, QListWidgetItem *item, const QString &tooltip);
	static QPixmap DeletedMessageIconPixmap(int size = 14);
	void RelayoutMessageItems();
	int LayoutMessageRow(QWidget *row, QListWidgetItem *item, int viewportWidth) const;
	void FetchBadgePixmap(const QString &url, QLabel *target);
	void ApplyImageDataToLabel(QLabel *label, const QByteArray &body, int size);
	void FetchImageToLabel(const QString &url, QLabel *target, int size, const QString &cacheKey = QString());
	static QString PixmapDataUri(const QPixmap &pix);
	static int MessageTextHeight(const QString &html, int width, const QFont &font);
	bool DockIsOpen() const;
	void SendChatMessage();
	static const char *PlatformKey(Platform platform);
	static QString PlatformLabel(Platform platform);
	static QIcon PlatformIcon(Platform platform);
	static QColor PlatformAccent(Platform platform);
	static QColor PlatformChipAccent(Platform platform);
};
