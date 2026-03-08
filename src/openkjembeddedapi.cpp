#include "openkjembeddedapi.h"

#include <algorithm>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

OpenKJEmbeddedApi::OpenKJEmbeddedApi(TableModelRotation &rotationModel,
                                     TableModelQueueSongs &queueModel,
                                     Settings &settings,
                                     QObject *parent)
    : QObject(parent),
      m_rotationModel(rotationModel),
      m_queueModel(queueModel),
      m_settings(settings)
{
    connect(&m_server, &QTcpServer::newConnection, this, &OpenKJEmbeddedApi::onNewConnection);
}

bool OpenKJEmbeddedApi::start(const quint16 port)
{
    if (m_server.isListening()) {
        return true;
    }

    return m_server.listen(QHostAddress::Any, port);
}

void OpenKJEmbeddedApi::stop()
{
    if (!m_server.isListening()) {
        return;
    }

    for (QTcpSocket *socket : m_buffers.keys()) {
        socket->disconnectFromHost();
    }

    m_server.close();
}

void OpenKJEmbeddedApi::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, &OpenKJEmbeddedApi::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &OpenKJEmbeddedApi::onSocketDisconnected);
    }
}

void OpenKJEmbeddedApi::onSocketReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket || !m_buffers.contains(socket)) {
        return;
    }

    auto &buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    HttpRequest request;
    if (!tryParseHttpRequest(buffer, request)) {
        return;
    }

    const QByteArray response = handleRequest(request);
    socket->write(response);
    socket->disconnectFromHost();
}

void OpenKJEmbeddedApi::onSocketDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) {
        return;
    }

    m_buffers.remove(socket);
    socket->deleteLater();
}

bool OpenKJEmbeddedApi::tryParseHttpRequest(QByteArray &buffer, HttpRequest &request)
{
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return false;
    }

    const QByteArray headerSection = buffer.left(headerEnd);
    const QList<QByteArray> headerLines = headerSection.split('\n');
    if (headerLines.isEmpty()) {
        return false;
    }

    const QList<QByteArray> firstLineParts = headerLines.first().trimmed().split(' ');
    if (firstLineParts.size() < 2) {
        return false;
    }

    request.method = QString::fromUtf8(firstLineParts.at(0)).trimmed().toUpper();
    request.path = QString::fromUtf8(firstLineParts.at(1)).trimmed();
    request.headers = parseHeaders(headerLines.mid(1));

    const int contentLength = request.headers.value("content-length", "0").toInt();
    const int totalNeeded = headerEnd + 4 + contentLength;
    if (buffer.size() < totalNeeded) {
        return false;
    }

    request.body = buffer.mid(headerEnd + 4, contentLength);
    buffer.remove(0, totalNeeded);
    return true;
}

