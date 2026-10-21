// Pasta de dados da versão com o nome antigo (ver legacydata.h).

#include "legacydata.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

constexpr auto legacyOrganization = "AbelhasPetterson";
constexpr auto databaseFile = "sensor_readings.db";

bool hasDatabase(const QString &dir)
{
    return QFileInfo::exists(QDir(dir).filePath(QString::fromLatin1(databaseFile)));
}

} // namespace

QString moveLegacyDataDir(const QString &oldDir, const QString &newDir, QString *log)
{
    log->clear();
    if (QDir::cleanPath(oldDir) == QDir::cleanPath(newDir) || !hasDatabase(oldDir) || hasDatabase(newDir)) {
        return newDir;
    }

    // A pasta inteira de uma vez: o banco e o diário do SQLite nunca ficam
    // separados. No Windows o rename falha enquanto outro processo (a versão
    // antiga aberta) usa o banco; aí fica tudo onde está, para a próxima vez.
    QDir().rmdir(newDir);   // uma pasta nova vazia impediria o rename (rmdir só apaga vazias)
    QDir().mkpath(QFileInfo(newDir).absolutePath());
    if (!QDir().rename(oldDir, newDir)) {
        *log = QStringLiteral("Could not move the data folder %1 to %2; using the old one this time")
                   .arg(QDir::toNativeSeparators(oldDir), QDir::toNativeSeparators(newDir));
        return oldDir;
    }

    // A pasta da organização antiga (...\AbelhasPetterson) some se ficou vazia.
    QDir().rmdir(QFileInfo(oldDir).absolutePath());
    *log = QStringLiteral("Moved the data folder %1 to %2")
               .arg(QDir::toNativeSeparators(oldDir), QDir::toNativeSeparators(newDir));
    return newDir;
}

QString resolveDataDir()
{
    const QString organization = QCoreApplication::organizationName();
    const QString newDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // O caminho antigo é o mesmo cálculo com o nome antigo da organização.
    QCoreApplication::setOrganizationName(QString::fromLatin1(legacyOrganization));
    const QString oldDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setOrganizationName(organization);

    QString log;
    const QString dir = moveLegacyDataDir(oldDir, newDir, &log);
    if (!log.isEmpty()) {
        qInfo().noquote() << log;
    }
    QDir().mkpath(dir);
    return dir;
}
