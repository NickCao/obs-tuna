/*************************************************************************
 * This file is part of tuna
 * git.vrsal.xyz/alex/tuna
 * Copyright 2023 univrsal <uni@vrsal.xyz>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#define NOMINMAX
#include "spotify_source.hpp"
#include "../gui/tuna_gui.hpp"
#include "../gui/widgets/spotify.hpp"
#include "../util/config.hpp"
#include "../util/constants.hpp"
#if !defined(SPOTIFY_CREDENTIALS)
#    include "../util/creds.hpp"
#endif
#include "../util/utility.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <curl/curl.h>
#include <util/config-file.h>
#include <util/platform.h>

#define TOKEN_URL "https://accounts.spotify.com/api/token"
#define PLAYER_URL "https://api.spotify.com/v1/me/player"
#define PLAYER_PAUSE_URL (PLAYER_URL "/pause")
#define PLAYER_PLAY_URL (PLAYER_URL "/play")
#define PLAYER_NEXT_URL (PLAYER_URL "/next")
#define PLAYER_PREVIOUS_URL (PLAYER_URL "/previous")
#define PLAYER_VOLUME_URL (PLAYER_URL "/volume")
#define CURL_DEBUG 0L
#define REDIRECT_URI "https%3A%2F%2Funivrsal.github.io%2Fauth%2Ftoken"

spotify_source::spotify_source()
    : music_source(S_SOURCE_SPOTIFY, T_SOURCE_SPOTIFY, new spotify)
{
    build_credentials();
    m_capabilities = CAP_NEXT_SONG | CAP_PREV_SONG | CAP_PLAY_PAUSE | CAP_VOLUME_MUTE | CAP_PREV_SONG;
    supported_metadata({ meta::TITLE, meta::ARTIST, meta::ALBUM, meta::RELEASE, meta::COVER, meta::DURATION, meta::PROGRESS, meta::STATUS, meta::URL, meta::CONTEXT_URL, meta::PLAYLIST_NAME });
}

bool spotify_source::enabled() const
{
    return true;
}

void spotify_source::build_credentials()
{
    auto client_id = utf8_to_qt(CGET_STR(CFG_SPOTIFY_CLIENT_ID));
    auto client_secret = utf8_to_qt(CGET_STR(CFG_SPOTIFY_CLIENT_SECRET));

    if (!client_id.isEmpty() && !client_secret.isEmpty()) {
        m_creds = (client_id + ":" + client_secret).toUtf8().toBase64();
    } else {
        QString str = utf8_to_qt(SPOTIFY_CREDENTIALS);
        auto utf8 = str.toUtf8();
        m_creds = utf8.toBase64();
    }
}

void spotify_source::load()
{
    CDEF_BOOL(CFG_SPOTIFY_LOGGEDIN, false);
    CDEF_STR(CFG_SPOTIFY_TOKEN, "");
    CDEF_STR(CFG_SPOTIFY_AUTH_CODE, "");
    CDEF_STR(CFG_SPOTIFY_REFRESH_TOKEN, "");
    CDEF_INT(CFG_SPOTIFY_TOKEN_TERMINATION, 0);
    CDEF_STR(CFG_SPOTIFY_CLIENT_ID, "");
    CDEF_STR(CFG_SPOTIFY_CLIENT_SECRET, "");
    CDEF_INT(CFG_SPOTIFY_REQUEST_TIMEOUT, 1000);

    m_logged_in = CGET_BOOL(CFG_SPOTIFY_LOGGEDIN);
    m_token = utf8_to_qt(CGET_STR(CFG_SPOTIFY_TOKEN));
    m_refresh_token = utf8_to_qt(CGET_STR(CFG_SPOTIFY_REFRESH_TOKEN));
    m_auth_code = utf8_to_qt(CGET_STR(CFG_SPOTIFY_AUTH_CODE));
    m_token_termination = CGET_INT(CFG_SPOTIFY_TOKEN_TERMINATION);
    m_curl_timeout_ms = CGET_INT(CFG_SPOTIFY_REQUEST_TIMEOUT);

    build_credentials();
    music_source::load();

    /* Token handling */
    if (m_logged_in) {
        if (util::epoch() > m_token_termination) {
            binfo("Refreshing Spotify token");
            QString log;
            const auto result = do_refresh_token(log);
            if (result)
                binfo("Successfully renewed Spotify token");
            save();
            music_source::load(); // Reload token stuff etc.
        }
    }
}

/* implementation further down */
long execute_command(const char* auth_token, const char* url, std::string& response_header,
    QJsonDocument& response_json, int64_t curl_timeout, const char* custom_request_type = nullptr, const char* request_data = nullptr);

