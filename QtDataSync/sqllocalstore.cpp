#include "sqllocalstore.h"

#include <QJsonArray>

#include <QtSql>

using namespace QtDataSync;

#define TABLE_DIR(tName) \
	auto tableDir = storageDir; \
	if(!tableDir.mkpath(tName) || !tableDir.cd(tName)) { \
		emit requestFailed(id, QStringLiteral("Failed to create table directory %1").arg(tName)); \
		return; \
	}

#define EXEC_QUERY(query) do {\
	if(!query.exec()) { \
		emit requestFailed(id, query.lastError().text()); \
		return; \
	} \
} while(false)

const QString SqlLocalStore::DatabaseName(QStringLiteral("__QtDataSync_default_database"));

SqlLocalStore::SqlLocalStore(QObject *parent) :
	LocalStore(parent),
	storageDir(),
	database()
{}

void SqlLocalStore::initialize()
{
	storageDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
	storageDir.mkpath(QStringLiteral("./qtdatasync_localstore"));
	storageDir.cd(QStringLiteral("./qtdatasync_localstore"));

	database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), DatabaseName);
	database.setDatabaseName(storageDir.absoluteFilePath(QStringLiteral("./store.db")));
	database.open();
}

void SqlLocalStore::finalize()
{
	database.close();
	database = QSqlDatabase();
	QSqlDatabase::removeDatabase(DatabaseName);
}

void SqlLocalStore::count(quint64 id, int metaTypeId)
{
	auto tName = tableName(metaTypeId);

	if(testTableExists(tName)) {
		QSqlQuery countQuery(database);
		countQuery.prepare(QStringLiteral("SELECT Count(*) FROM %1").arg(tName));
		EXEC_QUERY(countQuery);

		if(countQuery.first()) {
			emit requestCompleted(id, countQuery.value(0).toInt());
			return;
		}
	}

	emit requestCompleted(id, 0);
}

void SqlLocalStore::loadAll(quint64 id, int metaTypeId)
{
	auto tName = tableName(metaTypeId);
	TABLE_DIR(tName)

	if(testTableExists(tName)) {
		QSqlQuery loadQuery(database);
		loadQuery.prepare(QStringLiteral("SELECT File FROM %1").arg(tName));
		EXEC_QUERY(loadQuery);

		QJsonArray array;
		while(loadQuery.next()) {
			QFile file(tableDir.absoluteFilePath(loadQuery.value(0).toString() + QStringLiteral(".dat")));
			file.open(QIODevice::ReadOnly);
			auto doc = QJsonDocument::fromBinaryData(file.readAll());
			file.close();

			if(doc.isNull() || file.error() != QFile::NoError) {
				emit requestFailed(id, QStringLiteral("Failed to read data from file \"%1\" with error: %2")
								   .arg(file.fileName())
								   .arg(file.errorString()));
				return;
			} else
				array.append(doc.object());
		}

		emit requestCompleted(id, array);
	} else
		emit requestCompleted(id, QJsonArray());
}

void SqlLocalStore::load(quint64 id, int metaTypeId, const QString &, const QString &value)
{
	auto tName = tableName(metaTypeId);
	TABLE_DIR(tName)

	if(testTableExists(tName)) {
		QSqlQuery loadQuery(database);
		loadQuery.prepare(QStringLiteral("SELECT File FROM %1 WHERE Key = ?").arg(tName));
		loadQuery.addBindValue(value);
		EXEC_QUERY(loadQuery);

		if(loadQuery.first()) {
			QFile file(tableDir.absoluteFilePath(loadQuery.value(0).toString() + QStringLiteral(".dat")));
			file.open(QIODevice::ReadOnly);
			auto doc = QJsonDocument::fromBinaryData(file.readAll());
			file.close();

			if(doc.isNull() || file.error() != QFile::NoError) {
				emit requestFailed(id, QStringLiteral("Failed to read data from file \"%1\" with error: %2")
								   .arg(file.fileName())
								   .arg(file.errorString()));
			} else
				emit requestCompleted(id, doc.object());
			return;
		}
	}

	emit requestCompleted(id, QJsonValue::Null);
}

