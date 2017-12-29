#include "client.h"
#include "app.h"
#include "errormessage_p.h"
#include "identifymessage_p.h"
#include "accountmessage_p.h"
#include "welcomemessage_p.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QUuid>

#include <QtConcurrent/QtConcurrentRun>

#if QT_HAS_INCLUDE(<chrono>)
#define scdtime(x) x
#else
#define scdtime(x) std::chrono::duration_cast<std::chrono::milliseconds>(x).count()
#endif

using namespace QtDataSync;

#undef qDebug
#define qDebug(...) qCDebug(logFn, __VA_ARGS__)
#undef qInfo
#define qInfo(...) qCInfo(logFn, __VA_ARGS__)
#undef qWarning
#define qWarning(...) qCWarning(logFn, __VA_ARGS__)
#undef qCritical
#define qCritical(...) qCCritical(logFn, __VA_ARGS__)

// ------------- Exceptions Definitions -------------

class MessageException : public QException
{
public:
	MessageException(const QByteArray &message);

	const char *what() const noexcept override;
	void raise() const override;
	QException *clone() const override;

private:
	const QByteArray _msg;
};

class ClientErrorException : public QException, public ErrorMessage
{
public:
	ClientErrorException(ErrorType type = UnknownError,
						 const QString &message = {},
						 bool canRecover = false);

	const char *what() const noexcept override;
	void raise() const override;
	QException *clone() const override;

private:
	mutable QByteArray _msg;
};

template <typename TMessage>
class UnexpectedException : public ClientErrorException
{
	static_assert(std::is_void<typename TMessage::QtGadgetHelper>::value, "Only Q_GADGETS can be serialized");
public:
	UnexpectedException();
};

// ------------- Client Implementation -------------

QThreadStorage<Client::Rng> Client::rngPool;

Client::Client(DatabaseController *database, QWebSocket *websocket, QObject *parent) :
	QObject(parent),
	_catStr(),
	_logCat(new QLoggingCategory("client.unknown")),
	_database(database),
	_socket(websocket),
	_idleTimer(nullptr),
	_downLimit(20),
	_downThreshold(10),
	_queue(new SingleTaskQueue(qApp->threadPool(), this)),
	_state(Authenticating),
	_deviceId(),
	_loginNonce(),
	_cachedChanges(0),
	_activeDownloads()
{
	_socket->setParent(this);

	connect(_socket, &QWebSocket::disconnected,
			this, &Client::closeClient);
	connect(_socket, &QWebSocket::binaryMessageReceived,
			this, &Client::binaryMessageReceived);
	connect(_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
			this, &Client::error);
	connect(_socket, &QWebSocket::sslErrors,
			this, &Client::sslErrors);

	_downLimit = qApp->configuration()->value(QStringLiteral("server/downloads/limit"), _downLimit).toUInt();
	_downThreshold = qApp->configuration()->value(QStringLiteral("server/downloads/threshold"), _downThreshold).toUInt();
	auto idleTimeout = qApp->configuration()->value(QStringLiteral("server/idleTimeout"), 5).toInt();
	if(idleTimeout > 0) {
		_idleTimer = new QTimer(this);
		_idleTimer->setInterval(scdtime(std::chrono::minutes(idleTimeout)));
		_idleTimer->setTimerType(Qt::VeryCoarseTimer);
		_idleTimer->setSingleShot(true);
		connect(_idleTimer, &QTimer::timeout,
				this, &Client::timeout);
		_idleTimer->start();
	}

	run([this]() {
		//initialize connection by sending indent message
		auto msg = IdentifyMessage::createRandom(rngPool.localData());
		_loginNonce = msg.nonce;
		sendMessage(serializeMessage(msg));
	});
}

void Client::notifyChanged()
{
	run([this]() {
		if(_state == Idle) //silently ignore other states
			triggerDownload();
	});
}

void Client::binaryMessageReceived(const QByteArray &message)
{
	if(message == PingMessage) {
		if(_idleTimer)
			_idleTimer->start();
		_socket->sendBinaryMessage(PingMessage);
		return;
	}

	run([message, this]() {
		try {
			QDataStream stream(message);
			setupStream(stream);
			stream.startTransaction();
			QByteArray name;
			stream >> name;
			if(!stream.commitTransaction())
				throw DataStreamException(stream);

			if(isType<RegisterMessage>(name))
				onRegister(deserializeMessage<RegisterMessage>(stream), stream);
			else if(isType<LoginMessage>(name))
				onLogin(deserializeMessage<LoginMessage>(stream), stream);
			else if(isType<SyncMessage>(name))
				onSync(deserializeMessage<SyncMessage>(stream));
			else if(isType<ChangeMessage>(name))
				onChange(deserializeMessage<ChangeMessage>(stream));
			else if(isType<ChangedAckMessage>(name))
				onChangedAck(deserializeMessage<ChangedAckMessage>(stream));
			else {
				qWarning() << "Unknown message received:" << typeName(name);
				sendMessage(serializeMessage<ErrorMessage>({
															   ErrorMessage::IncompatibleVersionError,
															   QStringLiteral("Unknown message type \"%1\"").arg(QString::fromUtf8(typeName(name)))
														   }));
				closeLater();
			}
		} catch(IncompatibleVersionException &e) {
			qWarning() << "Message error:" << e.what();
			sendMessage(serializeMessage<ErrorMessage>({
														   ErrorMessage::IncompatibleVersionError,
														   QStringLiteral("Version %1 is not compatible to server version %2")
														   .arg(e.invalidVersion().toString())
														   .arg(IdentifyMessage::CurrentVersion.toString())
													   }));
			closeLater();
		}
	});
}

