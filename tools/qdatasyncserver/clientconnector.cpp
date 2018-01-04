#include "clientconnector.h"
#include <QFile>
#include <QSslKey>
#include <QWebSocket>
#include <QWebSocketCorsAuthenticator>
#include "app.h"

ClientConnector::ClientConnector(DatabaseController *database, QObject *parent) :
	QObject(parent),
	database(database),
	server(nullptr),
	secret(),
	clients()
{
	auto name = qApp->configuration()->value(QStringLiteral("server/name"), QCoreApplication::applicationName()).toString();
	auto mode = qApp->configuration()->value(QStringLiteral("server/wss"), false).toBool() ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode;
	secret = qApp->configuration()->value(QStringLiteral("server/secret")).toString();

	server = new QWebSocketServer(name, mode, this);
	connect(server, &QWebSocketServer::newConnection,
			this, &ClientConnector::newConnection);
	connect(server, &QWebSocketServer::serverError,
			this, &ClientConnector::serverError);
	connect(server, &QWebSocketServer::sslErrors,
			this, &ClientConnector::sslErrors);
	if(!secret.isEmpty()) {
		connect(server, &QWebSocketServer::originAuthenticationRequired,
				this, &ClientConnector::verifySecret);
	}
}

bool ClientConnector::setupWss()
{
	if(server->secureMode() != QWebSocketServer::SecureMode)
		return true;

	auto filePath = qApp->configuration()->value(QStringLiteral("server/wss/pfx")).toString();
	filePath = qApp->absolutePath(filePath);

	QSslKey privateKey;
	QSslCertificate localCert;
	QList<QSslCertificate> caCerts;

	QFile file(filePath);
	if(!file.open(QIODevice::ReadOnly))
		return false;

	if(QSslCertificate::importPkcs12(&file,
								  &privateKey,
								  &localCert,
								  &caCerts,
								  qApp->configuration()->value(QStringLiteral("server/wss/pass")).toString().toUtf8())) {
		auto conf = server->sslConfiguration();
		conf.setLocalCertificate(localCert);
		conf.setPrivateKey(privateKey);
		caCerts.append(conf.caCertificates());
		conf.setCaCertificates(caCerts);
		server->setSslConfiguration(conf);
		return true;
	} else
		return false;
}

bool ClientConnector::listen()
{
	auto host = qApp->configuration()->value(QStringLiteral("server/host"), QHostAddress(QHostAddress::Any).toString()).toString();
	auto port = static_cast<quint16>(qApp->configuration()->value(QStringLiteral("server/port"), 4242).toUInt());
	if(server->listen(QHostAddress(host), port)) {
		qInfo() << "Listening on port" << server->serverPort();
		return true;
	} else {
		qCritical() << "Failed to create server with error:"
					<< server->errorString();
		return false;
	}
}

void ClientConnector::notifyChanged(const QUuid &deviceId)
{
	auto client = clients.value(deviceId);
	if(client)
		client->notifyChanged();
}

void ClientConnector::verifySecret(QWebSocketCorsAuthenticator *authenticator)
{
	if(secret.isNull())
		authenticator->setAllowed(true);
	else
		authenticator->setAllowed(authenticator->origin() == secret);
}

void ClientConnector::newConnection()
{
	while (server->hasPendingConnections()) {
		auto socket = server->nextPendingConnection();
		auto client = new Client(database, socket, this);
		//queued is needed because they are emitted from threads
		connect(client, &Client::connected,
				this, &ClientConnector::clientConnected,
				Qt::QueuedConnection);
		connect(client, &Client::proofRequested,
				this, &ClientConnector::proofRequested,
				Qt::QueuedConnection);
	}
}

void ClientConnector::serverError()
{
	qWarning() << "Server error:"
			   << server->errorString();
}

void ClientConnector::sslErrors(const QList<QSslError> &errors)
{
	foreach(auto error, errors) {
		qWarning() << "SSL error:"
				   << error.errorString();
	}
}

void ClientConnector::clientConnected(const QUuid &deviceId)
{
	auto client = qobject_cast<Client*>(sender());
	if(!client)
		return;

	clients.insert(deviceId, client);
	connect(client, &Client::destroyed, this, [this, deviceId](){
		clients.remove(deviceId);
	});
}

void ClientConnector::proofRequested(const QUuid &partner, const QtDataSync::ProofMessage &message)
{
	auto client = qobject_cast<Client*>(sender());
	if(!client)
		return;

	QPointer<Client> pClient = clients.value(partner);
	if(!pClient)
		client->proofResult(false);
	else {
		auto devId = message.deviceId;
		connect(pClient, &Client::proofDone,
				client, [devId, pClient, client](const QUuid &partner, bool result) {
			if(devId == partner) {
				client->proofResult(result);
				if(pClient)
					pClient->disconnect(client);
			}
		}, Qt::QueuedConnection);
		pClient->sendProof(message);
	}
}
