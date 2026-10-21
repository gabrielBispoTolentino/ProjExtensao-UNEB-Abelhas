// Mudança da pasta de dados do nome antigo (Abelhas Petterson) para o novo
// (BeeDuino), em pastas temporárias: nunca toca na pasta de dados real.

#include "legacydata.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

namespace {

void makeDatabase(const QString &dir, const QByteArray &marker)
{
    QDir().mkpath(dir);
    QFile file(dir + "/sensor_readings.db");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(marker);
}

QByteArray readDatabase(const QString &dir)
{
    QFile file(dir + "/sensor_readings.db");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestLegacyData : public QObject
{
    Q_OBJECT

    QTemporaryDir base;
    QString oldDir(const QString &c) { return base.filePath(c + "/AbelhasPetterson/SensorMonitor"); }
    QString newDir(const QString &c) { return base.filePath(c + "/BeeDuino/SensorMonitor"); }

private slots:
    void movesOldFolder()
    {
        makeDatabase(oldDir("a"), "old");
        QFile(oldDir("a") + "/extra.txt").open(QIODevice::WriteOnly);
        QString log;
        QCOMPARE(moveLegacyDataDir(oldDir("a"), newDir("a"), &log), newDir("a"));
        QCOMPARE(readDatabase(newDir("a")), QByteArray("old"));
        QVERIFY(QFile::exists(newDir("a") + "/extra.txt"));
        QVERIFY(!QDir(oldDir("a")).exists());
        QVERIFY(!QDir(base.filePath("a/AbelhasPetterson")).exists());
        QVERIFY(log.startsWith("Moved"));
    }

    void newFolderWithDataIsKept()
    {
        makeDatabase(oldDir("b"), "old");
        makeDatabase(newDir("b"), "new");
        QString log;
        QCOMPARE(moveLegacyDataDir(oldDir("b"), newDir("b"), &log), newDir("b"));
        QCOMPARE(readDatabase(newDir("b")), QByteArray("new"));
        QCOMPARE(readDatabase(oldDir("b")), QByteArray("old"));
        QVERIFY(log.isEmpty());
    }

    void emptyNewFolderDoesNotBlock()
    {
        makeDatabase(oldDir("c"), "old");
        QDir().mkpath(newDir("c"));
        QString log;
        QCOMPARE(moveLegacyDataDir(oldDir("c"), newDir("c"), &log), newDir("c"));
        QCOMPARE(readDatabase(newDir("c")), QByteArray("old"));
    }

    void openDatabaseKeepsOldFolderUntilNextTime()
    {
        // A versão antiga ainda aberta com o banco: não dá para mover agora.
        QDir().mkpath(oldDir("d"));
        QString log;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("held"));
            db.setDatabaseName(oldDir("d") + "/sensor_readings.db");
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec(QStringLiteral("CREATE TABLE t(x)")));
            QCOMPARE(moveLegacyDataDir(oldDir("d"), newDir("d"), &log), oldDir("d"));
            QVERIFY(!QFile::exists(newDir("d") + "/sensor_readings.db"));
            QVERIFY(log.startsWith("Could not move"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("held"));
        QCOMPARE(moveLegacyDataDir(oldDir("d"), newDir("d"), &log), newDir("d"));
    }

    void sameFolderOnAndroid()
    {
        makeDatabase(base.filePath("e/files"), "same");
        QString log;
        QCOMPARE(moveLegacyDataDir(base.filePath("e/files"), base.filePath("e/files"), &log), base.filePath("e/files"));
        QVERIFY(log.isEmpty());
    }

    void freshInstall()
    {
        QString log;
        QCOMPARE(moveLegacyDataDir(oldDir("f"), newDir("f"), &log), newDir("f"));
        QVERIFY(log.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestLegacyData)
#include "tst_legacydata.moc"
