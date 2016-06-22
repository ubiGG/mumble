// Copyright 2005-2016 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "murmur_pch.h"

// Needed for MumbleSSL::qsslSanityCheck()
#ifdef Q_OS_LINUX
# include <dlfcn.h>
# include <link.h>
#endif

#include "SSL.h"

#include "Version.h"

void MumbleSSL::initialize() {
	// Let Qt initialize OpenSSL...
	QSslSocket::supportsSsl();

	// Check that we aren't in a situation where
	// Qt has dynamically loaded *another*
	// version/soname of libssl and/or libcrypto
	// alongside the copy we link against ourselves.
	//
	// This can happen when Qt is built without the
	// -openssl-linked configure option. When Qt is
	// built without -openssl-linked, it will try
	// to dynamically load the OpenSSL libraries.
	// When Qt dynamically loads OpenSSL and there
	// are multiple version of libcrypto and libssl
	// available on the system, it is possible for
	// Qt to load another version of OpenSSL in
	// addition to the one our binary is linked
	// against. Typically, this will happen if
	// Mumble or Murmur are linked against
	// an older version, and a newer version is
	// available on the system as well.
	//
	// If we're in a situation where two version of
	// OpenSSL's libraries are loaded into our process,
	// we abort immediately. Things WILL go wrong.
	MumbleSSL::qsslSanityCheck();
}

// Check that we haven't loaded multiple copies of OpenSSL
// into our process by accident.
void MumbleSSL::qsslSanityCheck() {
#ifdef Q_OS_LINUX
	struct link_map *lm = NULL;
	void *self = dlopen(NULL, RTLD_NOW);
	if (self == NULL) {
		qFatal("SSL: could not dlopen program binary");
	}

	if (dlinfo(self, RTLD_DI_LINKMAP, &lm) == -1) {
		qFatal("SSL: unable to acquire link_map: %s", dlerror());
	}
	if (lm == NULL) {
		qFatal("SSL: link_map is NULL");
	}

	QStringList libssl;
	QStringList libcrypto;

	while (lm != NULL) {
		QString name = QString::fromLocal8Bit(lm->l_name);
		if (name.contains(QLatin1String("libssl"))) {
			libssl << name;
		} else if (name.contains(QLatin1String("libcrypto"))) {
			libcrypto << name;
		}
		lm = lm->l_next;
	}

	bool ok = true;
	if (libcrypto.size() == 0 || libssl.size() == 0) {
		qFatal("SSL library query failed: %i libcrypto's and %i libssl's found.", libcrypto.size(), libssl.size());
	}
	if (libssl.size() > 1) {
		qCritical("Found multiple libssl.so copies in binary: %s", qPrintable(libssl.join(QLatin1String(", "))));
		ok = false;
	}
	if (libcrypto.size() > 1) {
		qCritical("Found multiple libcrypto.so copies in binary: %s", qPrintable(libcrypto.join(QLatin1String(", "))));
		ok = false;
	}

	if (!ok) {
		qFatal("Aborting due to previous errors");
	}
#endif
}

QString MumbleSSL::defaultOpenSSLCipherString() {
	return QLatin1String("EECDH+AESGCM:EDH+aRSA+AESGCM:DHE-RSA-AES256-SHA:DHE-RSA-AES128-SHA:AES256-SHA:AES128-SHA");
}