void Client::error()
{
	qWarning() << "Socket error:"
			   << _socket->errorString();
	if(_socket->state() == QAbstractSocket::ConnectedState)
		_socket->close();
}

void Client::sslErrors(const QList<QSslError> &errors)
{
	foreach(auto error, errors) {
		qWarning() << "SSL error:"
				   << error.errorString();
	}
	if(_socket->state() == QAbstractSocket::ConnectedState)
		_socket->close();
}

void Client::closeClient()
{
	if(_queue->clear()) {//save close -> delete only if no parallel stuff anymore (check every 5s)
		qDebug() << "Client disconnected";
		deleteLater();
	} else {
		auto destroyTimer = new QTimer(this);
		destroyTimer->setTimerType(Qt::VeryCoarseTimer);
		connect(destroyTimer, &QTimer::timeout, this, [this](){
			if(_queue->isFinished()) {
				qDebug() << "Client disconnected";
				deleteLater();
			}
		});
		destroyTimer->start(scdtime(std::chrono::seconds(5)));
	}
}

void Client::timeout()
{
	qDebug() << "Disconnecting idle client";
	_socket->close();
}

void Client::run(const std::function<void ()> &fn)
{
	_queue->enqueue([fn, this]() {
		//TODO "log" (hashed) ip on error? to allow blocking???
		try {
			fn();
		} catch (DatabaseException &e) {
			qCritical() << "Internal database error:" << e.what();
			sendMessage(serializeMessage<ErrorMessage>({ErrorMessage::ServerError, QString(), true}));
			closeLater();
		} catch (ClientErrorException &e) {
			qWarning() << "Message error:" << e.what();
			sendMessage(serializeMessage<ErrorMessage>(e));
			closeLater();
		} catch (std::exception &e) {
			qWarning() << "Message error:" << e.what();
			sendMessage(serializeMessage<ErrorMessage>({ErrorMessage::ClientError}));
			closeLater();
		}
	});
}

const QLoggingCategory &Client::logFn() const
{
	return (*(_logCat.data()));
}

void Client::close()
{
	QMetaObject::invokeMethod(_socket, "close", Qt::QueuedConnection);
}

void Client::closeLater()
{
	QTimer::singleShot(scdtime(std::chrono::seconds(10)), _socket, [this](){
		if(_socket && _socket->state() == QAbstractSocket::ConnectedState)
			_socket->close();
	});
}

void Client::sendMessage(const QByteArray &message)
{
	QMetaObject::invokeMethod(this, "doSend", Qt::QueuedConnection,
							  Q_ARG(QByteArray, message));
}

void Client::doSend(const QByteArray &message)
{
	_socket->sendBinaryMessage(message);
}

void Client::onRegister(const RegisterMessage &message, QDataStream &stream)
{
	if(_state != Authenticating)
		throw UnexpectedException<RegisterMessage>();
	if(_loginNonce != message.nonce)
		throw MessageException("Invalid nonce in RegisterMessagee");
	_loginNonce.clear();

	try {
		QScopedPointer<AsymmetricCryptoInfo> crypto(message.createCryptoInfo(rngPool.localData()));
		verifySignature(stream, crypto->signatureKey(), crypto.data());
	} catch(CryptoPP::HashVerificationFilter::HashVerificationFailed &e) {
		qWarning() << "Authentication error:" << e.what();
		throw ClientErrorException(ErrorMessage::AuthenticationError);
	}

	_deviceId = _database->addNewDevice(message.deviceName,
										message.signAlgorithm,
										message.signKey,
										message.cryptAlgorithm,
										message.cryptKey);
	_catStr = "client." + _deviceId.toByteArray();
	_logCat.reset(new QLoggingCategory(_catStr.constData()));

	qDebug() << "Created new device and user accounts";
	sendMessage(serializeMessage<AccountMessage>(_deviceId));
	_state = Idle;
	emit connected(_deviceId);
}

