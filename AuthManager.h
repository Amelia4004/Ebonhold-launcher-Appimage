#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QString>

class AuthManager final : public QObject
{
    Q_OBJECT

public:
    explicit AuthManager(QObject *parent = nullptr);

    bool hasToken() const;
    QString token() const;
    void clearToken();

    void fetchGamesManifest();
    void login(const QString &username, const QString &password);

signals:
    void loginRequired();
    void loginSucceeded();
    void loginFailed(const QString &message);
    void manifestReady(const QJsonObject &manifest);
    void requestFailed(const QString &message);

private:
    void loadToken();
    bool saveToken(const QString &token);
    QString tokenFilePath() const;
    QNetworkRequest authenticatedRequest(const QUrl &url) const;
    static QString messageFromJson(const QByteArray &data);

    QNetworkAccessManager m_network;
    QString m_token;
};