void extract_timeout(const std::string& header, uint64_t& timeout)
{
    static const std::string what = "Retry-After: ";
    timeout = 0;
    auto pos = header.find(what);

    if (pos != std::string::npos) {
        pos += what.length();
        auto end = pos;
        while (header.at(end) != '\n')
            end++;
        const auto tmp = header.substr(pos, end - pos);
        timeout = std::stoi(tmp);
    }
}

void spotify_source::refresh()
{
    if (!m_logged_in)
        return;

    begin_refresh();
    bdebug("[Spotify] begin refresh");

    if (util::epoch() > m_token_termination) {
        binfo("Refreshing Spotify token");
        QString log;
        do_refresh_token(log);
        //        emit(get_ui<spotify>())->login_state_changed(result, log);
        save();
    }

    if (m_timout_start) {
        if (os_gettime_ns() - m_timout_start >= m_timeout_length) {
            m_timout_start = 0;
            m_timeout_length = 0;
            binfo("API timeout of %i seconds is over", int(m_timeout_length));
        } else {
            bdebug("Waiting for Spotify-API timeout");
            return;
        }
    }

    std::string header = "";
    QJsonDocument response;
    QJsonObject obj;

    const auto http_code = execute_command(qt_to_utf8(m_token), PLAYER_URL, header, response, m_curl_timeout_ms);
    bdebug("Executed %s command", PLAYER_URL);
    if (response.isObject())
        obj = response.object();

    if (http_code == HTTP_OK) {
        const auto& progress = obj["progress_ms"];
        const auto& device = obj["device"];
        const auto& playing = obj["is_playing"];
        const auto& play_type = obj["currently_playing_type"];

        /* If an ad is playing we assume playback is paused */
        if (play_type.isString() && play_type.toString() == "ad") {
            m_current.set(meta::STATUS, state_paused);
            return;
        }

        if (device.isObject() && playing.isBool()) {
            if (device.toObject()["is_private"].toBool()) {
                berr("Spotify session is private! Can't read track");
            } else {
                parse_track_json(obj);
                m_current.set(meta::STATUS, playing.toBool() ? state_playing : state_stopped);
            }
            m_current.set(meta::PROGRESS, progress.toInt());
        } else {
            QString str(response.toJson());
            berr("Couldn't fetch song data from spotify json: %s", str.toStdString().c_str());
        }
        m_last_state = m_current.get<int>(meta::STATUS);
    } else if (http_code == HTTP_NO_CONTENT) {
        /* No session running */
        m_current.clear();
    } else {
        /* Don't reset cover or info here since
         * we're just waiting for the API to give a proper
         * response again
         */
        if (http_code == STATUS_RETRY_AFTER && !header.empty()) {
            extract_timeout(header, m_timeout_length);
            if (m_timeout_length) {
                bwarn("Spotify-API Rate limit hit, waiting %i seconds\n", int(m_timeout_length));
                m_timeout_length *= SECOND_TO_NS;
                m_timout_start = os_gettime_ns();
            }
        }
    }
    bdebug("[Spotify] Finished refresh");
}

void spotify_source::parse_track_json(const QJsonValue& response)
{
    const auto& trackObj = response["item"].toObject();
    const auto& album = trackObj["album"].toObject();
    const auto& artists = trackObj["artists"].toArray();
    const auto& urls = trackObj["external_urls"].toObject();

    m_current.clear();

    if (response["context"].isObject()) {
        const auto& context = response["context"].toObject();

        QJsonDocument doc;
        doc.setObject(context);
        m_current.set(meta::CONTEXT, context["type"].toString());
        m_current.set(meta::CONTEXT_URL, context["uri"].toString());
        if (context["external_urls"].isObject())
            m_current.set(meta::CONTEXT_EXTERNAL_URL, context["external_urls"].toObject()["spotify"].toString());

        if (context["href"].isString()) {
            QJsonDocument playlist_response;
            QJsonObject obj;
            std::string header = "";
            const auto& url = context["href"].toString();
            const auto http_code = execute_command(qt_to_utf8(m_token), qt_to_utf8(url), header, playlist_response, m_curl_timeout_ms);

            if (playlist_response.isObject())
                obj = playlist_response.object();

            if (http_code == HTTP_OK) {
                m_current.set(meta::PLAYLIST_NAME, playlist_response["name"].toString());
            }
        }
    }

    QStringList tmp;
    /* Get All artists */
    for (const auto& artist : std::as_const(artists))
        tmp.append(artist.toObject()["name"].toString());
    m_current.set(meta::ARTIST, tmp);

    /* Cover link */
    const auto& covers = album["images"];
    if (covers.isArray() && !covers.toArray().isEmpty()) {
        const QJsonValue v = covers.toArray()[0];
        if (v.isObject() && v.toObject().contains("url"))
            m_current.set(meta::COVER, v.toObject()["url"].toString());
    }

    /* Song link */
    if (!urls.isEmpty() && urls["spotify"].isString()) {
        m_current.set(meta::URL, urls["spotify"].toString());
    }

    /* Other stuff */
    m_current.set(meta::TITLE, trackObj["name"].toString());
    m_current.set(meta::DURATION, trackObj["duration_ms"].toInt());
    m_current.set(meta::ALBUM, album["name"].toString());
    m_current.set(meta::EXPLICIT, trackObj["explicit"].toBool());
    m_current.set(meta::DISC_NUMBER, trackObj["disc_number"].toInt());
    m_current.set(meta::TRACK_NUMBER, trackObj["track_number"].toInt());

    /* Release date */
    const auto& date = album["release_date"].toString();
    if (date.length() > 0) {
        QStringList list = date.split("-");
        switch (list.length()) {
        case 3:
            m_current.set(meta::RELEASE_DAY, list[2].toInt());
            [[fallthrough]];
        case 2:
            m_current.set(meta::RELEASE_MONTH, list[1].toInt());
            [[fallthrough]];
        case 1:
            m_current.set(meta::RELEASE_YEAR, list[0].toInt());
        }
    }
}