QByteArray OpenKJEmbeddedApi::handleRequest(const HttpRequest &request)
{
    if (request.method == "GET" && request.path == "/health") {
        QJsonObject out;
        out.insert("status", "ok");
        return jsonResponse(200, out);
    }

    if (request.method == "GET" && request.path == "/stats") {
        QSqlQuery query;
        query.exec("SELECT COUNT(1) FROM dbsongs WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!'");
        int songCount = 0;
        if (query.next()) {
            songCount = query.value(0).toInt();
        }

        QSqlQuery queueQuery;
        queueQuery.exec("SELECT COUNT(1) FROM queuesongs WHERE played = 0");
        int queueCount = 0;
        if (queueQuery.next()) {
            queueCount = queueQuery.value(0).toInt();
        }

        QJsonObject out;
        out.insert("status", "ok");
        out.insert("song_count", songCount);
        out.insert("request_count", queueCount);
        out.insert("accepting", isAccepting());
        out.insert("serial", m_settings.embeddedApiSerial());
        return jsonResponse(200, out);
    }

    if (request.method == "POST" && request.path == "/api.php") {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject out;
            out.insert("error", "true");
            out.insert("errorString", "Invalid JSON");
            return jsonResponse(400, out);
        }

        return jsonResponse(200, handleApiCommand(doc.object()));
    }

    if (request.method == "POST" && request.path == "/browse") {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject out;
            out.insert("error", "Invalid JSON");
            return jsonResponse(400, out);
        }

        const QJsonObject payload = doc.object();
        const QString by = payload.value("by").toString("artist").trimmed().toLower();
        const QString letter = payload.value("letter").toString("A").trimmed().toUpper();
        const int limit = std::clamp(payload.value("limit").toInt(200), 1, 1000);

        if ((by != "artist" && by != "title") || letter.size() != 1 || letter.at(0) < QChar('A') ||
            letter.at(0) > QChar('Z')) {
            QJsonObject out;
            out.insert("error", "Invalid browse params");
            return jsonResponse(400, out);
        }

        const QString primary = (by == "title") ? "title" : "artist";
        const QString secondary = (by == "title") ? "artist" : "title";

        QSqlQuery query;
        query.prepare(QString("SELECT songid, artist, title, COALESCE(duration, 0) FROM dbsongs "
                              "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' "
                              "AND upper(trim(%1)) LIKE :prefix "
                              "ORDER BY upper(%1), upper(%2) LIMIT :limit").arg(primary, secondary));
        query.bindValue(":prefix", letter + "%");
        query.bindValue(":limit", limit);

        QJsonArray songs;
        if (query.exec()) {
            while (query.next()) {
                QJsonObject song;
                song.insert("song_id", query.value(0).toInt());
                song.insert("artist", query.value(1).toString());
                song.insert("title", query.value(2).toString());
                song.insert("duration_seconds", std::max(1, query.value(3).toInt() / 1000));
                songs.append(song);
            }
        }

        QJsonObject out;
        out.insert("songs", songs);
        return jsonResponse(200, out);
    }

    QJsonObject out;
    out.insert("error", "true");
    out.insert("errorString", "Not Found");
    return jsonResponse(404, out);
}

