// profiles.h -- the saved profiles behind the "Who's watching?" screen. Pure data and rules, no UI,
// no Wii U includes, so the host tests can run it (tests/host/test_profiles.cpp).
//
// A profile is one saved Jellyfin sign-in: which server, which user, and the access token the app got
// when that user signed in. The password is never stored (only a token), exactly like the single
// sign-in Ufin always had. Profiles live in config.json under "profiles" (see config_loader.cpp).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct Profile {
    std::string name;          // shown under the picture: the Jellyfin user name
    std::string host;          // the server this profile signs in to
    int port = 0;
    std::string userId;        // Jellyfin's id for the user (also names the user's picture on the server)
    std::string accessToken;   // the saved sign-in; empty = it expired, sign in again
    bool keepLoggedIn = false; // "Auto log in": sign in as this profile at start-up, without showing Who's watching
                               // (only one profile can have it; saved as "keep_logged_in" for older configs)
    int color = -1;            // avatar colour index, handed out when the profile is created
};

namespace profiles {

constexpr size_t kMax = 5;     // profiles on the screen (plus the "Add profile" tile while there is room)
constexpr int kColors = 8;     // avatar colours (see ui/profiles_ui.inc)

// Index of the profile for this Jellyfin user (matched by user id, so a changed server address still
// finds it; a profile saved without an id is matched by name and server), or -1.
inline int find(const std::vector<Profile>& list, const std::string& host, int port, const std::string& userId,
                const std::string& name) {
    for (size_t i = 0; i < list.size(); i++) {
        const Profile& p = list[i];
        if (!userId.empty() && !p.userId.empty()) {
            if (p.userId == userId) return (int)i;
        } else if (p.name == name && p.host == host && p.port == port) {
            return (int)i;
        }
    }
    return -1;
}

// The colour a new profile gets: the first one no other profile has, else by position.
inline int nextColor(const std::vector<Profile>& list) {
    for (int c = 0; c < kColors; c++) {
        bool used = false;
        for (const Profile& p : list) used = used || (p.color == c);
        if (!used) return c;
    }
    return (int)(list.size() % (size_t)kColors);
}

// Adds the profile, or updates the one for the same user (name, server and token follow; the
// "auto log in" choice and the colour stay). Returns its index, or -1 if the list is full.
inline int upsert(std::vector<Profile>& list, const Profile& p) {
    const int at = find(list, p.host, p.port, p.userId, p.name);
    if (at >= 0) {
        Profile& old = list[(size_t)at];
        old.name = p.name.empty() ? old.name : p.name;
        old.host = p.host;
        old.port = p.port;
        if (!p.userId.empty()) old.userId = p.userId;
        old.accessToken = p.accessToken;
        if (old.color < 0) old.color = nextColor(list);
        return at;
    }
    if (list.size() >= kMax) return -1;
    Profile fresh = p;
    fresh.keepLoggedIn = false;
    fresh.color = nextColor(list);
    list.push_back(fresh);
    return (int)list.size() - 1;
}

// Removes profile `index`. Returns where `last` (the index of the last-used profile) points afterwards.
inline int remove(std::vector<Profile>& list, int index, int last) {
    if (index < 0 || (size_t)index >= list.size()) return last;
    list.erase(list.begin() + index);
    if (last == index) return -1;
    return last > index ? last - 1 : last;
}

// Which profile Ufin signs in as at start-up, or -1 to show Who's watching: only a profile with
// "auto log in" ticked and a saved sign-in counts. (Only one can be ticked; if a hand-edited list has several, the one used last wins,
// else the first of them.
inline int autoLoginIndex(const std::vector<Profile>& list, int last) {
    auto ok = [&](int i) {
        return i >= 0 && (size_t)i < list.size() && list[(size_t)i].keepLoggedIn && !list[(size_t)i].accessToken.empty();
    };
    if (ok(last)) return last;
    for (size_t i = 0; i < list.size(); i++)
        if (ok((int)i)) return (int)i;
    return -1;
}

// "Auto log in" is exclusive: ticking profile `index` unticks every other one; `on` = false just unticks it.
inline void setAutoLogin(std::vector<Profile>& list, int index, bool on) {
    if (index < 0 || (size_t)index >= list.size()) return;
    if (on) {
        for (Profile& p : list) p.keepLoggedIn = false;
        list[(size_t)index].keepLoggedIn = true;
    } else {
        list[(size_t)index].keepLoggedIn = false;
    }
}

// Makes sure at most one profile is ticked (a config.json written by hand or by an older Ufin may have
// several): the one used last if it is ticked, else the first ticked one.
inline void normalizeAutoLogin(std::vector<Profile>& list, int last) {
    int keep = -1;
    if (last >= 0 && (size_t)last < list.size() && list[(size_t)last].keepLoggedIn) keep = last;
    for (size_t i = 0; keep < 0 && i < list.size(); i++)
        if (list[i].keepLoggedIn) keep = (int)i;
    for (size_t i = 0; i < list.size(); i++) list[i].keepLoggedIn = ((int)i == keep);
}

// Logs profile `index` out on this console: its saved sign-in is forgotten (the profile itself stays, so
// it can sign in again) and it stops being the auto log in profile. False if it was already logged out.
inline bool logOut(std::vector<Profile>& list, int index) {
    if (index < 0 || (size_t)index >= list.size() || list[(size_t)index].accessToken.empty()) return false;
    list[(size_t)index].accessToken.clear();
    list[(size_t)index].keepLoggedIn = false;
    return true;
}

// The letter shown on a picture-less avatar: the first character of the name (UTF-8 aware), upper-cased
// when it is plain ASCII. "?" for an empty name.
inline std::string initial(const std::string& name) {
    size_t i = 0;
    while (i < name.size() && name[i] == ' ') i++;
    if (i >= name.size()) return "?";
    const unsigned char c = (unsigned char)name[i];
    size_t len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    std::string s = name.substr(i, len);
    if (len == 1 && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
    return s;
}

} // namespace profiles