void SqlLocalStore::save(quint64 id, int metaTypeId, const QString &key, const QJsonObject &object)
{
	auto tName = tableName(metaTypeId);
	auto tKey = object[key].toVariant().toString();
	TABLE_DIR(tName)

	//create table
	QSqlQuery createQuery(database);
	createQuery.prepare(QStringLiteral("CREATE TABLE IF NOT EXISTS %1 ("
							"Key	TEXT NOT NULL,"
							"File	TEXT NOT NULL,"
							"PRIMARY KEY(Key)"
						");").arg(tName));
	EXEC_QUERY(createQuery);

	//check if the file exists
	QSqlQuery existQuery(database);
	existQuery.prepare(QStringLiteral("SELECT File FROM %1 WHERE Key = ?").arg(tName));
	existQuery.addBindValue(tKey);
	EXEC_QUERY(existQuery);

	auto needUpdate = false;
	QScopedPointer<QFile> file;
	if(existQuery.first()) {
		file.reset(new QFile(tableDir.absoluteFilePath(existQuery.value(0).toString() + QStringLiteral(".dat"))));
		file->open(QIODevice::WriteOnly);
	} else {
		auto fileName = QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
		fileName = tableDir.absoluteFilePath(QStringLiteral("%1XXXXXX.dat")).arg(fileName);
		auto tFile = new QTemporaryFile(fileName);
		tFile->setAutoRemove(false);
		tFile->open();
		file.reset(tFile);
		needUpdate = true;
	}

	//save the file
	QJsonDocument doc(object);
	file->write(doc.toBinaryData());
	file->close();
	if(file->error() != QFile::NoError) {
		emit requestFailed(id, QStringLiteral("Failed to write data to file \"%1\" with error: %2")
						   .arg(file->fileName())
						   .arg(file->errorString()));
		return;
	}

	//save key in database
	if(needUpdate) {
		QSqlQuery insertQuery(database);
		insertQuery.prepare(QStringLiteral("INSERT INTO %1 (Key, File) VALUES(?, ?)").arg(tName));
		insertQuery.addBindValue(tKey);
		insertQuery.addBindValue(tableDir.relativeFilePath(QFileInfo(file->fileName()).completeBaseName()));
		EXEC_QUERY(insertQuery);
	}

	emit requestCompleted(id, QJsonValue::Undefined);
}

void SqlLocalStore::remove(quint64 id, int metaTypeId, const QString &, const QString &value)
{
	auto tName = tableName(metaTypeId);
	TABLE_DIR(tName)

	if(testTableExists(tName)) {
		QSqlQuery loadQuery(database);
		loadQuery.prepare(QStringLiteral("SELECT File FROM %1 WHERE Key = ?").arg(tName));
		loadQuery.addBindValue(value);
		EXEC_QUERY(loadQuery);

		if(loadQuery.first()) {
			auto fileName = tableDir.absoluteFilePath(loadQuery.value(0).toString() + QStringLiteral(".dat"));
			if(!QFile::remove(fileName)) {
				emit requestFailed(id, QStringLiteral("Failed to delete file %1").arg(fileName));
				return;
			}

			QSqlQuery removeQuery(database);
			removeQuery.prepare(QStringLiteral("DELETE FROM %1 WHERE Key = ?").arg(tName));
			removeQuery.addBindValue(value);
			EXEC_QUERY(removeQuery);
		}
	}

	emit requestCompleted(id, QJsonValue::Undefined);
}

void SqlLocalStore::removeAll(quint64 id, int metaTypeId)
{
	auto tName = tableName(metaTypeId);
	TABLE_DIR(tName)

	if(!tableDir.removeRecursively()) {
		emit requestFailed(id, QStringLiteral("Failed to delete table directory %1").arg(tName));
		return;
	}

	QSqlQuery removeQuery(database);
	removeQuery.prepare(QStringLiteral("DROP TABLE IF EXISTS %1").arg(tName));
	EXEC_QUERY(removeQuery);
	emit requestCompleted(id, QJsonValue::Undefined);
}

QString SqlLocalStore::tableName(int metaTypeId) const
{
	return QString::fromLatin1('_' + QByteArray(QMetaType::typeName(metaTypeId)).toHex());
}

bool SqlLocalStore::testTableExists(const QString &tableName) const
{
	return database.tables().contains(tableName);
}
