#include "MainWindow.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QSurfaceFormat>

namespace {
void setResourceEnvironment(const char* name, const QString& path)
{
    const QString absolutePath = QDir::toNativeSeparators(QDir::cleanPath(path));
    if (QFileInfo::exists(absolutePath))
        qputenv(name, absolutePath.toUtf8());
}

void configureOpenCascadeResources()
{
    const QString appDirectory = QCoreApplication::applicationDirPath();
    const QString resources = QDir(appDirectory).filePath(QStringLiteral("opencascade-resources"));
    if (!QDir(resources).exists())
        return;

    qputenv("CSF_OCCTResourcePath", QDir::toNativeSeparators(resources).toUtf8());
    qputenv("CSF_LANGUAGE", "us");
    qputenv("MMGT_CLEAR", "1");
    setResourceEnvironment("CSF_SHMessage", QDir(resources).filePath(QStringLiteral("SHMessage")));
    setResourceEnvironment("CSF_MDTVTexturesDirectory", QDir(resources).filePath(QStringLiteral("Textures")));
    setResourceEnvironment("CSF_ShadersDirectory", QDir(resources).filePath(QStringLiteral("Shaders")));
    setResourceEnvironment("CSF_XSMessage", QDir(resources).filePath(QStringLiteral("XSMessage")));
    setResourceEnvironment("CSF_TObjMessage", QDir(resources).filePath(QStringLiteral("TObj")));
    setResourceEnvironment("CSF_StandardDefaults", QDir(resources).filePath(QStringLiteral("StdResource")));
    setResourceEnvironment("CSF_PluginDefaults", QDir(resources).filePath(QStringLiteral("StdResource")));
    setResourceEnvironment("CSF_XCAFDefaults", QDir(resources).filePath(QStringLiteral("StdResource")));
    setResourceEnvironment("CSF_TObjDefaults", QDir(resources).filePath(QStringLiteral("StdResource")));
    setResourceEnvironment("CSF_StandardLiteDefaults", QDir(resources).filePath(QStringLiteral("StdResource")));
    setResourceEnvironment("CSF_IGESDefaults", QDir(resources).filePath(QStringLiteral("XSTEPResource")));
    setResourceEnvironment("CSF_STEPDefaults", QDir(resources).filePath(QStringLiteral("XSTEPResource")));
    setResourceEnvironment("CSF_XmlOcafResource", QDir(resources).filePath(QStringLiteral("XmlOcafResource")));
    setResourceEnvironment("CSF_MIGRATION_TYPES", QDir(resources).filePath(QStringLiteral("StdResource/MigrationSheet.txt")));
}
} // namespace

int main(int argc, char* argv[])
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("AoSCAD Studio"));
    QCoreApplication::setOrganizationName(QStringLiteral("AoSCAD"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    configureOpenCascadeResources();

    const QString iconPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/icon.png"));
    if (QFileInfo::exists(iconPath))
        application.setWindowIcon(QIcon(iconPath));

    MainWindow window;
    window.show();
    return application.exec();
}
