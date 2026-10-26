#pragma once

#include <QFrame>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;
class QSslSocket;

/* Unified multi-platform chat + per-platform viewer counts.
 * Uses the same stream_accounts OAuth tokens as Stream Info. */
class ChatDock : public QFrame {
	Q_OBJECT

public:
	explicit ChatDock(QWidget *parent = nullptr);
	~ChatDock() override;

	void ReloadAccounts();
	void RefreshAvailablePlatforms();

protected:
	void showEvent(QShowEvent *event) override;
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

	struct ChatMessage {
		Platform platform = Twitch;
		QString id;
		QString user;
		QString color;
		QString text;
		qint64 ts = 0;
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
	QListWidget *messageList = nullptr;
	QLabel *sendPlatformIcon = nullptr;
	QLineEdit *messageEdit = nullptr;
	QLabel *sendStatus = nullptr;
	QPushButton *settingsButton = nullptr;
	QPushButton *accountsButton = nullptr;

	QTimer *viewerTimer = nullptr;
	QTimer *youtubeTimer = nullptr;
	QTimer *kickReconnectTimer = nullptr;

	QSslSocket *twitchIrc = nullptr;
	QByteArray twitchBuffer;
	bool twitchJoined = false;

	QSslSocket *kickWs = nullptr;
	QByteArray kickBuffer;
	bool kickWsReady = false;
	bool kickSubscribed = false;

	QSet<QString> seenMessageIds;
	int maxMessages = 400;

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

	void RefreshViewers();
	void FetchTwitchViewers();
	void FetchYouTubeViewers();
	void FetchKickViewers();
	void SetViewerCount(Platform platform, int count, bool live);

	void ConnectTwitchIrc();
	void DisconnectTwitchIrc();
	void OnTwitchReady();
	void OnTwitchData();
	void HandleTwitchLine(const QString &line);
	void SendTwitchMessage(const QString &text);

	void StartYouTubeChat();
	void PollYouTubeChat();
	void SendYouTubeMessage(const QString &text);

	void ConnectKickChat();
	void DisconnectKickChat();
	void ResolveKickChatroom();
	void OnKickEncrypted();
	void OnKickData();
	void HandleKickFrame(const QByteArray &payload);
	void SendKickWsText(const QString &text);
	void OnKickWsMessage(const QString &message);
	void SendKickMessage(const QString &text);

	void AppendMessage(const ChatMessage &msg);
	void SendChatMessage();
	static const char *PlatformKey(Platform platform);
	static QString PlatformLabel(Platform platform);
	static QIcon PlatformIcon(Platform platform);
	static QColor PlatformAccent(Platform platform);
};
