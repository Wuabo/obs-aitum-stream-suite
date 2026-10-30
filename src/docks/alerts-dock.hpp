#pragma once

#include <QFrame>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class QListWidget;
class QTimer;

/* Activity Feed: follow / sub / raid / bits / Super Chat / redeems.
 * Twitch arrives on EventSub. Kick and YouTube are pushed by the chat dock. */
class AlertsDock : public QFrame {
	Q_OBJECT

public:
	explicit AlertsDock(QWidget *parent = nullptr);
	~AlertsDock() override;

	void ReloadAccounts();
	void ReloadHistory();
	void Push(int platform, const QString &kind, const QString &user, const QString &detail,
		  const QString &message = QString(), qint64 atMs = 0);

private:
	struct WsSession {
		std::atomic<bool> stop{false};
		std::mutex mu;
	};

	struct Entry {
		int platform = 0;
		QString kind;
		QString user;
		QString detail;
		QString message;
		qint64 atMs = 0;
	};

	QListWidget *list = nullptr;
	QTimer *ageTimer = nullptr;
	QTimer *saveTimer = nullptr;
	QString clientId;
	QString accessToken;
	QString userId;
	int wsGen = 0;
	bool askedReconnect = false;
	bool loadingHistory = false;
	std::shared_ptr<WsSession> session;
	std::vector<Entry> entries;

	void LoadTwitchAccount();
	void StartEventSub(const QString &url, bool subscribeOnWelcome);
	void StopEventSub();
	void SubscribeEventSub(const QString &sessionId);
	void HandleEventSubMessage(const QByteArray &payload, int gen, bool subscribeOnWelcome);
	void InsertCard(const Entry &entry, bool atTop);
	void RefreshAges();
	void ScheduleSave();
	void SaveHistory();
	void LoadHistory();
};

extern AlertsDock *alerts_dock;
