#pragma once

#include "UpdateManager.h"

#include <QJsonObject>
#include <QMainWindow>
#include <QVector>

class AuthManager;
class QLineEdit;
class QLabel;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void startCheck(bool fullRepair = false);
    void startUpdate();
    void resetUpdateState();
    void showLoginDialog();
    void showLauncherScriptsDialog();
    void showOptionalContentDialog();
    void requestOptionalContent();
    void setBusy(bool busy);
    void appendLog(const QString &text);

    AuthManager *m_auth = nullptr;
    UpdateManager *m_updater = nullptr;

    QLineEdit *m_gamePath = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_checkButton = nullptr;
    QPushButton *m_fullRepairButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_launcherScriptsButton = nullptr;
    QPushButton *m_addonFolderButton = nullptr;
    QPushButton *m_optionalContentButton = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QJsonObject m_manifest;
    QVector<PatchFile> m_pendingUpdates;
    QString m_realmlist;
    QString m_scannedGameDirectory;
    bool m_loginDialogOpen = false;
    bool m_fullRepairRequested = false;
    bool m_busy = false;
    bool m_optionalContentRequested = false;
};
