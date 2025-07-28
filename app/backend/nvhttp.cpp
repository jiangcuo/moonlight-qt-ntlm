#include "nvcomputer.h"
#include <Limelight.h>

#include <QDebug>
#include <QUuid>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QImageReader>
#include <QtEndian>
#include <QNetworkProxy>

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000
#define QUIT_TIMEOUT_MS 30000

NvHTTP::NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert) :
    m_ServerCert(serverCert)
{
    m_BaseUrlHttp.setScheme("http");
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setHttpsPort(httpsPort);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam.setProxy(noProxy);

    connect(&m_Nam, &QNetworkAccessManager::sslErrors, this, &NvHTTP::handleSslErrors);
}

NvHTTP::NvHTTP(NvComputer* computer) :
    NvHTTP(computer->activeAddress, computer->activeHttpsPort, computer->serverCert)
{

}

void NvHTTP::setServerCert(QSslCertificate serverCert)
{
    m_ServerCert = serverCert;
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttp.setHost(address.address());
    m_BaseUrlHttps.setHost(address.address());

    m_BaseUrlHttp.setPort(address.port());
}

void NvHTTP::setHttpsPort(uint16_t port)
{
    m_BaseUrlHttps.setPort(port);
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

QSslCertificate NvHTTP::serverCert()
{
    return m_ServerCert;
}

uint16_t NvHTTP::httpPort()
{
    return m_BaseUrlHttp.port();
}

uint16_t NvHTTP::httpsPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState != nullptr && serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    QString serverInfo;

    // Check if we have a pinned cert and HTTPS port for this host yet
    if (!m_ServerCert.isNull() && httpsPort() != 0)
    {
        try
        {
            // Always try HTTPS first, since it properly reports
            // pairing status (and a few other attributes).
            serverInfo = openConnectionToString(m_BaseUrlHttps,
                                                "serverinfo",
                                                nullptr,
                                                fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                logLevel);
            // Throws if the request failed
            verifyResponseStatus(serverInfo);
        }
        catch (const GfeHttpResponseException& e)
        {
            if (e.getStatusCode() == 401)
            {
                // Certificate validation error, fallback to HTTP
                serverInfo = openConnectionToString(m_BaseUrlHttp,
                                                    "serverinfo",
                                                    nullptr,
                                                    fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                                    logLevel);
                verifyResponseStatus(serverInfo);
            }
            else
            {
                // Rethrow real errors
                throw e;
            }
        }
    }
    else
    {
        // Only use HTTP prior to pairing or fetching HTTPS port
        serverInfo = openConnectionToString(m_BaseUrlHttp,
                                            "serverinfo",
                                            nullptr,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        verifyResponseStatus(serverInfo);

        // Populate the HTTPS port
        uint16_t httpsPort = getXmlString(serverInfo, "HttpsPort").toUShort();
        if (httpsPort == 0) {
            httpsPort = DEFAULT_HTTPS_PORT;
        }
        setHttpsPort(httpsPort);

        // If we just needed to determine the HTTPS port, we'll try again over
        // HTTPS now that we have the port number
        if (!m_ServerCert.isNull()) {
            return getServerInfo(logLevel, fastFail);
        }
    }

    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                 bool isGfe,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool sops,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString& rtspSessionUrl)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   verb,
                                   "appid="+QString::number(appId)+
                                   "&mode="+QString::number(streamConfig->width)+"x"+
                                   QString::number(streamConfig->height)+"x"+
                                   // Using an FPS value over 60 causes SOPS to default to 720p60,
                                   // so force it to 0 to ensure the correct resolution is set. We
                                   // used to use 60 here but that locked the frame rate to 60 FPS
                                   // on GFE 3.20.3. We don't need this hack for Sunshine.
                                   QString::number((streamConfig->fps > 60 && isGfe) ? 0 : streamConfig->fps)+
                                   "&additionalStates=1&sops="+QString::number(sops ? 1 : 0)+
                                   "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                                   "&rikeyid="+QString::number(riKeyId)+
                                   ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   LiGetLaunchUrlQueryParameters(),
                                   LAUNCH_TIMEOUT_MS);

    qInfo() << "Launch response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
}

