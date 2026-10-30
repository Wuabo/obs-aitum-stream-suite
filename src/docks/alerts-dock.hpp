#pragma once

#include <QFrame>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class QListWidget;

/* Unified follow / sub / raid / bits / Super Chat list. Twitch arrives on EventSub.
 * Kick and YouTube are pushed by the chat dock from the sockets it already holds. */
class AlertsDock : public QFrame {
	Q_OBJECT

public:
	explicit AlertsDock(QWidget *parent = nullptr);
	~AlertsDock() override;

	void ReloadAccounts();
	void Push(int platform, const QString &kind, const QString &user, const QString &detail);

private:
	struct WsSession {
		std::atomic<bool> stop{false};
		std::mutex mu;
	};

	QListWidget *list = nullptr;
	QString clientId;
	QString accessToken;
	QString userId;
	int wsGen = 0;
	bool askedReconnect = false;
	std::shared_ptr<WsSession> session;

	void LoadTwitchAccount();
	void StartEventSub(const QString &url, bool subscribeOnWelcome);
	void StopEventSub();
	void SubscribeEventSub(const QString &sessionId);
	void HandleEventSubMessage(const QByteArray &payload, int gen, bool subscribeOnWelcome);
};

extern AlertsDock *alerts_dock;