QList<QSslCipher> MumbleSSL::ciphersFromOpenSSLCipherString(QString cipherString) {
	QList<QSslCipher> chosenCiphers;

	SSL_CTX *ctx = NULL;
	SSL *ssl = NULL;
	const SSL_METHOD *meth = NULL;
	int i = 0;

	QByteArray csbuf = cipherString.toLatin1();
	const char *ciphers = csbuf.constData();

	meth = SSLv23_server_method();
	if (meth == NULL) {
		qWarning("MumbleSSL: unable to get SSL method");
		goto out;
	}

	// We use const_cast to be compatible with OpenSSL 0.9.8.
	ctx = SSL_CTX_new(const_cast<SSL_METHOD *>(meth));
	if (ctx == NULL) {
		qWarning("MumbleSSL: unable to allocate SSL_CTX");
		goto out;
	}

	if (!SSL_CTX_set_cipher_list(ctx, ciphers)) {
		qWarning("MumbleSSL: error parsing OpenSSL cipher string in ciphersFromOpenSSLCipherString");
		goto out;
	}

	ssl = SSL_new(ctx);
	if (ssl == NULL) {
		qWarning("MumbleSSL: unable to create SSL object in ciphersFromOpenSSLCipherString");
		goto out;
	}

	while (1) {
		const char *name = SSL_get_cipher_list(ssl, i);
		if (name == NULL) {
			break;
		}
#if QT_VERSION >= 0x050300
		QSslCipher c = QSslCipher(QString::fromLatin1(name));
		if (!c.isNull()) {
			chosenCiphers << c;
		}
#else
		foreach (const QSslCipher &c, QSslSocket::supportedCiphers()) {
			if (c.name() == QString::fromLatin1(name)) {
				chosenCiphers << c;
			}
		}
#endif
		++i;
	}

out:
	SSL_CTX_free(ctx);
	SSL_free(ssl);
	return chosenCiphers;
}