// 新增：支持用户名密码认证的startApp重载方法实现
void
NvHTTP::startApp(QString verb,
                 bool isGfe,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool sops,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString& rtspSessionUrl,
                 bool enableUserpass,
                 QString username,
                 QString password)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    QString arguments = "appid="+QString::number(appId)+
                       "&mode="+QString::number(streamConfig->width)+"x"+
                       QString::number(streamConfig->height)+"x"+
                       QString::number((streamConfig->fps > 60 && isGfe) ? 0 : streamConfig->fps)+
                       "&additionalStates=1&sops="+QString::number(sops ? 1 : 0)+
                       "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                       "&rikeyid="+QString::number(riKeyId)+
                       ((streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) ?
                           "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                            "")+
                       "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                       "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                       "&remoteControllersBitmap="+QString::number(gamepadMask)+
                       "&gcmap="+QString::number(gamepadMask)+
                       "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                       LiGetLaunchUrlQueryParameters();

    // 如果启用用户名密码认证，添加相关参数
    if (enableUserpass) {
        arguments += "&enable_userpass=true&username=" + username + "&password=" + password;
    }

    QString response;
    if (enableUserpass) {
        // 在用户名密码认证模式下，忽略SSL验证进行启动请求
        response = openConnectionToStringIgnoreSsl(m_BaseUrlHttps,
                                                  verb,
                                                  arguments,
                                                  LAUNCH_TIMEOUT_MS);
    } else {
        response = openConnectionToString(m_BaseUrlHttps,
                                        verb,
                                        arguments,
                                        LAUNCH_TIMEOUT_MS);
    }

    qInfo() << "Launch response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
}

void
NvHTTP::quitApp()
{
    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   "cancel",
                                   nullptr,
                                   QUIT_TIMEOUT_MS);

    qInfo() << "Quit response:" << response;

    // Throws if the request failed
    verifyResponseStatus(response);

    // Newer GFE versions will just return success even if quitting fails
    // if we're not the original requester.
    if (getCurrentGame(getServerInfo(NvHTTP::NVLL_ERROR)) != 0) {
        // Generate a synthetic GfeResponseException letting the caller know
        // that they can't kill someone else's stream.
        throw GfeHttpResponseException(599, "");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (name == QString("DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (name == QString("Width")) {
                modes.last().width = xmlReader.readElementText().toInt();
            }
            else if (name == QString("Height")) {
                modes.last().height = xmlReader.readElementText().toInt();
            }
            else if (name == QString("RefreshRate")) {
                modes.last().refreshRate = xmlReader.readElementText().toInt();
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR);
    verifyResponseStatus(appxml);

    qInfo() << "[DEBUG] Starting XML parsing...";
    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    int appCount = 0;
    
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (name == QString("App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "[DEBUG] Invalid applist XML - incomplete app entry";
                    qWarning() << "Invalid applist XML";
                    Q_ASSERT(false);
                    return QVector<NvApp>();
                }
                apps.append(NvApp());
                appCount++;
                qInfo() << "[DEBUG] Found new app entry #" << appCount;
            }
            else if (name == QString("AppTitle")) {
                QString appTitle = xmlReader.readElementText();
                apps.last().name = appTitle;
                qInfo() << "[DEBUG] App" << appCount << "title:" << appTitle;
            }
            else if (name == QString("ID")) {
                QString idText = xmlReader.readElementText();
                int appId = idText.toInt();
                apps.last().id = appId;
                qInfo() << "[DEBUG] App" << appCount << "ID:" << appId;
            }
            else if (name == QString("IsHdrSupported")) {
                QString hdrText = xmlReader.readElementText();
                bool hdrSupported = hdrText == "1";
                apps.last().hdrSupported = hdrSupported;
                qInfo() << "[DEBUG] App" << appCount << "HDR supported:" << hdrSupported;
            }
            else if (name == QString("IsAppCollectorGame")) {
                QString collectorText = xmlReader.readElementText();
                bool isCollector = collectorText == "1";
                apps.last().isAppCollectorGame = isCollector;
                qInfo() << "[DEBUG] App" << appCount << "is collector game:" << isCollector;
            } else {
                qInfo() << "[DEBUG] Skipping unknown XML element:" << name.toString();
            }
        }
    }

    if (xmlReader.hasError()) {
        qWarning() << "[DEBUG] XML parsing error:" << xmlReader.errorString();
        qWarning() << "[DEBUG] Error at line:" << xmlReader.lineNumber() << "column:" << xmlReader.columnNumber();
    }

    qInfo() << "[DEBUG] XML parsing completed. Total apps found:" << apps.size();
    for (int i = 0; i < apps.size(); i++) {
        qInfo() << "[DEBUG] App" << (i+1) << "summary: ID=" << apps[i].id << "Name=" << apps[i].name;
    }

    return apps;
}

