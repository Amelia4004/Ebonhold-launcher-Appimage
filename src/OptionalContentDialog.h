#pragma once

#include "AddonManager.h"
#include "OptionalContentManager.h"

#include <QDialog>
#include <QJsonObject>
#include <QVector>

class QLabel;
class QPushButton;
class QProgressBar;
class QTableWidget;

class OptionalContentDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit OptionalContentDialog(const QString &gameDirectory,
                                   const QString &authToken,
                                   const QJsonObject &manifest,
                                   QWidget *parent = nullptr);

signals:
    void authenticationExpired();

private:
    void buildUi();
    void refreshAll();
    void rebuildAddonTable(const QVector<AddonInfo> &addons);
    void rebuildHdTable(const QVector<HdPatchInfo> &patches);
    void updateControls();
    void openAddonFolder();
    void showOperationError(const QString &message);

    static QString addonStatusText(AddonStatus status);
    static QString hdStatusText(HdPatchStatus status);
    static QString formatBytes(qint64 bytes);

    QString m_gameDirectory;
    AddonManager *m_addons = nullptr;
    OptionalContentManager *m_hdPatches = nullptr;

    QLabel *m_operationText = nullptr;
    QProgressBar *m_progress = nullptr;
    QTableWidget *m_addonTable = nullptr;
    QTableWidget *m_hdTable = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_updateAllButton = nullptr;
    QPushButton *m_openAddonFolderButton = nullptr;
    QPushButton *m_closeButton = nullptr;

    QVector<AddonInfo> m_currentAddons;
    QVector<HdPatchInfo> m_currentPatches;
};
