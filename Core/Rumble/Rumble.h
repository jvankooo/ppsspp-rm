// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

// Rumble for PSP games, which never asked for it (the PSP has no motor). It is inferred from two sources:
//
// - the game's sound: a sudden loud burst below ~150 Hz in the mix of the sound effects (music channels,
//   the ones sceAtrac and sceMp3 decode into, are left out), against its recent average, timed with when
//   those samples play;
// - the game's own state, read from its memory once per vblank: values and rules from a per-game profile.
//
// Rumble only runs for a game that has a profile: memstick PSP/RUMBLE/<DISC_ID>.ini (see docs/rumble.md).
// A platform that can vibrate polls Sample() from a thread of its own and drives its motors with it.

#include <cstdint>
#include <string>
#include <vector>

namespace Rumble {

// What the motors should do now. HD Rumble style: two bands, each with an amplitude (0..1) and a frequency in Hz.
// Platforms with plain motors can use ampLow for the big (low) motor and ampHigh for the small one.
struct MotorState {
	float ampLow = 0.0f;
	float freqLow = 160.0f;
	float ampHigh = 0.0f;
	float freqHigh = 320.0f;
	bool operator==(const MotorState &o) const {
		return ampLow == o.ampLow && ampHigh == o.ampHigh && freqLow == o.freqLow && freqHigh == o.freqHigh;
	}
};

// Strength levels of the setting (g_Config.iRumbleStrength and the per-game override).
enum class Strength {
	NONE = 0,
	LIGHT = 1,
	MEDIUM = 2,
	STRONG = 3,
	HARD = 4,
	COUNT,
};

extern const char *const g_strengthNames[(int)Strength::COUNT];

// --- Emulator side (CPU thread) ---

// Once per vblank. Loads the profile when the game changes, reads the values and runs the rules.
void OnVBlank();
// Values jump after a savestate load or a game reset: start the rules afresh.
void ResetGameState();
// Game shut down.
void OnShutdown();

// sceAtrac / sceMp3 decoded music into this buffer. A sceAudio channel that plays from it is music.
void NoteMusicBuffer(uint32_t addr, uint32_t bytes);
bool IsMusicBuffer(uint32_t addr, uint32_t bytes);
// A movie frame was decoded (sceMpeg / scePsmfPlayer).
void NoteVideoFrame();

// Whether the mixer should hand over the mono mix of the sound effects (sound rumble is on for this game).
bool WantsAudio();
// After each mixed block: the mono mix of the sound effects (null if WantsAudio() was false), its frame count and rate.
void FeedAudio(const int32_t *mono, int frames, int sampleRateHz);

// --- Audio output thread ---

// The host played this many frames of game audio (at sampleRateHz). Used to time the sound rumble.
void OnAudioPlayed(int frames, int sampleRateHz);

// --- Rumble output (any thread) ---

// The platform has a device that can rumble. Without one, the settings show that rumble isn't available.
void SetOutputAvailable(bool available);
bool OutputAvailable();
// What the motors should do now (time_now_d() seconds). Zero while nothing plays, the emulator is paused,
// or rumble is off.
MotorState Sample(double now);
// A test pulse for the settings (ignores the game state; uses the current strength).
void TestPulse(int kind);

// --- Settings and status (UI thread) ---

struct RuleInfo {
	std::string name;
	std::string label;
	bool enabled;
};

struct Status {
	bool inGame = false;
	std::string discID;
	std::string profilePath;   // Where the profile was looked for.
	bool profileFound = false;
	std::string profileName;
	std::string profileError;  // Parse problems, shown in the settings.
	bool hasSound = false;
	std::vector<RuleInfo> rules;
	std::vector<std::string> valueLines;  // "health = 87.5 @ 09A1B2C4" for research.
	uint64_t rumblesFired = 0;
};
Status GetStatus();

// Per-game settings, kept in PSP/RUMBLE/settings/<DISC_ID>.ini.
struct GameSettings {
	int strength = -1;          // -1: the global setting, else a Strength.
	int soundSensitivity = -1;  // -1: the profile's default. 0 off, 1..10.
	std::vector<std::pair<std::string, bool>> rules;  // Rule name, enabled.
};
// The running game's (empty if none or without profile).
GameSettings GetGameSettings();
void SetGameSettings(const GameSettings &settings);
int ProfileSoundSensitivity();  // The profile's default sensitivity (5 without one).
int EffectiveStrength();        // The strength in use for the running game.

// Research mode: writes RAM and the current values to PSP/RUMBLE/snapshots. Returns the file written, or empty.
std::string SaveSnapshot(const std::string &label);
// Reloads the running game's profile from the memstick (after editing it).
void ReloadProfile();

// The folders used.
std::string RumbleDirectory();

}  // namespace Rumble