// 新增：支持忽略SSL验证的getAppList重载方法
QVector<NvApp>
NvHTTP::getAppList(bool ignoreSsl)
{
    qInfo() << "[DEBUG] getAppList called with ignoreSsl:" << ignoreSsl;
    qInfo() << "[DEBUG] Base HTTPS URL:" << m_BaseUrlHttps.toString();
    
    QString appxml;
    
    try {
        if (ignoreSsl) {
            qInfo() << "[DEBUG] Using SSL-ignored connection for applist";
            // 在用户名密码认证模式下，忽略SSL验证获取应用程序列表
            appxml = openConnectionToStringIgnoreSsl(m_BaseUrlHttps,
                                                     "applist",
                                                     nullptr,
                                                     REQUEST_TIMEOUT_MS,
                                                     NvLogLevel::NVLL_ERROR);
        } else {
            qInfo() << "[DEBUG] Using normal SSL connection for applist";
            appxml = openConnectionToString(m_BaseUrlHttps,
                                           "applist",
                                           nullptr,
                                           REQUEST_TIMEOUT_MS,
                                           NvLogLevel::NVLL_ERROR);
        }
    } catch (const QtNetworkReplyException& e) {
        qWarning() << "[DEBUG] Network exception caught in getAppList:";
        qWarning() << "[DEBUG] Exception error:" << e.getError();
        throw; // 重新抛出异常
    }
    
    qInfo() << "[DEBUG] AppXML received, length:" << appxml.length();
    qInfo() << "[DEBUG] AppXML content preview:" << appxml.left(300);
    
    verifyResponseStatus(appxml);
    qInfo() << "[DEBUG] Response status verified successfully";

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (name == QString("App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "Invalid applist XML";
                    Q_ASSERT(false);
                    return QVector<NvApp>();
                }
                apps.append(NvApp());
            }
            else if (name == QString("AppTitle")) {
                apps.last().name = xmlReader.readElementText();
            }
            else if (name == QString("ID")) {
                apps.last().id = xmlReader.readElementText().toInt();
            }
            else if (name == QString("IsHdrSupported")) {
                apps.last().hdrSupported = xmlReader.readElementText() == "1";
            }
            else if (name == QString("IsAppCollectorGame")) {
                apps.last().isAppCollectorGame = xmlReader.readElementText() == "1";
            }
        }
    }

    qInfo() << "Found" << apps.count() << "apps using" << (ignoreSsl ? "HTTPS (SSL ignored)" : "HTTPS");
    return apps;
}