void Client::onLogin(const LoginMessage &message, QDataStream &stream)
{
	if(_state != Authenticating)
		throw UnexpectedException<LoginMessage>();
	if(_loginNonce != message.nonce)
		throw MessageException("Invalid nonce in LoginMessage");
	_loginNonce.clear();

	//load public key to verify signature
	try {
		QScopedPointer<AsymmetricCryptoInfo> crypto(_database->loadCrypto(message.deviceId, rngPool.localData()));
		if(!crypto)
			throw ClientErrorException(ErrorMessage::AuthenticationError);
		verifySignature(stream, crypto->signatureKey(), crypto.data());
	} catch(CryptoPP::HashVerificationFilter::HashVerificationFailed &e) {
		qWarning() << "Authentication error:" << e.what();
		throw ClientErrorException(ErrorMessage::AuthenticationError);
	}

	_deviceId = message.deviceId;
	_catStr = "client." + _deviceId.toByteArray();
	_logCat.reset(new QLoggingCategory(_catStr.constData()));

	_database->updateLogin(_deviceId, message.name);
	qDebug() << "Device successfully logged in";

	//load changecount early to find out if data changed
	_cachedChanges = _database->changeCount(_deviceId);
	sendMessage(serializeMessage(WelcomeMessage(_cachedChanges > 0)));
	_state = Idle;
	emit connected(_deviceId);

	// send changed, always send info msg first, because count was preloaded (no force)
	// in case of no changes, send nothing if no changes
	triggerDownload(true, _cachedChanges == 0);
}

void Client::onSync(const SyncMessage &message)
{
	if(_state != Idle)
		throw UnexpectedException<SyncMessage>();
	Q_UNUSED(message)
	triggerDownload();
}

void Client::onChange(const ChangeMessage &message)
{
	if(_state != Idle)
		throw UnexpectedException<ChangeMessage>();

	_database->addChange(_deviceId,
						 message.dataId,
						 message.keyIndex,
						 message.salt,
						 message.data);

	sendMessage(serializeMessage<ChangeAckMessage>(message.dataId));
}

void Client::onChangedAck(const ChangedAckMessage &message)
{
	_database->completeChange(_deviceId, message.dataIndex);
	_activeDownloads.removeOne(message.dataIndex);
	//trigger next download. method itself decides when and how etc.
	triggerDownload();
}

void Client::triggerDownload(bool forceUpdate, bool skipNoChanges)
{
	auto updateChange = forceUpdate;

	auto cnt = _downLimit - _activeDownloads.size();
	if(cnt >= _downThreshold) {
		auto changes = _database->loadNextChanges(_deviceId, cnt, _activeDownloads.size());
		foreach(auto change, changes) {
			if(_cachedChanges == 0) {
				updateChange = true;
				_cachedChanges = _database->changeCount(_deviceId) - _activeDownloads.size();
			}

			if(updateChange) {
				ChangedInfoMessage message;
				message.changeEstimate = _cachedChanges;
				std::tie(message.dataIndex, message.keyIndex, message.salt, message.data) = change;
				sendMessage(serializeMessage<ChangedInfoMessage>(message));
				updateChange = false; //only the first message has that info
			} else {
				ChangedMessage message;
				std::tie(message.dataIndex, message.keyIndex, message.salt, message.data) = change;
				sendMessage(serializeMessage<ChangedMessage>(message));
			}
			_activeDownloads.append(std::get<0>(change));
			_cachedChanges--;
		}
	}

	if(_activeDownloads.isEmpty() && !skipNoChanges) {
		_cachedChanges = 0; //to make shure the next message is a ChangedInfoMessage
		sendMessage(serializeMessage<LastChangedMessage>({}));
	}
}

// ------------- Exceptions Implementation -------------

MessageException::MessageException(const QByteArray &message) :
	_msg(message)
{}

const char *MessageException::what() const noexcept
{
	return _msg.constData();
}

void MessageException::raise() const
{
	throw (*this);
}

QException *MessageException::clone() const
{
	return new MessageException(_msg);
}



ClientErrorException::ClientErrorException(ErrorMessage::ErrorType type, const QString &message, bool canRecover) :
	QException(),
	ErrorMessage(type, message, canRecover)
{}

const char *ClientErrorException::what() const noexcept
{
	QString s;
	QDebug(&s) << ErrorMessage(*this);
	_msg = s.toUtf8();
	return _msg.constData();
}

void ClientErrorException::raise() const
{
	throw *this;
}

QException *ClientErrorException::clone() const
{
	return new ClientErrorException(type, message, canRecover);
}



template<typename TMessage>
UnexpectedException<TMessage>::UnexpectedException() :
	ClientErrorException(UnexpectedMessageError,
						 QStringLiteral("Received unexpected %1").arg(QString::fromUtf8(TMessage::staticMetaObject.className())),
						 true)
{}