bool spotify_source::execute_capability(capability c)
{
    QString const token = qt_to_utf8(m_token);
    auto const playing = m_current.get<int>(meta::STATUS);
    auto timeout = m_curl_timeout_ms;
    // offload this into a separate thread because the request
    // can take up to one second
    std::thread([timeout, token, playing, c] {
        std::string header;
        long http_code = -1;
        QJsonDocument response;

        switch (c) {
        case CAP_PLAY_PAUSE:
            if (playing) {
            case CAP_STOP_SONG:
                http_code = execute_command(qt_to_utf8(token), PLAYER_PAUSE_URL, header, response, timeout, "PUT");
            } else {
                http_code = execute_command(qt_to_utf8(token), PLAYER_PLAY_URL, header, response, timeout, "PUT", "{\"position_ms\": 0}");
            }
            break;
        case CAP_PREV_SONG:
            http_code = execute_command(qt_to_utf8(token), PLAYER_PREVIOUS_URL, header, response, timeout, "POST");
            break;
        case CAP_NEXT_SONG:
            http_code = execute_command(qt_to_utf8(token), PLAYER_NEXT_URL, header, response, timeout, "POST");
            break;
        case CAP_VOLUME_UP:
            /* TODO? */
            break;
        case CAP_VOLUME_DOWN:
            /* TODO? */
            break;
        default:;
        }

        /* Parse response */
        if (http_code != HTTP_NO_CONTENT) {
            QString r(response.toJson());
            binfo("Couldn't run spotify command! HTTP code: %i", int(http_code));
            binfo("Spotify controls only work for premium users!");
            binfo("Response: %s", qt_to_utf8(r));
        }
    }).detach();

    // Ideally we would check if the http request succeeded, but we can't wait here
    // otherwise the UI stalls
    return true;
}

/* === CURL/Spotify API handling === */

size_t header_callback(char* ptr, size_t size, size_t nmemb, std::string* str)
{
    size_t new_length = size * nmemb;
    try {
        str->append(ptr, new_length);
    } catch (std::bad_alloc& e) {
        berr("Error reading curl header: %s", e.what());
        return 0;
    }
    return new_length;
}

CURL* prepare_curl(struct curl_slist* header, std::string* response, std::string* response_header,
    const std::string& request, int64_t timeout)
{
    CURL* curl = curl_easy_init();

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, TOKEN_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(request.c_str()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, util::write_callback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response_header);
#ifdef DEBUG
    curl_easy_setopt(curl, CURLOPT_VERBOSE, CURL_DEBUG);
#endif
    return curl;
}

/* Requests an access token via request body
 * over a POST request to spotify */
void request_token(const std::string& request, const std::string& credentials, QJsonDocument& response_json, int64_t timeout)
{
    if (request.empty() || credentials.empty()) {
        berr("Cannot request token without valid credentials"
             " and/or auth code!");
        return;
    }

    std::string response, response_header;
    std::string header = "Authorization: Basic ";
    header.append(credentials);

    auto* list = curl_slist_append(nullptr, header.c_str());
    CURL* curl = prepare_curl(list, &response, &response_header, request, timeout);
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        QJsonParseError err;
        response_json = QJsonDocument::fromJson(response.c_str(), &err);
        if (response_json.isNull()) {
            berr("Couldn't parse response to json: %s", err.errorString().toStdString().c_str());
        } else {
            /* Log response without tokens */
            auto obj = response_json.object();
            if (obj["access_token"].isString())
                obj["access_token"] = "REDACTED";
            if (obj["refresh_token"].isString())
                obj["refresh_token"] = "REDACTED";
            auto doc = QJsonDocument(obj);
            QString str(doc.toJson());
            binfo("Spotify response: %s", qt_to_utf8(str));
        }
    } else {
        berr("Curl returned error code (%i) %s", res, curl_easy_strerror(res));
    }

    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
}