// 新增：支持用户名密码认证的getAppList重载方法
QVector<NvApp>
NvHTTP::getAppList(bool ignoreSsl, const QString& username, const QString& password)
{
    qInfo() << "[DEBUG] getAppList called with ignoreSsl:" << ignoreSsl << "username:" << username;
    qInfo() << "[DEBUG] Base HTTPS URL:" << m_BaseUrlHttps.toString();
    
    QString appxml;
    QString arguments;
    
    // 构建认证参数
    if (!username.isEmpty() && !password.isEmpty()) {
        arguments = QString("enable_userpass=true&username=%1&password=%2")
                   .arg(QString(QUrl::toPercentEncoding(username)))
                   .arg(QString(QUrl::toPercentEncoding(password)));
        qInfo() << "[DEBUG] Using user-pass authentication arguments";
    }
    
    try {
        if (ignoreSsl) {
            qInfo() << "[DEBUG] Using SSL-ignored connection for applist with user-pass auth";
            appxml = openConnectionToStringIgnoreSsl(m_BaseUrlHttps,
                                                     "applist",
                                                     arguments.isEmpty() ? nullptr : arguments,
                                                     REQUEST_TIMEOUT_MS,
                                                     NvLogLevel::NVLL_ERROR);
        } else {
            qInfo() << "[DEBUG] Using normal SSL connection for applist with user-pass auth";
            appxml = openConnectionToString(m_BaseUrlHttps,
                                           "applist",
                                           arguments.isEmpty() ? nullptr : arguments,
                                           REQUEST_TIMEOUT_MS,
                                           NvLogLevel::NVLL_ERROR);
        }
    } catch (const QtNetworkReplyException& e) {
        qWarning() << "[DEBUG] Network exception caught in getAppList (user-pass version):";
        qWarning() << "[DEBUG] Exception error:" << e.getError();
        throw; // 重新抛出异常
    }
    
    qInfo() << "[DEBUG] AppXML received (user-pass), length:" << appxml.length();
    qInfo() << "[DEBUG] AppXML content preview (user-pass):" << appxml.left(300);
    
    verifyResponseStatus(appxml);
    qInfo() << "[DEBUG] Response status verified successfully (user-pass)";

    qInfo() << "[DEBUG] Starting XML parsing (user-pass)...";
    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    int appCount = 0;
    
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (name == QString("App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "[DEBUG] Invalid applist XML - incomplete app entry (user-pass)";
                    qWarning() << "Invalid applist XML";
                    Q_ASSERT(false);
                    return QVector<NvApp>();
                }
                apps.append(NvApp());
                appCount++;
                qInfo() << "[DEBUG] Found new app entry (user-pass) #" << appCount;
            }
            else if (name == QString("AppTitle")) {
                QString appTitle = xmlReader.readElementText();
                apps.last().name = appTitle;
                qInfo() << "[DEBUG] App (user-pass)" << appCount << "title:" << appTitle;
            }
            else if (name == QString("ID")) {
                QString idText = xmlReader.readElementText();
                int appId = idText.toInt();
                apps.last().id = appId;
                qInfo() << "[DEBUG] App (user-pass)" << appCount << "ID:" << appId;
            }
            else if (name == QString("IsHdrSupported")) {
                QString hdrText = xmlReader.readElementText();
                bool hdrSupported = hdrText == "1";
                apps.last().hdrSupported = hdrSupported;
                qInfo() << "[DEBUG] App (user-pass)" << appCount << "HDR supported:" << hdrSupported;
            }
            else if (name == QString("IsAppCollectorGame")) {
                QString collectorText = xmlReader.readElementText();
                bool isCollector = collectorText == "1";
                apps.last().isAppCollectorGame = isCollector;
                qInfo() << "[DEBUG] App (user-pass)" << appCount << "is collector game:" << isCollector;
            } else {
                qInfo() << "[DEBUG] Skipping unknown XML element (user-pass):" << name.toString();
            }
        }
    }

    if (xmlReader.hasError()) {
        qWarning() << "[DEBUG] XML parsing error (user-pass):" << xmlReader.errorString();
        qWarning() << "[DEBUG] Error at line:" << xmlReader.lineNumber() << "column:" << xmlReader.columnNumber();
    }

    qInfo() << "[DEBUG] XML parsing completed (user-pass). Total apps found:" << apps.size();
    for (int i = 0; i < apps.size(); i++) {
        qInfo() << "[DEBUG] App (user-pass)" << (i+1) << "summary: ID=" << apps[i].id << "Name=" << apps[i].name;
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (xmlReader.name() == QString("root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected for unpaired PCs when we fetch serverinfo over HTTPS
                    qWarning() << "Request failed:" << statusCode << statusMessage;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE);
    QImage image = QImageReader(reply).read();
    delete reply;

    return image;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    QString str = getXmlString(xml, tagName);
    if (str == nullptr)
    {
        return nullptr;
    }

    return QByteArray::fromHex(str.toLatin1());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return nullptr;
}

void NvHTTP::handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    bool ignoreErrors = true;

    if (m_ServerCert.isNull()) {
        // We should never make an HTTPS request without a cert
        Q_ASSERT(!m_ServerCert.isNull());
        return;
    }

    for (const QSslError& error : errors) {
        if (m_ServerCert != error.certificate()) {
            ignoreErrors = false;
            break;
        }
    }

    if (ignoreErrors) {
        reply->ignoreSslErrors(errors);
    }
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

// 新增：忽略SSL验证的连接方法
QString
NvHTTP::openConnectionToStringIgnoreSsl(QUrl baseUrl,
                                       QString command,
                                       QString arguments,
                                       int timeoutMs,
                                       NvLogLevel logLevel)
{
    // Debug: Function entry
    qInfo() << "[DEBUG] openConnectionToStringIgnoreSsl called";
    qInfo() << "[DEBUG] Command:" << command;
    qInfo() << "[DEBUG] Base URL:" << baseUrl.toString();
    qInfo() << "[DEBUG] Arguments:" << (arguments.isNull() ? "NULL" : arguments);
    qInfo() << "[DEBUG] Timeout:" << timeoutMs << "ms";

    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Use a common UID for Moonlight clients to allow them to quit
    // games for each other (otherwise GFE gets screwed up and it requires
    // manual intervention to solve).
    QString queryString = "uniqueid=0123456789ABCDEF&uuid=" +
                 QUuid::createUuid().toRfc4122().toHex() +
                 ((arguments != nullptr) ? ("&" + arguments) : "");
    url.setQuery(queryString);

    // Debug: Final URL
    qInfo() << "[DEBUG] Final request URL:" << url.toString();

    QNetworkRequest request(url);

    // Add our client certificate
    auto sslConfig = IdentityManager::get()->getSslConfig();
    request.setSslConfiguration(sslConfig);
    
    // Debug: SSL configuration
    qInfo() << "[DEBUG] SSL certificate loaded, protocol:" << sslConfig.protocol();
    qInfo() << "[DEBUG] SSL peer verify mode:" << sslConfig.peerVerifyMode();

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam.setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    // Debug: Starting network request
    qInfo() << "[DEBUG] Creating network request...";
    QNetworkReply* reply = m_Nam.get(request);
    qInfo() << "[DEBUG] Network request created, waiting for response...";

    // 忽略所有SSL错误（仅用于用户名密码认证模式下的应用程序列表获取）
    connect(reply, QOverload<const QList<QSslError>&>::of(&QNetworkReply::sslErrors),
            [reply](const QList<QSslError>& errors) {
                qInfo() << "[DEBUG] SSL errors detected, count:" << errors.size();
                for (const QSslError& error : errors) {
                    qInfo() << "[DEBUG] SSL Error:" << error.errorString();
                }
                qInfo() << "[DEBUG] Ignoring SSL errors for applist request (user-pass auth mode)";
                reply->ignoreSslErrors();
            });

    // Run the request with a timeout if requested
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        qInfo() << "[DEBUG] Timeout set to:" << timeoutMs << "ms";
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request (SSL ignored):" << url.toString();
    }
    
    qInfo() << "[DEBUG] Starting event loop, waiting for network response...";
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    qInfo() << "[DEBUG] Event loop finished";

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        qWarning() << "[DEBUG] Request timed out! Aborting...";
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    } else {
        qInfo() << "[DEBUG] Request completed successfully";
    }

    // We must clear out cached authentication and connections or
    // GFE will puke next time
    m_Nam.clearAccessCache();
    qInfo() << "[DEBUG] Network access cache cleared";

    // Debug: Check response status
    qInfo() << "[DEBUG] HTTP status code:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qInfo() << "[DEBUG] Network error code:" << reply->error();
    qInfo() << "[DEBUG] Error string:" << reply->errorString();

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        qWarning() << "[DEBUG] Network request failed!";
        qWarning() << "[DEBUG] Error code:" << reply->error();
        qWarning() << "[DEBUG] Error description:" << reply->errorString();
        qWarning() << "[DEBUG] HTTP status:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (reply->error() == QNetworkReply::OperationCanceledError) {
            qWarning() << "[DEBUG] Operation was canceled (timeout)";
            QtNetworkReplyException exception(QNetworkReply::TimeoutError, "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            qWarning() << "[DEBUG] Other network error occurred";
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            delete reply;
            throw exception;
        }
    }

    // 读取响应
    qInfo() << "[DEBUG] Reading response data...";
    QString ret;
    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    qInfo() << "[DEBUG] Response data length:" << ret.length() << "characters";
    qInfo() << "[DEBUG] Response content preview (first 200 chars):" << ret.left(200);
    
    if (ret.isEmpty()) {
        qWarning() << "[DEBUG] WARNING: Response is empty!";
    } else if (ret.contains("status_code")) {
        qInfo() << "[DEBUG] Response contains status_code field";
        if (ret.contains("status_code=\"200\"") || ret.contains("status_code\": 200")) {
            qInfo() << "[DEBUG] Status code indicates success (200)";
        } else {
            qWarning() << "[DEBUG] Status code might indicate failure (not 200)";
        }
    }
    
    delete reply;
    qInfo() << "[DEBUG] openConnectionToStringIgnoreSsl completed successfully";

    return ret;
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel)
{
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Use a common UID for Moonlight clients to allow them to quit
    // games for each other (otherwise GFE gets screwed up and it requires
    // manual intervention to solve).
    url.setQuery("uniqueid=0123456789ABCDEF&uuid=" +
                 QUuid::createUuid().toRfc4122().toHex() +
                 ((arguments != nullptr) ? ("&" + arguments) : ""));

    QNetworkRequest request(url);

    // Add our client certificate
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam.setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    QNetworkReply* reply = m_Nam.get(request);

    // Run the request with a timeout if requested
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << url.toString();
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << url.toString();
        }
        reply->abort();
    }

    // We must clear out cached authentication and connections or
    // GFE will puke next time
    m_Nam.clearAccessCache();

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            // This will trigger falling back to HTTP for the serverinfo query
            // then pairing again to get the updated certificate.
            GfeHttpResponseException exception(401, "Server certificate mismatch");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QtNetworkReplyException exception(QNetworkReply::TimeoutError, "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            QtNetworkReplyException exception(reply->error(), reply->errorString());
            delete reply;
            throw exception;
        }
    }

    return reply;
}