QByteArray OpenKJEmbeddedApi::handleApiCommand(const QJsonObject &payload)
{
    const QString command = payload.value("command").toString();
    if (command.isEmpty()) {
        QJsonObject out;
        out.insert("error", "true");
        out.insert("errorString", "Missing command");
        return out;
    }

    if (command == "connectionTest") {
        QJsonObject out;
        out.insert("command", command);
        out.insert("connection", "ok");
        return out;
    }

    if (command == "getSerial") {
        QJsonObject out;
        out.insert("command", command);
        out.insert("serial", m_settings.embeddedApiSerial());
        out.insert("error", "false");
        return out;
    }

    if (command == "venueAccepting" || command == "getAccepting") {
        QJsonObject out;
        out.insert("command", command);
        out.insert("accepting", isAccepting());
        out.insert("venue_id", 0);
        return out;
    }

    if (command == "setAccepting") {
        return commandSetAccepting(payload);
    }

    if (command == "adminAction") {
        return commandAdminAction(payload);
    }

    if (command == "getVenues") {
        QJsonObject venue;
        venue.insert("venue_id", 0);
        venue.insert("accepting", isAccepting());
        venue.insert("name", "OpenKJ");
        venue.insert("url_name", "none");

        QJsonArray venues;
        venues.append(venue);

        QJsonObject out;
        out.insert("command", command);
        out.insert("error", "false");
        out.insert("venues", venues);
        return out;
    }

    if (command == "search") {
        return commandSearch(payload);
    }

    if (command == "submitRequest") {
        return commandSubmitRequest(payload);
    }

    if (command == "getRequests") {
        return commandGetRequests();
    }

    if (command == "deleteRequest") {
        return commandDeleteRequest(payload);
    }

    if (command == "clearRequests") {
        QSqlQuery query;
        query.exec("DELETE FROM queuesongs WHERE played = 0");
        nextSerial();

        QJsonObject out;
        out.insert("command", command);
        out.insert("error", "false");
        out.insert("serial", m_settings.embeddedApiSerial());
        return out;
    }

    if (command == "addSongs" || command == "clearDatabase") {
        // Not needed when OpenKJ serves its own catalog. Keep compatibility with success response.
        QJsonObject out;
        out.insert("command", command);
        out.insert("error", "false");
        out.insert("serial", m_settings.embeddedApiSerial());
        return out;
    }

    QJsonObject out;
    out.insert("command", command);
    out.insert("error", "true");
    out.insert("errorString", QString("Unsupported command: %1").arg(command));
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandSearch(const QJsonObject &payload)
{
    const QString searchString = payload.value("searchString").toString().trimmed();

    QStringList terms = searchString.split(' ', Qt::SkipEmptyParts);
    QSqlQuery query;

    QString sql = "SELECT songid, artist, title, COALESCE(duration, 0) FROM dbsongs "
                  "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' ";
    if (!terms.isEmpty()) {
        for (int i = 0; i < terms.size(); ++i) {
            sql += QString("AND lower(artist || ' ' || title) LIKE :term%1 ").arg(i);
        }
    }
    sql += "ORDER BY upper(artist), upper(title) LIMIT 100";

    query.prepare(sql);
    for (int i = 0; i < terms.size(); ++i) {
        query.bindValue(QString(":term%1").arg(i), QString("%%1%").arg(terms.at(i).toLower()));
    }

    QJsonArray songs;
    if (query.exec()) {
        while (query.next()) {
            const QString artist = query.value(1).toString();
            const QString title = query.value(2).toString();
            const QString combined = (artist + " " + title).toLower();
            if (combined.contains("wvocal") || combined.contains("w-vocal") || combined.contains("vocals")) {
                continue;
            }

            QJsonObject song;
            song.insert("song_id", query.value(0).toInt());
            song.insert("artist", artist);
            song.insert("title", title);
            song.insert("duration_seconds", std::max(1, query.value(3).toInt() / 1000));
            songs.append(song);
        }
    }

    QJsonObject out;
    out.insert("command", "search");
    out.insert("songs", songs);
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandSubmitRequest(const QJsonObject &payload)
{
    if (!isAccepting()) {
        QJsonObject out;
        out.insert("command", "submitRequest");
        out.insert("error", "true");
        out.insert("errorString", "Requests not accepted");
        return out;
    }

    bool ok = false;
    int songId = payload.value("songId").toVariant().toInt(&ok);
    if (!ok && payload.value("songId").isString()) {
        songId = payload.value("songId").toString().toInt(&ok);
    }
    const QString singerName = payload.value("singerName").toString().trimmed();

    if (!ok || songId < 1 || singerName.isEmpty()) {
        QJsonObject out;
        out.insert("command", "submitRequest");
        out.insert("error", "true");
        out.insert("errorString", "Missing songId or singerName");
        return out;
    }

    int singerId = -1;
    if (m_rotationModel.singerExists(singerName)) {
        singerId = m_rotationModel.getSingerByName(singerName).id;
    } else {
        singerId = m_rotationModel.singerAdd(singerName, m_settings.lastSingerAddPositionType());
    }

    m_queueModel.songAddSlot(songId, singerId, 0);
    nextSerial();

    QJsonObject out;
    out.insert("command", "submitRequest");
    out.insert("error", "false");
    out.insert("success", true);
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandGetRequests()
{
    const int nowSingerId = currentSingerId();

    QJsonObject nowPlaying;
    if (nowSingerId >= 0) {
        QSqlQuery nowQuery;
        nowQuery.prepare(
            "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0) "
            "FROM queuesongs qs "
            "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
            "INNER JOIN dbsongs d ON d.songid = qs.song "
            "WHERE qs.singer = :singerId "
            "ORDER BY qs.played ASC, qs.position ASC LIMIT 1");
        nowQuery.bindValue(":singerId", nowSingerId);
        if (nowQuery.exec() && nowQuery.next()) {
            nowPlaying.insert("request_id", nowQuery.value(0).toInt());
            nowPlaying.insert("singer", nowQuery.value(1).toString());
            nowPlaying.insert("song_id", nowQuery.value(2).toInt());
            nowPlaying.insert("artist", nowQuery.value(3).toString());
            nowPlaying.insert("title", nowQuery.value(4).toString());
            nowPlaying.insert("duration_seconds", std::max(1, nowQuery.value(5).toInt() / 1000));
            nowPlaying.insert("request_time", QDateTime::currentSecsSinceEpoch());
        }
    }

    QSqlQuery query;
    query.prepare(
        "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0), qs.keychg "
        "FROM queuesongs qs "
        "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
        "INNER JOIN dbsongs d ON d.songid = qs.song "
        "WHERE qs.played = 0 AND qs.singer != :nowSinger "
        "ORDER BY rs.position ASC, qs.position ASC");
    query.bindValue(":nowSinger", nowSingerId);

    QJsonArray requests;
    QJsonArray upNext;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject request;
            request.insert("request_id", query.value(0).toInt());
            request.insert("singer", query.value(1).toString());
            request.insert("song_id", query.value(2).toInt());
            request.insert("artist", query.value(3).toString());
            request.insert("title", query.value(4).toString());
            request.insert("duration_seconds", std::max(1, query.value(5).toInt() / 1000));
            request.insert("key_change", query.value(6).toInt());
            request.insert("request_time", QDateTime::currentSecsSinceEpoch());
            requests.append(request);
            upNext.append(request);
        }
    }

    QJsonArray recentlyPlayed;
    if (nowSingerId >= 0) {
        QSqlQuery playedQuery;
        playedQuery.prepare(
            "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0) "
            "FROM queuesongs qs "
            "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
            "INNER JOIN dbsongs d ON d.songid = qs.song "
            "WHERE qs.played = 1 "
            "ORDER BY qs.qsongid DESC LIMIT 30");
        if (playedQuery.exec()) {
            while (playedQuery.next()) {
                QJsonObject item;
                item.insert("request_id", playedQuery.value(0).toInt());
                item.insert("singer", playedQuery.value(1).toString());
                item.insert("song_id", playedQuery.value(2).toInt());
                item.insert("artist", playedQuery.value(3).toString());
                item.insert("title", playedQuery.value(4).toString());
                item.insert("duration_seconds", std::max(1, playedQuery.value(5).toInt() / 1000));
                item.insert("played_at", QDateTime::currentSecsSinceEpoch());
                recentlyPlayed.append(item);
            }
        }
    }

    QJsonObject out;
    out.insert("command", "getRequests");
    out.insert("error", "false");
    out.insert("serial", m_settings.embeddedApiSerial());
    out.insert("requests", requests);
    out.insert("now_playing", nowPlaying);
    out.insert("up_next", upNext);
    out.insert("recently_played", recentlyPlayed);
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandDeleteRequest(const QJsonObject &payload)
{
    bool ok = false;
    const int requestId = payload.value("request_id").toVariant().toInt(&ok);
    if (!ok) {
        QJsonObject out;
        out.insert("command", "deleteRequest");
        out.insert("error", "true");
        out.insert("errorString", "Missing request_id");
        return out;
    }

    removeQueueSongById(requestId);
    nextSerial();

    QJsonObject out;
    out.insert("command", "deleteRequest");
    out.insert("error", "false");
    out.insert("serial", m_settings.embeddedApiSerial());
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandSetAccepting(const QJsonObject &payload)
{
    bool accepting = payload.value("accepting").toBool(true);
    if (payload.value("accepting").isDouble()) {
        accepting = payload.value("accepting").toInt() != 0;
    }

    setAccepting(accepting);
    nextSerial();

    QJsonObject out;
    out.insert("command", "setAccepting");
    out.insert("error", "false");
    out.insert("venue_id", 0);
    out.insert("accepting", accepting);
    out.insert("serial", m_settings.embeddedApiSerial());
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandAdminAction(const QJsonObject &payload)
{
    const QString action = payload.value("action").toString().trimmed();
    if (action.isEmpty()) {
        return QJsonObject{{"command", "adminAction"}, {"error", "true"}, {"errorString", "Missing action"}};
    }

    bool success = false;
    if (action == "remove_song") {
        success = removeQueueSongById(payload.value("request_id").toVariant().toInt());
    } else if (action == "move_song_up") {
        success = moveQueueSongByOffset(payload.value("request_id").toVariant().toInt(), -1);
    } else if (action == "move_song_down") {
        success = moveQueueSongByOffset(payload.value("request_id").toVariant().toInt(), 1);
    } else if (action == "set_key_change") {
        int requestId = payload.value("request_id").toVariant().toInt();
        if (requestId <= 0) {
            QSqlQuery query;
            query.prepare("SELECT qsongid FROM queuesongs WHERE singer = :singer AND played = 0 ORDER BY position LIMIT 1");
            query.bindValue(":singer", currentSingerId());
            if (query.exec() && query.next()) {
                requestId = query.value(0).toInt();
            }
        }
        success = requestId > 0 && setQueueSongKey(requestId, payload.value("value").toInt());
    } else if (action == "skip_song") {
        const int singerId = currentSingerId();
        success = false;
        if (singerId >= 0) {
            QSqlQuery query;
            query.prepare("SELECT qsongid FROM queuesongs WHERE singer = :singer AND played = 0 ORDER BY position LIMIT 1");
            query.bindValue(":singer", singerId);
            if (query.exec() && query.next()) {
                QSqlQuery upd;
                upd.prepare("UPDATE queuesongs SET played = 1 WHERE qsongid = :id");
                upd.bindValue(":id", query.value(0).toInt());
                success = upd.exec();
            }
        }
    } else if (action == "rotation_next") {
        const int nextId = nextSingerId(1);
        if (nextId >= 0) {
            m_settings.setCurrentRotationPosition(nextId);
            success = true;
        }
    } else if (action == "rotation_previous") {
        const int prevId = nextSingerId(-1);
        if (prevId >= 0) {
            m_settings.setCurrentRotationPosition(prevId);
            success = true;
        }
    } else if (action == "set_volume") {
        m_settings.setAudioVolume(std::clamp(payload.value("value").toInt(60), 0, 100));
        success = true;
    } else if (action == "lock_queue") {
        setAccepting(false);
        success = true;
    } else if (action == "unlock_queue") {
        setAccepting(true);
        success = true;
    }

    if (!success) {
        return QJsonObject{
            {"command", "adminAction"},
            {"error", "true"},
            {"errorString", QString("Action failed or unsupported: %1").arg(action)}
        };
    }

    nextSerial();
    return QJsonObject{{"command", "adminAction"}, {"error", "false"}, {"serial", m_settings.embeddedApiSerial()}};
}

QByteArray OpenKJEmbeddedApi::jsonResponse(const int statusCode, const QJsonObject &object) const
{
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray response;
    response.append("HTTP/1.1 ");
    response.append(QByteArray::number(statusCode));
    response.append(" ");
    response.append(statusText(statusCode));
    response.append("\r\n");
    response.append("Content-Type: application/json; charset=utf-8\r\n");
    response.append("Content-Length: ");
    response.append(QByteArray::number(body.size()));
    response.append("\r\n");
    response.append("Connection: close\r\n\r\n");
    response.append(body);
    return response;
}

QByteArray OpenKJEmbeddedApi::statusText(const int code)
{
    switch (code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        default:
            return "Error";
    }
}

QHash<QString, QString> OpenKJEmbeddedApi::parseHeaders(const QList<QByteArray> &headerLines)
{
    QHash<QString, QString> headers;
    for (const auto &lineRaw : headerLines) {
        const QByteArray line = lineRaw.trimmed();
        const int sep = line.indexOf(':');
        if (sep <= 0) {
            continue;
        }

        const QString key = QString::fromUtf8(line.left(sep)).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(sep + 1)).trimmed();
        headers.insert(key, value);
    }

    return headers;
}

int OpenKJEmbeddedApi::nextSerial()
{
    int serial = QRandomGenerator::global()->bounded(100000);
    if (serial == m_settings.embeddedApiSerial()) {
        serial = (serial + 1) % 100000;
    }
    m_settings.setEmbeddedApiSerial(serial);
    return serial;
}

bool OpenKJEmbeddedApi::isAccepting() const
{
    return m_settings.embeddedApiAccepting();
}

void OpenKJEmbeddedApi::setAccepting(const bool accepting)
{
    m_settings.setEmbeddedApiAccepting(accepting);
}

bool OpenKJEmbeddedApi::removeQueueSongById(const int qsongId)
{
    QSqlQuery find;
    find.prepare("SELECT singer FROM queuesongs WHERE qsongid = :id LIMIT 1");
    find.bindValue(":id", qsongId);
    int singerId = -1;
    if (find.exec() && find.next()) {
        singerId = find.value(0).toInt();
    }

    QSqlQuery del;
    del.prepare("DELETE FROM queuesongs WHERE qsongid = :id");
    del.bindValue(":id", qsongId);
    const bool removed = del.exec();

    if (removed && singerId >= 0) {
        normalizeSingerQueuePositions(singerId);
        if (m_queueModel.getSingerId() == singerId) {
            m_queueModel.loadSinger(singerId);
        }
    }

    return removed;
}

void OpenKJEmbeddedApi::normalizeSingerQueuePositions(const int singerId)
{
    QSqlQuery query;
    query.prepare("SELECT qsongid FROM queuesongs WHERE singer = :singerId ORDER BY position ASC, qsongid ASC");
    query.bindValue(":singerId", singerId);
    if (!query.exec()) {
        return;
    }

    QList<int> ids;
    while (query.next()) {
        ids.append(query.value(0).toInt());
    }

    QSqlQuery tx;
    tx.exec("BEGIN TRANSACTION");
    QSqlQuery upd;
    upd.prepare("UPDATE queuesongs SET position = :position WHERE qsongid = :id");
    for (int i = 0; i < ids.size(); ++i) {
        upd.bindValue(":position", i);
        upd.bindValue(":id", ids.at(i));
        upd.exec();
    }
    tx.exec("COMMIT");
}

bool OpenKJEmbeddedApi::moveQueueSongByOffset(const int qsongId, const int offset)
{
    QSqlQuery find;
    find.prepare("SELECT singer, position FROM queuesongs WHERE qsongid = :id LIMIT 1");
    find.bindValue(":id", qsongId);
    if (!find.exec() || !find.next()) {
        return false;
    }

    const int singerId = find.value(0).toInt();
    const int oldPos = find.value(1).toInt();
    const int newPos = oldPos + offset;
    if (newPos < 0) {
        return false;
    }

    QSqlQuery countQ;
    countQ.prepare("SELECT COUNT(1) FROM queuesongs WHERE singer = :singer");
    countQ.bindValue(":singer", singerId);
    int count = 0;
    if (countQ.exec() && countQ.next()) {
        count = countQ.value(0).toInt();
    }
    if (newPos >= count) {
        return false;
    }

    QSqlQuery tx;
    tx.exec("BEGIN TRANSACTION");
    if (offset < 0) {
        QSqlQuery shift;
        shift.prepare("UPDATE queuesongs SET position = position + 1 WHERE singer = :singer AND position >= :newPos AND position < :oldPos");
        shift.bindValue(":singer", singerId);
        shift.bindValue(":newPos", newPos);
        shift.bindValue(":oldPos", oldPos);
        shift.exec();
    } else {
        QSqlQuery shift;
        shift.prepare("UPDATE queuesongs SET position = position - 1 WHERE singer = :singer AND position <= :newPos AND position > :oldPos");
        shift.bindValue(":singer", singerId);
        shift.bindValue(":newPos", newPos);
        shift.bindValue(":oldPos", oldPos);
        shift.exec();
    }

    QSqlQuery upd;
    upd.prepare("UPDATE queuesongs SET position = :newPos WHERE qsongid = :id");
    upd.bindValue(":newPos", newPos);
    upd.bindValue(":id", qsongId);
    const bool ok = upd.exec();
    tx.exec(ok ? "COMMIT" : "ROLLBACK");

    if (ok && m_queueModel.getSingerId() == singerId) {
        m_queueModel.loadSinger(singerId);
    }
    return ok;
}

bool OpenKJEmbeddedApi::setQueueSongKey(const int qsongId, const int keyChange)
{
    QSqlQuery query;
    query.prepare("UPDATE queuesongs SET keychg = :key WHERE qsongid = :id");
    query.bindValue(":key", std::clamp(keyChange, -12, 12));
    query.bindValue(":id", qsongId);
    return query.exec();
}

int OpenKJEmbeddedApi::currentSingerId() const
{
    return m_settings.currentRotationPosition();
}

int OpenKJEmbeddedApi::nextSingerId(const int direction) const
{
    QSqlQuery query;
    query.exec("SELECT singerid, position FROM rotationsingers ORDER BY position ASC");

    QList<QPair<int, int>> singers;
    while (query.next()) {
        singers.append({query.value(0).toInt(), query.value(1).toInt()});
    }
    if (singers.isEmpty()) {
        return -1;
    }

    const int curId = currentSingerId();
    int idx = 0;
    for (int i = 0; i < singers.size(); ++i) {
        if (singers.at(i).first == curId) {
            idx = i;
            break;
        }
    }

    const int nextIdx = (idx + direction + singers.size()) % singers.size();
    return singers.at(nextIdx).first;
}
