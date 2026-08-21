#pragma once
#include <string>
#include <vector>

struct JellyfinItem {
    std::string id;
    std::string name;
    std::string type; // "CollectionFolder", "Folder", "Movie", "Series", "Episode", ...
};

class JellyfinClient {
public:
    JellyfinClient(std::string host, int port);

    // Logs in with a username/password and stashes the access token +
    // user id for subsequent calls. Returns false on failure -- check
    // lastError() for details.
    bool authenticate(const std::string& username, const std::string& password);

    // Top-level libraries ("Movies", "TV Shows", "Music", ...).
    bool getViews(std::vector<JellyfinItem>& out);

    // Contents of a given library/folder.
    bool getItems(const std::string& parentId, std::vector<JellyfinItem>& out);

    const std::string& lastError() const { return last_error_; }

private:
    std::string host_;
    int port_;
    std::string token_;
    std::string user_id_;
    std::string last_error_;

    std::string authHeader() const;
};
