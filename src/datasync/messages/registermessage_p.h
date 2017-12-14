#ifndef REGISTERMESSAGE_H
#define REGISTERMESSAGE_H

#include "message_p.h"
#include "asymmetriccrypto_p.h"

#include <functional>

#include <cryptopp/rsa.h>

namespace QtDataSync {

class Q_DATASYNC_EXPORT RegisterMessage
{
	Q_GADGET

	Q_PROPERTY(QByteArray signAlgorithm MEMBER signAlgorithm)
	Q_PROPERTY(QByteArray signKey MEMBER signKey)
	Q_PROPERTY(QByteArray cryptAlgorithm MEMBER cryptAlgorithm)
	Q_PROPERTY(QByteArray cryptKey MEMBER cryptKey)
	Q_PROPERTY(QString deviceName MEMBER deviceName)
	Q_PROPERTY(quint64 nonce MEMBER nonce)

public:
	RegisterMessage();
	RegisterMessage(const QString &deviceName,
					quint64 nonce,
					const QSharedPointer<CryptoPP::X509PublicKey> &signKey,
					const QSharedPointer<CryptoPP::X509PublicKey> &cryptKey,
					AsymmetricCrypto *crypto);

	QByteArray signAlgorithm;
	QByteArray signKey;
	QByteArray cryptAlgorithm;
	QByteArray cryptKey;
	QString deviceName;
	quint64 nonce;

	AsymmetricCrypto *createCrypto(QObject *parent = nullptr);
	QSharedPointer<CryptoPP::X509PublicKey> getSignKey(CryptoPP::RandomNumberGenerator &rng, AsymmetricCrypto *crypto);
	QSharedPointer<CryptoPP::X509PublicKey> getCryptKey(CryptoPP::RandomNumberGenerator &rng, AsymmetricCrypto *crypto);
};

Q_DATASYNC_EXPORT QDataStream &operator<<(QDataStream &stream, const RegisterMessage &message);
Q_DATASYNC_EXPORT QDataStream &operator>>(QDataStream &stream, RegisterMessage &message);

}

Q_DECLARE_METATYPE(QtDataSync::RegisterMessage)

#endif // REGISTERMESSAGE_H
