// How Jellyfin items are presented in the list: display name, the short
// right-aligned tag, and the longer detail line shown for the selection.
// Pure functions over JellyfinItem so they're covered by host tests.
#pragma once
#include "jellyfin_client.h"
#include "ui/app_ui.h"

#include <string>

// True for things Player can play (video, audio, a Live TV channel)
// rather than folders to browse into.
bool isPlayableItem(const JellyfinItem& item);

// True for the "Live TV" view, whose contents come from
// JellyfinClient::getLiveTvChannels() instead of getItems().
bool isLiveTvView(const JellyfinItem& item);

// Albums and playlists: + shuffles their tracks.
bool isShufflableContainer(const JellyfinItem& item);

std::string itemDisplayName(const JellyfinItem& item);
std::string itemTag(const JellyfinItem& item);
std::string itemDetail(const JellyfinItem& item);

// Icon shown next to the item in lists.
ui::Icon itemIcon(const JellyfinItem& item);

// The rows Ufin adds to the top of Home (not libraries on the server).
static const char* const HOME_RESUME = "UfinResume";
static const char* const HOME_NEXT_UP = "UfinNextUp";
static const char* const HOME_FAVORITES = "UfinFavorites";
bool isHomeRow(const JellyfinItem& item);
JellyfinItem makeHomeRow(const char* type, int count);

// Watched tick, heart, resume bar and unwatched count for a list row.
void applyUserState(const JellyfinItem& item, ui::ListEntry& entry);
