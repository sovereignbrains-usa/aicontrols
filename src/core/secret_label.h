#pragma once

#include <cctype>
#include <string>
#include <string_view>

#include "core/tools.h"

namespace aic {

// How one stored secret is shown in the secrets panel. The storage name stays what the tools look up.
// `leaked` is tied to the name: a rotated value under the same name still says «утёк» until this list changes.
struct SecretFace {
    int group = 8;
    int rank = 100;
    std::string heading = "Другое";
    std::string title;
    std::string hint;
    bool leaked = false;
};

inline bool SecretNameIs(std::string_view name, std::string_view expected) {
    if (name.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(name[i]);
        const unsigned char b = static_cast<unsigned char>(expected[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

inline bool SecretNameHas(std::string_view name, std::string_view piece) {
    if (piece.empty() || name.size() < piece.size()) {
        return false;
    }
    for (size_t i = 0; i + piece.size() <= name.size(); ++i) {
        if (SecretNameIs(name.substr(i, piece.size()), piece)) {
            return true;
        }
    }
    return false;
}

// The host of "ssh user@host[:port]", without the port. Empty when there is no @.
inline std::string SecretHost(std::string_view sshName) {
    const size_t at = sshName.rfind('@');
    if (at == std::string_view::npos) {
        return {};
    }
    std::string host(sshName.substr(at + 1));
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && colon + 1 < host.size()) {
        bool port = true;
        for (size_t i = colon + 1; i < host.size(); ++i) {
            if (host[i] < '0' || host[i] > '9') {
                port = false;
                break;
            }
        }
        if (port) {
            host.resize(colon);
        }
    }
    return host;
}

inline SecretFace Face(int group, int rank, std::string heading, std::string title, std::string hint, bool leaked = false) {
    SecretFace face;
    face.group = group;
    face.rank = rank;
    face.heading = std::move(heading);
    face.title = std::move(title);
    face.hint = std::move(hint);
    face.leaked = leaked;
    return face;
}

// Groups, in this order: servers, SSH keys, subscriptions, GitHub, Drive, Cloudflare, Groq, home, the rest.
inline SecretFace DescribeSecret(std::string_view name) {
    const std::string server = ServerSecretName(name);
    if (!server.empty() && server == name) {
        const std::string host = SecretHost(server);
        if (host == "5.83.147.210") {
            return Face(0, 0, "Серверы", "packetlab", host);
        }
        if (host == "94.249.230.169") {
            return Face(0, 1, "Серверы", "Нидерланды", host);
        }
        std::string shown = server.substr(4);
        return Face(0, 50, "Серверы", shown, "сервер");
    }
    if (SecretNameIs(name, "id_ed25519") || SecretNameIs(name, "id_rsa") || SecretNameIs(name, "id_ecdsa") ||
        SecretNameIs(name, "SSH_KEY")) {
        return Face(1, 0, "Ключи SSH", "Ключ серверов", std::string(name));
    }
    if (SecretNameIs(name, "WARP")) {
        return Face(2, 0, "Подписки", "WARP", "ключ аккаунта", true);
    }
    if (SecretNameIs(name, "PACKETLAB_SUB")) {
        return Face(2, 1, "Подписки", "packetlab", "ссылка подписки", true);
    }
    if (SecretNameIs(name, "GITHUB")) {
        return Face(3, 0, "GitHub", "Токен", "пуш и релизы");
    }
    if (SecretNameIs(name, "GITHUB_OAUTH_ID")) {
        return Face(3, 1, "GitHub", "OAuth ID", "вход через браузер");
    }
    if (SecretNameIs(name, "GITHUB_OAUTH_SECRET")) {
        return Face(3, 2, "GitHub", "OAuth секрет", "вход через браузер");
    }
    if (name.starts_with("GITHUB")) {
        return Face(3, 50, "GitHub", std::string(name), "GitHub");
    }
    if (SecretNameIs(name, "GDRIVE_CLIENT_ID")) {
        return Face(4, 0, "Google Drive", "Client ID", "приложение Диска");
    }
    if (SecretNameIs(name, "GDRIVE_CLIENT_SECRET")) {
        return Face(4, 1, "Google Drive", "Client secret", "приложение Диска");
    }
    if (SecretNameIs(name, "GDRIVE_REFRESH_TOKEN")) {
        return Face(4, 2, "Google Drive", "Доступ", "refresh token");
    }
    if (name.starts_with("GDRIVE_")) {
        return Face(4, 50, "Google Drive", std::string(name), "Google Drive");
    }
    if (SecretNameIs(name, "CLAUDEFLARE")) {
        return Face(5, 0, "Cloudflare", "API", "SovereignBrains");
    }
    if (SecretNameIs(name, "CFTOKENAEZA")) {
        return Face(5, 1, "Cloudflare", "Токен Aeza", "невалиден с 29.10");
    }
    if (SecretNameIs(name, "Groq") || SecretNameIs(name, "GROC")) {
        return Face(6, 0, "Groq", "Groq", "голос Whisper");
    }
    if (SecretNameHas(name, "router")) {
        const std::string hint = SecretNameIs(name, "192.168.31.1 router") ? "192.168.31.1" : std::string(name);
        return Face(7, 0, "Дом", "Роутер", hint);
    }
    return Face(8, 100, "Другое", std::string(name), {});
}

}  // namespace aic
