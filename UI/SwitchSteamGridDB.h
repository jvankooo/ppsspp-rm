// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class SteamGridDBResult {
	OK,
	MissingKey,
	Unauthorized,
	Forbidden,
	NetworkError,
	NotFound,
	InvalidResponse,
	Cancelled,
};

struct SteamGridDBGame {
	int64_t id = 0;
	std::string name;
};

struct SteamGridDBArtwork {
	std::string url;
	std::string thumbnailUrl;
	int width = 0;
	int height = 0;
};

SteamGridDBResult SteamGridDB_Search(const std::string &apiKey, const std::string &title, std::vector<SteamGridDBGame> *games,
									 const std::atomic<bool> *cancel);
SteamGridDBResult SteamGridDB_GetIcons(const std::string &apiKey, int64_t gameId, std::vector<SteamGridDBArtwork> *artworks,
									   const std::atomic<bool> *cancel);
SteamGridDBResult SteamGridDB_GetArtwork(const std::string &apiKey, int64_t gameId, bool background,
	std::vector<SteamGridDBArtwork> *artworks, const std::atomic<bool> *cancel);
SteamGridDBResult SteamGridDB_DownloadImage(const std::string &url, std::vector<uint8_t> *data, const std::atomic<bool> *cancel);
const char *SteamGridDB_ResultMessage(SteamGridDBResult result);

// API keys are 32 letters and digits. Drops what pasting tends to add: spaces, line breaks, quotes, "Bearer".
std::string SteamGridDB_CleanKey(std::string_view key);
// "Not set", or the key's start and end and its length, to compare with the one on steamgriddb.com.
std::string SteamGridDB_DescribeKey(const std::string &key);
// Typing a key on the Switch keyboard is error-prone: a key saved on the SD card, in
// /switch/ppsspp-nx/steamgriddb_key.txt, is taken from there instead, and the file renamed to .imported.
bool SteamGridDB_ImportKeyFile(std::string *key);
