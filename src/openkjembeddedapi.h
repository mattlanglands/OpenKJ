#ifndef OPENKJEMBEDDEDAPI_H
#define OPENKJEMBEDDEDAPI_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QJsonObject>
#include "models/tablemodelrotation.h"
#include "models/tablemodelqueuesongs.h"
#include "settings.h"

class OpenKJEmbeddedApi : public QObject
{
    Q_OBJECT

public:
    explicit OpenKJEmbeddedApi(TableModelRotation &rotationModel,
                               TableModelQueueSongs &queueModel,
                               Settings &settings,
                               QObject *parent = nullptr);

    bool start(quint16 port = 5050);
    void stop();

private:
    struct HttpRequest
    {
        QString method;
        QString path;
        QHash<QString, QString> headers;
        QByteArray body;
    };

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    TableModelRotation &m_rotationModel;
    TableModelQueueSongs &m_queueModel;
    Settings &m_settings;

    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

    bool tryParseHttpRequest(QByteArray &buffer, HttpRequest &request);
    QByteArray handleRequest(const HttpRequest &request);
    QByteArray handleApiCommand(const QJsonObject &payload);

    QByteArray jsonResponse(int statusCode, const QJsonObject &object) const;
    static QByteArray statusText(int code);
    static QHash<QString, QString> parseHeaders(const QList<QByteArray> &headerLines);

    int nextSerial();
    bool isAccepting() const;
    void setAccepting(bool accepting);

    QJsonObject commandSearch(const QJsonObject &payload);
    QJsonObject commandSubmitRequest(const QJsonObject &payload);
    QJsonObject commandGetRequests();
    QJsonObject commandDeleteRequest(const QJsonObject &payload);
    QJsonObject commandSetAccepting(const QJsonObject &payload);

    bool removeQueueSongById(int qsongId);
    void normalizeSingerQueuePositions(int singerId);
};

#endif