void MumbleSSL::addSystemCA() {
#if QT_VERSION < 0x040700 && !defined(NO_SYSTEM_CA_OVERRIDE)
#if defined(Q_OS_WIN)
	QStringList qsl;
	qsl << QLatin1String("Ca");
	qsl << QLatin1String("Root");
	qsl << QLatin1String("AuthRoot");
	foreach(const QString &store, qsl) {
		HCERTSTORE hCertStore;
		PCCERT_CONTEXT pCertContext = NULL;

		bool found = false;

		hCertStore = CertOpenSystemStore(NULL, store.utf16());
		if (! hCertStore) {
			qWarning("SSL: Failed to open CA store %s", qPrintable(store));
			continue;
		}

		while (pCertContext = CertEnumCertificatesInStore(hCertStore, pCertContext)) {
			QByteArray qba(reinterpret_cast<const char *>(pCertContext->pbCertEncoded), pCertContext->cbCertEncoded);

			QList<QSslCertificate> ql = QSslCertificate::fromData(qba, QSsl::Pem);
			ql += QSslCertificate::fromData(qba, QSsl::Der);
			if (! ql.isEmpty()) {
				found = true;
				QSslSocket::addDefaultCaCertificates(ql);
			}
		}
		if (found)
			qWarning("SSL: Added CA certificates from system store '%s'", qPrintable(store));

		CertCloseStore(hCertStore, 0);
	}

#elif defined(Q_OS_MAC)
	CFArrayRef certs = NULL;
	bool found = false;

	if (SecTrustCopyAnchorCertificates(&certs) == noErr) {
		int ncerts = CFArrayGetCount(certs);
		for (int i = 0; i < ncerts; i++) {
			CFDataRef data = NULL;

			SecCertificateRef cert = reinterpret_cast<SecCertificateRef>(const_cast<void *>(CFArrayGetValueAtIndex(certs, i)));
			if (! cert)
				continue;

			if (SecKeychainItemExport(cert, kSecFormatX509Cert, kSecItemPemArmour, NULL, &data) == noErr) {
				const char *ptr = reinterpret_cast<const char *>(CFDataGetBytePtr(data));
				int len = CFDataGetLength(data);
				QByteArray qba(ptr, len);

				QList<QSslCertificate> ql = QSslCertificate::fromData(qba, QSsl::Pem);
				if (! ql.isEmpty()) {
					found = true;
					QSslSocket::addDefaultCaCertificates(ql);
				}
			}
		}

		CFRelease(certs);

		if (found)
			qWarning("SSL: Added CA certificates from 'System Roots' store.");
	}
#elif defined(Q_OS_UNIX)
	QStringList qsl;

#ifdef SYSTEM_CA_DIR
	QSslSocket::addDefaultCaCertificates(QLatin1String(MUMTEXT(SYSTEM_CA_DIR)));
#else
#ifdef SYSTEM_CA_BUNDLE
	qsl << QLatin1String(MUMTEXT(SYSTEM_CA_BUNDLE));
#else
#ifdef __FreeBSD__
	qsl << QLatin1String("/usr/local/share/certs/ca-root-nss.crt");
#else
	qsl << QLatin1String("/etc/pki/tls/certs/ca-bundle.crt");
	qsl << QLatin1String("/etc/ssl/certs/ca-certificates.crt");
#endif
#endif

	foreach(const QString &filename, qsl) {
		QFile f(filename);
		if (f.exists() && f.open(QIODevice::ReadOnly)) {
			QList<QSslCertificate> ql = QSslCertificate::fromDevice(&f, QSsl::Pem);
			ql += QSslCertificate::fromDevice(&f, QSsl::Der);
			if (! ql.isEmpty()) {
				qWarning("SSL: Added CA certificates from '%s'", qPrintable(filename));
				QSslSocket::addDefaultCaCertificates(ql);
			}
		}
	}
#endif // SYSTEM_CA_DIR
#endif // Q_OS_UNIX

	QSet<QByteArray> digests;
	QList<QSslCertificate> ql;
	foreach(const QSslCertificate &crt, QSslSocket::defaultCaCertificates()) {
		QByteArray digest = crt.digest(QCryptographicHash::Sha3_512);
		if (! digests.contains(digest) && crt.isValid()) {
			ql << crt;
			digests.insert(digest);
		}
	}
	QSslSocket::setDefaultCaCertificates(ql);
#endif // NO_SYSTEM_CA_OVERRIDE

#if QT_VERSION >= 0x040800
	// Don't perform on-demand loading of root certificates
	QSslSocket::addDefaultCaCertificates(QSslSocket::systemCaCertificates());
#endif

#ifdef Q_OS_WIN
	// Work around issue #1271.
	// Skype's click-to-call feature creates an enormous
	// amount of certificates in the Root CA store.
	{
		QSslConfiguration sslCfg = QSslConfiguration::defaultConfiguration();
		QList<QSslCertificate> caList = sslCfg.caCertificates();

		QList<QSslCertificate> filteredCaList;
		foreach (QSslCertificate cert, caList) {
#if QT_VERSION >= 0x050000
			QStringList orgs = cert.subjectInfo(QSslCertificate::Organization);
			bool skip = false;
			foreach (QString ou, orgs) {
				if (ou.contains(QLatin1String("Skype"), Qt::CaseInsensitive)) {
					skip = true;
					break;
				}
			}
			if (skip) {
				continue;
			}
#else
			QString ou = cert.subjectInfo(QSslCertificate::Organization);
			if (ou.contains(QLatin1String("Skype"), Qt::CaseInsensitive)) {
				continue;
			}
#endif
			filteredCaList.append(cert);
		}

		sslCfg.setCaCertificates(filteredCaList);
		QSslConfiguration::setDefaultConfiguration(sslCfg);

		qWarning("SSL: CA certificate filter applied. Filtered size: %i, original size: %i", filteredCaList.size(), caList.size());
	}
#endif
}

QString MumbleSSL::protocolToString(QSsl::SslProtocol protocol) {
	switch(protocol) {
		case QSsl::SslV3: return QLatin1String("SSL 3");
		case QSsl::SslV2: return QLatin1String("SSL 2");
#if QT_VERSION >= 0x050000
		case QSsl::TlsV1_0: return QLatin1String("TLS 1.0");
		case QSsl::TlsV1_1: return QLatin1String("TLS 1.1");
		case QSsl::TlsV1_2: return QLatin1String("TLS 1.2");
#else
		case QSsl::TlsV1: return  QLatin1String("TLS 1.0");
#endif
		case QSsl::AnyProtocol: return QLatin1String("AnyProtocol");
#if QT_VERSION >= 0x040800
		case QSsl::TlsV1SslV3: return QLatin1String("TlsV1SslV3");
		case QSsl::SecureProtocols: return QLatin1String("SecureProtocols");
#endif
		default:
		case QSsl::UnknownProtocol: return QLatin1String("UnknownProtocol");
	}
}
