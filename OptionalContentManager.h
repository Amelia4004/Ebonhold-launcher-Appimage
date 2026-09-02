#pragma once

#include "UpdateManager.h"

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

enum class HdPatchKind
{
    CharacterWorld,
    Creatures
};

enum class HdPatchStatus
{
    Unavailable,
    NotInstalled,
    Installed,
    UpdateAvailable,
    Error
};

struct HdPatchInfo
{
    HdPatchKind kind = HdPatchKind::CharacterWorld;
    QString title;
    QString description;
    QString relativePath;
    PatchFile file;
    HdPatchStatus status = HdPatchStatus::Unavailable;
    QString error;
};

Q_DECLARE_METATYPE(HdPatchInfo)

class OptionalContentManager final : public QObject
{
    Q_OBJECT

public:
    explicit OptionalContentManager(QObject *parent = nullptr);

    void setAuthToken(const QString &token);
    void setGameDirectory(const QString &gameDirectory);
    void setManifest(const QJsonObject &manifest);

    void refresh();
    void install(HdPatchKind kind);
    void remove(HdPatchKind kind);

    bool isBusy() const;

signals:
    void patchesReady(const QVector<HdPatchInfo> &patches);
    void busyChanged(bool busy);
    void operationProgress(int percent, const QString &text);
    void operationFinished(const QString &message);
    void operationFailed(const QString &message);
    void authenticationExpired();

private:
    static QString pathForKind(HdPatchKind kind);
    static QString titleForKind(HdPatchKind kind);
    static QString descriptionForKind(HdPatchKind kind);
    static HdPatchInfo scanOne(const QString &gameDirectory,
                               const QJsonObject &manifest,
                               HdPatchKind kind);
    std::optional<PatchFile> fileForKind(HdPatchKind kind) const;
    QString destinationForKind(HdPatchKind kind) const;
    void setBusy(bool busy);

    UpdateManager *m_updater = nullptr;
    QString m_authToken;
    QString m_gameDirectory;
    QJsonObject m_manifest;
    bool m_busy = false;
    HdPatchKind m_activeKind = HdPatchKind::CharacterWorld;
};