/* Gets a new token using the refresh token */
bool spotify_source::do_refresh_token(QString& log)
{
    build_credentials();
    static std::string request;
    bool result = false;
    QJsonDocument response;

    if (m_refresh_token.isEmpty()) {
        berr("Refresh token is empty!");
    }

    request = "grant_type=refresh_token&refresh_token=";
    request.append(m_refresh_token.toStdString());
    request_token(request, m_creds.toStdString(), response, m_curl_timeout_ms);

    if (response.isNull()) {
        berr("Couldn't refresh Spotify token, response was null");
    } else {
        const auto& response_obj = response.object();
        const auto& token = response_obj["access_token"];
        const auto& expires = response_obj["expires_in"];
        const auto& error = response_obj["error"];
        const auto& refresh_token = response_obj["refresh_token"];

        /* Dump the json into the log text */
        log = QString(response.toJson(QJsonDocument::Indented));
        if (token.isString() && expires.isDouble()) {
            m_token = token.toString();
            m_token_termination = util::epoch() + expires.toInt();
            result = true;
            binfo("Successfully logged in");
        } else {
            if (error.isString())
                berr("Received error from spotify: %s", qt_to_utf8(error.toString()));
            else
                berr("Couldn't parse json response");
        }

        /* Refreshing the token can return a new refresh token */
        if (refresh_token.isString()) {
            QString tmp = refresh_token.toString();
            if (!tmp.isEmpty()) {
                binfo("Received a new fresh token");
                m_refresh_token = refresh_token.toString();
            }
        }
    }

    m_logged_in = result;
    save();
    return result;
}

/* Gets the first token from the access code */
bool spotify_source::new_token(QString& log)
{
    build_credentials();
    static std::string request;
    bool result = false;
    QJsonDocument response;
    request = "grant_type=authorization_code&code=";
    request.append(m_auth_code.toStdString());
    request.append("&redirect_uri=").append(REDIRECT_URI);
    request_token(request, m_creds.toStdString(), response, m_curl_timeout_ms);

    if (response.isObject()) {
        const auto& response_obj = response.object();
        const auto& token = response_obj["access_token"];
        const auto& refresh = response_obj["refresh_token"];
        const auto& expires = response_obj["expires_in"];

        /* Dump the json into the log textbox */
        log = QString(response.toJson(QJsonDocument::Indented));

        if (token.isString() && refresh.isString() && expires.isDouble()) {
            m_token = token.toString();
            m_refresh_token = refresh.toString();
            m_token_termination = util::epoch() + expires.toInt();
            result = true;
        } else {
            berr("Couldn't parse json response!");
        }
    } else {
    }

    m_logged_in = result;
    save();
    return result;
}

/* Sends commands to spotify api via url */

long execute_command(const char* auth_token, const char* url, std::string& response_header,
    QJsonDocument& response_json, int64_t curl_timeout, const char* custom_request_type, const char* request_data)
{
    static int timeout_start = 0;
    static int timeout = 0;
    static int timeout_multiplier = 1;

    if (timeout > 0) {
        if (util::epoch() - timeout_start >= timeout) {
            binfo("cURL request timeout over.");
            timeout = 0;
        } else {
            return 0; // Waiting for timeout to be over
        }
    }

    long http_code = -1;
    std::string response;

    std::string header = "Authorization: Bearer ";
    header.append(auth_token);

    auto* list = curl_slist_append(nullptr, header.c_str());

    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, util::write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, curl_timeout);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_header);

    if (custom_request_type != nullptr) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, custom_request_type);
        if (request_data)
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_data);
        else
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    }

    if (!response_header.empty())
        bdebug("Response header: %s", response_header.c_str());

#ifdef DEBUG
    curl_easy_setopt(curl, CURLOPT_VERBOSE, CURL_DEBUG);
#endif
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        QJsonParseError err;

        response_json = QJsonDocument::fromJson(response.c_str(), &err);
        if (response_json.isNull() && !response.empty()) {
            berr("Failed to parse json response: %s, Error: %s", response.c_str(), qt_to_utf8(err.errorString()));
        } else {
            timeout_multiplier = 1; // Reset on successful requests
            timeout_start = 0;
            timeout = 0;
        }
    } else {
        timeout_start = util::epoch();
        timeout = 5 * timeout_multiplier++;
        berr(
            "cURL failed while sending spotify command (HTTP error %i, cURL error %i: '%s'). Waiting %i seconds before trying again",
            int(http_code), res, curl_easy_strerror(res), timeout);
    }

    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return http_code;
}
