#include <QApplication>
#include <QCoreApplication>

#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName("EbonholdLinux");
    QCoreApplication::setApplicationName("EbonholdUpdater");
    QCoreApplication::setApplicationVersion("0.6.0");

    MainWindow window;
    window.show();

    return app.exec();
}
