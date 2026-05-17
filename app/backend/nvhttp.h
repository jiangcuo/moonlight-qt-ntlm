#pragma once

#include "identitymanager.h"
#include "nvapp.h"
#include "nvaddress.h"

#include <Limelight.h>

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class NvComputer;

class NvDisplayMode
{
public:
    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width &&
                height == other.height &&
                refreshRate == other.refreshRate;
    }

    int width;
    int height;
    int refreshRate;
};
Q_DECLARE_TYPEINFO(NvDisplayMode, Q_PRIMITIVE_TYPE);

class GfeHttpResponseException : public std::exception
{
public:
    GfeHttpResponseException(int statusCode, QString message) :
        m_StatusCode(statusCode),
        m_StatusMessage(message)
    {

    }

    const char* what() const throw()
    {
        return m_StatusMessage.toLatin1();
    }

    const char* getStatusMessage() const
    {
        return m_StatusMessage.toLatin1();
    }

    int getStatusCode() const
    {
        return m_StatusCode;
    }

    QString toQString() const
    {
        return m_StatusMessage + " (Error " + QString::number(m_StatusCode) + ")";
    }

private:
    int m_StatusCode;
    QString m_StatusMessage;
};

class QtNetworkReplyException : public std::exception
{
public:
    QtNetworkReplyException(QNetworkReply::NetworkError error, QString errorText) :
        m_Error(error),
        m_ErrorText(errorText)
    {

    }

    const char* what() const throw()
    {
        return m_ErrorText.toLatin1();
    }

    const char* getErrorText() const
    {
        return m_ErrorText.toLatin1();
    }

    QNetworkReply::NetworkError getError() const
    {
        return m_Error;
    }

    QString toQString() const
    {
        return m_ErrorText + " (Error " + QString::number(m_Error) + ")";
    }

private:
    QNetworkReply::NetworkError m_Error;
    QString m_ErrorText;
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    enum NvLogLevel {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };

    explicit NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert);

    explicit NvHTTP(NvComputer* computer);

    static
    int
    getCurrentGame(QString serverInfo);

    QString
    getServerInfo(NvLogLevel logLevel, bool fastFail = false);

    static
    void
    verifyResponseStatus(QString xml);

    static
    QString
    getXmlString(QString xml,
                 QString tagName);

    static
    QByteArray
    getXmlStringFromHex(QString xml,
                        QString tagName);

    QString
    openConnectionToString(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs = 0,
                           NvLogLevel logLevel = NVLL_VERBOSE);

    void setServerCert(QSslCertificate serverCert);

    void setAddress(NvAddress address);
    void setHttpsPort(uint16_t port);

    NvAddress address();

    QSslCertificate serverCert();

    uint16_t httpPort();

    uint16_t httpsPort();

    static
    QVector<int>
    parseQuad(QString quad);

    void
    quitApp();

    void
    startApp(QString verb,
             bool isGfe,
             int appId,
             PSTREAM_CONFIGURATION streamConfig,
             bool sops,
             bool localAudio,
             int gamepadMask,
             bool persistGameControllersOnDisconnect,
             QString& rtspSessionUrl);

    // 新增：支持用户名密码认证的startApp重载方法
    void
    startApp(QString verb,
             bool isGfe,
             int appId,
             PSTREAM_CONFIGURATION streamConfig,
             bool sops,
             bool localAudio,
             int gamepadMask,
             bool persistGameControllersOnDisconnect,
             QString& rtspSessionUrl,
             bool enableUserpass,
             QString username = QString(),
             QString password = QString());

    QVector<NvApp>
    getAppList();

    QVector<NvApp>
    getAppList(bool ignoreSsl);
    
    QVector<NvApp>
    getAppList(bool ignoreSsl, const QString& username, const QString& password);

    QImage
    getBoxArt(int appId);

    static
    QVector<NvDisplayMode>
    getDisplayModeList(QString serverInfo);

    void setGatewayMode(const QString& gatewayHost, const QString& token);
    static void setGlobalGateway(const QString& gatewayHost, const QString& token);
    static void clearGlobalGateway();

    QUrl m_BaseUrlHttp;
    QUrl m_BaseUrlHttps;
private:
    void
    handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

    QString openConnectionToStringIgnoreSsl(QUrl baseUrl,
                                           QString command,
                                           QString arguments,
                                           int timeoutMs = 0,
                                           NvLogLevel logLevel = NVLL_ERROR);

    QNetworkReply* openConnection(QUrl baseUrl,
                                QString command,
                                QString arguments,
                                int timeoutMs = 0,
                                NvLogLevel logLevel = NVLL_ERROR);

    QUrl buildGatewayProxyUrl(QUrl originalUrl);

    NvAddress m_Address;
    QNetworkAccessManager m_Nam;
    QSslCertificate m_ServerCert;

    QString m_GatewayHost;
    int m_GatewayPort = 9443;
    QString m_GatewayToken;

public:
    static QString s_GlobalGatewayHost;
    static int s_GlobalGatewayPort;
    static QString s_GlobalGatewayToken;
};
