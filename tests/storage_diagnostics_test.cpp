#include <dsfilemanager/filemanager.h>
#include <QCoroTask>
#include <QTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <logfault/logfault.h>
#include <sstream>

using namespace dsfm;
class StorageDiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void rejectedCatalog_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("detail");
        QTest::newRow("schema-version") << "schema" << "schema validation failed; expected=2, stored=1";
        QTest::newRow("unrecognized-schema-is-redacted") << "private-schema" << "schema validation failed; expected=2, stored=-1";
        QTest::newRow("missing-limit") << "missing-limit" << "limit validation failed; field=max_temporary_bytes, missing=1";
        QTest::newRow("invalid-limit-is-redacted") << "private-limit" << "limit validation failed; field=max_temporary_bytes, missing=0, unsigned_integer=0";
        QTest::newRow("sql-prepare") << "table" << "SQL preparation failed; error_type=";
    }
    void rejectedCatalog() {
        QFETCH(QString, fault); QFETCH(QString, detail);
        QTemporaryDir temporary;
        NamespaceConfig config{QUuid::createUuid(), temporary.filePath("storage")};
        {
            NativeFileManager manager;
            QVERIFY(QCoro::waitFor(manager.createNamespace(config)));
            QVERIFY(QCoro::waitFor(manager.shutdown()));
        }
        const auto connection = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(config.storage_root + "/.filemanager/catalog.db");
            QVERIFY(db.open());
            QSqlQuery query(db);
            if (fault == "schema" || fault == "private-schema") {
                QVERIFY(query.prepare("UPDATE meta SET value=? WHERE key='schema'"));
                query.addBindValue(fault == "schema" ? "1" : "PRIVATE-CATALOG-CONTENT"); QVERIFY(query.exec());
            } else if (fault == "table") {
                QVERIFY(query.exec("DROP TABLE operations"));
            } else {
                QVERIFY(query.exec("SELECT value FROM meta WHERE key='limits'")); QVERIFY(query.next());
                auto limits = QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object();
                if (fault == "missing-limit") limits.remove("max_temporary_bytes");
                else limits["max_temporary_bytes"] = "PRIVATE-CATALOG-CONTENT";
                query.finish();
                QVERIFY(query.prepare("UPDATE meta SET value=? WHERE key='limits'"));
                query.addBindValue(QString::fromUtf8(QJsonDocument(limits).toJson(QJsonDocument::Compact))); QVERIFY(query.exec());
            }
            query.finish(); db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        std::ostringstream trace;
        logfault::LogManager::Instance().AddHandler(std::make_unique<logfault::StreamHandler>(trace, logfault::LogLevel::TRACE));
        auto clear = qScopeGuard([] { logfault::LogManager::Instance().ClearHandlers(); });
        NativeFileManager reopened;
        const auto result = QCoro::waitFor(reopened.openNamespace(config));
        QVERIFY(!result); QCOMPARE(result.error().code, ErrorCode::Corruption);
        QVERIFY(QCoro::waitFor(reopened.shutdown()));
        const auto logs = QString::fromStdString(trace.str());
        QVERIFY2(logs.contains(detail), qPrintable(logs));
        QVERIFY(logs.contains("FileManager failure; source="));
        QVERIFY(!logs.contains("PRIVATE-CATALOG-CONTENT"));
        if (fault == "table") {
            QVERIFY(result.error().native_code != 0);
            QVERIFY(logs.contains("source=recovery.cpp:"));
        } else QVERIFY(logs.contains("source=catalog.cpp:"));
    }
    void currentCatalogStillOpens() {
        QTemporaryDir temporary;
        NamespaceConfig config{QUuid::createUuid(), temporary.filePath("storage")};
        { NativeFileManager manager; QVERIFY(QCoro::waitFor(manager.createNamespace(config))); QVERIFY(QCoro::waitFor(manager.shutdown())); }
        NativeFileManager reopened;
        QVERIFY(QCoro::waitFor(reopened.openNamespace(config)));
        QVERIFY(QCoro::waitFor(reopened.shutdown()));
    }
};
QTEST_GUILESS_MAIN(StorageDiagnosticsTest)
#include "storage_diagnostics_test.moc"
