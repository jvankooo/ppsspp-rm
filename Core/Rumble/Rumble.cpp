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

// Rumble for PSP games. See Rumble.h for the idea and docs/rumble.md for the profile format.

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "Common/Data/Format/IniFile.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/MemMap.h"
#include "Core/Rumble/Rumble.h"
#include "Core/System.h"
#include "Core/Util/PathUtil.h"

namespace Rumble {

const char *const g_strengthNames[(int)Strength::COUNT] = { "None", "Light", "Medium", "Strong", "Hard" };

// What each strength level does to the amplitudes.
static const float STRENGTH_SCALE[(int)Strength::COUNT] = { 0.0f, 0.42f, 0.64f, 0.84f, 1.0f };

// --- Profile ---

enum class ValueType { U8, U16, U32, S8, S16, S32, FLOAT };

struct PathStep {
	enum Kind { DEREF, ADD, FIND } kind;
	int64_t add = 0;
	uint32_t count = 0, keyOff = 0, key = 0;
};

struct ValueDef {
	std::string name;
	uint32_t address = 0;
	std::vector<PathStep> steps;
	ValueType type = ValueType::U32;
	uint32_t mask = 0;
	bool hasMask = false;
};

enum class Op { GT, LT, GE, LE, EQ, NE };

struct Cond {
	int value = -1;
	Op op = Op::NE;
	double rhs = 0.0;
};

enum class Event { DROP, RISE, CHANGE, LEVEL };

// How an effect feels: the HD Rumble bands it uses and how it fades.
enum class Feel { HEAVY, SHARP, THUMP, SOFT, ENGINE, BUZZ, COUNT };
static const char *const FEEL_NAMES[(int)Feel::COUNT] = { "heavy", "sharp", "thump", "soft", "engine", "buzz" };

struct FeelDef {
	float freqLow, freqHigh;
	float weightLow, weightHigh;
};
static const FeelDef FEELS[(int)Feel::COUNT] = {
	{ 130.0f, 260.0f, 1.00f, 0.55f },  // HEAVY: a blow taken, a crash.
	{ 180.0f, 340.0f, 0.45f, 1.00f },  // SHARP: a short crisp tap, a blow landed.
	{ 160.0f, 320.0f, 1.00f, 0.60f },  // THUMP: from the sound (as in gowco-sw).
	{ 140.0f, 280.0f, 0.80f, 0.40f },  // SOFT
	{ 85.0f, 190.0f, 1.00f, 0.30f },   // ENGINE: continuous, low.
	{ 220.0f, 440.0f, 0.50f, 0.90f },  // BUZZ: continuous, rattly (off-road).
};

struct RuleDef {
	std::string name;
	std::string label;
	int value = -1;
	Event event = Event::DROP;
	int relativeTo = -1;      // Value index the change is divided by, or -1.
	double relativeConst = 1.0;
	double minChange = 0.0;
	double maxChange = 1e30;
	bool perSecond = false;   // The change per second (at 60 vblanks a second) instead of per frame.
	float strength = 0.5f;
	float gain = 0.0f;
	float strengthMax = 1.0f;
	int durationMs = 150;
	double big = -1.0;
	float bigStrength = 1.0f;
	int bigDurationMs = 400;
	int cooldownMs = 0;
	Feel feel = Feel::HEAVY;
	// LEVEL: the value mapped from [levelMin, levelMax] to [0, strengthMax].
	double levelMin = 0.0, levelMax = 1.0;
};

struct Profile {
	std::string discID;
	std::string name;
	Path path;
	std::vector<ValueDef> values;
	std::vector<RuleDef> rules;
	std::vector<Cond> activeWhen;
	bool sound = true;
	int soundSensitivity = 5;
	bool muteDuringVideo = true;
	std::string errors;
};

// --- State ---

struct ValueState {
	bool valid = false;
	uint32_t addr = 0;
	double v = 0.0;
	bool prevValid = false;
	uint32_t prevAddr = 0;
	double prev = 0.0;
};

struct Effect {
	double start;
	double duration;
	float strength;
	Feel feel;
	bool test;
};

struct SoundEvent {
	int64_t posUs;  // Pushed-audio time of the samples it goes with.
	float level;
};

static std::mutex g_mutex;  // Guards everything below that the output and UI threads also touch.
static std::unique_ptr<Profile> g_profile;
static std::string g_checkedDiscID;
static std::atomic<bool> g_checked{ false };
static std::vector<ValueState> g_valueStates;
static std::vector<bool> g_ruleEnabled;
static std::vector<double> g_ruleLastFire;
static std::vector<float> g_levels;  // LEVEL rules: current level (0 when off).
static GameSettings g_gameSettings;
static std::deque<Effect> g_effects;
static std::deque<SoundEvent> g_soundQueue;
static uint64_t g_fired = 0;
static bool g_active = false;

static std::atomic<bool> g_outputAvailable{ false };
static std::atomic<double> g_lastVBlankTime{ 0.0 };
static std::atomic<double> g_lastVideoTime{ -100.0 };
static std::atomic<double> g_lastPlayedTime{ -100.0 };
static std::atomic<int64_t> g_pushedUs{ 0 };
static std::atomic<int64_t> g_playedUs{ 0 };
static std::atomic<bool> g_wantsAudio{ false };

// CPU thread only.
static int g_frame = 0;
static int g_settleFrames = 0;
static struct { uint32_t addr, bytes; } g_musicBufs[8];
static int g_musicNext = 0;

// The sound detector (CPU thread).
static struct {
	float lp1 = 0.0f, lp2 = 0.0f, sumLo = 0.0f, sumAll = 0.0f, avgLo = 0.0f, avgAll = 0.0f;
	int n = 0;
	int rate = 44100;
	float coef = 0.021f;
	int block = 441;
} g_det;

static Path RumbleDir() {
	return GetSysDirectory(DIRECTORY_PSP) / "RUMBLE";
}

std::string RumbleDirectory() {
	return RumbleDir().ToVisualString();
}

// --- Parsing ---

static bool ParseInt(std::string_view s, int64_t *out) {
	std::string t(StripSpaces(s));
	if (t.empty()) {
		return false;
	}
	bool neg = false;
	size_t i = 0;
	if (t[0] == '+' || t[0] == '-') {
		neg = t[0] == '-';
		i = 1;
	}
	char *end = nullptr;
	const char *start = t.c_str() + i;
	unsigned long long v = strtoull(start, &end, 0);
	if (end == start || *end != '\0') {
		return false;
	}
	*out = neg ? -(int64_t)v : (int64_t)v;
	return true;
}

static bool ParseDouble(std::string_view s, double *out) {
	int64_t i;
	if (ParseInt(s, &i)) {
		*out = (double)i;
		return true;
	}
	std::string t(StripSpaces(s));
	char *end = nullptr;
	*out = strtod(t.c_str(), &end);
	return end != t.c_str() && *end == '\0';
}

static bool ParseBool(std::string_view s, bool def) {
	std::string t(StripSpaces(s));
	for (auto &c : t) {
		c = (char)tolower((unsigned char)c);
	}
	if (t == "on" || t == "true" || t == "yes" || t == "1") {
		return true;
	}
	if (t == "off" || t == "false" || t == "no" || t == "0") {
		return false;
	}
	return def;
}

static std::string Get(const Section *sec, const char *key, const char *def = "") {
	std::string v;
	if (sec && sec->Get(key, &v)) {
		// Inline comments.
		size_t c = v.find(';');
		if (c != std::string::npos) {
			v = v.substr(0, c);
		}
		return std::string(StripSpaces(v));
	}
	return def;
}

static int FindValue(const Profile &p, std::string_view name) {
	for (size_t i = 0; i < p.values.size(); i++) {
		if (equalsNoCase(p.values[i].name, name)) {
			return (int)i;
		}
	}
	return -1;
}

static bool ParsePath(std::string_view text, std::vector<PathStep> *steps, std::string *err) {
	size_t i = 0;
	while (i < text.size()) {
		char c = text[i];
		if (c == ' ' || c == '\t' || c == ',') {
			i++;
			continue;
		}
		PathStep step{};
		if (c == '*') {
			step.kind = PathStep::DEREF;
			i++;
		} else if (c == '+' || c == '-') {
			size_t j = i + 1;
			while (j < text.size() && (isalnum((unsigned char)text[j]))) {
				j++;
			}
			if (!ParseInt(text.substr(i, j - i), &step.add)) {
				*err = "bad offset in path: " + std::string(text.substr(i, j - i));
				return false;
			}
			step.kind = PathStep::ADD;
			i = j;
		} else if (text.substr(i, 5) == "find(") {
			size_t close = text.find(')', i);
			if (close == std::string_view::npos) {
				*err = "find( without )";
				return false;
			}
			std::vector<std::string> args;
			SplitString(std::string(text.substr(i + 5, close - i - 5)), ',', args);
			int64_t a[3];
			if (args.size() != 3 || !ParseInt(args[0], &a[0]) || !ParseInt(args[1], &a[1]) || !ParseInt(args[2], &a[2])) {
				*err = "find() takes count, key offset, key value";
				return false;
			}
			step.kind = PathStep::FIND;
			step.count = (uint32_t)std::clamp<int64_t>(a[0], 1, 1024);
			step.keyOff = (uint32_t)a[1];
			step.key = (uint32_t)a[2];
			i = close + 1;
		} else {
			*err = StringFromFormat("unexpected '%c' in path", c);
			return false;
		}
		steps->push_back(step);
	}
	return true;
}

static bool ParseCond(const Profile &p, std::string_view text, Cond *cond, std::string *err) {
	static const struct { const char *s; Op op; } ops[] = {
		{ ">=", Op::GE }, { "<=", Op::LE }, { "==", Op::EQ }, { "!=", Op::NE }, { ">", Op::GT }, { "<", Op::LT },
	};
	for (const auto &o : ops) {
		size_t at = text.find(o.s);
		if (at == std::string_view::npos) {
			continue;
		}
		std::string name(StripSpaces(text.substr(0, at)));
		cond->value = FindValue(p, name);
		cond->op = o.op;
		if (cond->value < 0) {
			*err = "active_when: unknown value " + name;
			return false;
		}
		if (!ParseDouble(text.substr(at + strlen(o.s)), &cond->rhs)) {
			*err = "active_when: bad number in " + std::string(text);
			return false;
		}
		return true;
	}
	*err = "active_when: no comparison in " + std::string(text);
	return false;
}

static std::unique_ptr<Profile> LoadProfile(const std::string &discID, const Path &path) {
	IniFile ini;
	if (!ini.Load(path)) {
		return nullptr;
	}
	auto p = std::make_unique<Profile>();
	p->discID = discID;
	p->path = path;
	std::string err;
	auto addError = [&](const std::string &e) {
		if (!p->errors.empty()) {
			p->errors += "; ";
		}
		p->errors += e;
		WARN_LOG(Log::System, "Rumble profile %s: %s", path.c_str(), e.c_str());
	};

	const Section *prof = ini.GetSection("Profile");
	p->name = Get(prof, "name", discID.c_str());
	p->sound = ParseBool(Get(prof, "sound", "on"), true);
	double d;
	if (ParseDouble(Get(prof, "sound_sensitivity", "5"), &d)) {
		p->soundSensitivity = std::clamp((int)d, 0, 10);
	}
	p->muteDuringVideo = ParseBool(Get(prof, "mute_during_video", "on"), true);

	// Values first: rules and conditions refer to them.
	for (const auto &secPtr : ini.Sections()) {
		const Section *sec = secPtr.get();
		if (!startsWithNoCase(sec->name(), "Value.")) {
			continue;
		}
		ValueDef v;
		v.name = sec->name().substr(6);
		int64_t addr = 0;
		if (!ParseInt(Get(sec, "address"), &addr)) {
			addError("value " + v.name + ": no address");
			continue;
		}
		v.address = (uint32_t)addr;
		if (!ParsePath(Get(sec, "path"), &v.steps, &err)) {
			addError("value " + v.name + ": " + err);
			continue;
		}
		std::string type = Get(sec, "type", "u32");
		static const struct { const char *n; ValueType t; } types[] = {
			{ "u8", ValueType::U8 }, { "u16", ValueType::U16 }, { "u32", ValueType::U32 }, { "s8", ValueType::S8 },
			{ "s16", ValueType::S16 }, { "s32", ValueType::S32 }, { "float", ValueType::FLOAT },
		};
		bool found = false;
		for (const auto &t : types) {
			if (equalsNoCase(type, t.n)) {
				v.type = t.t;
				found = true;
			}
		}
		if (!found) {
			addError("value " + v.name + ": unknown type " + type);
			continue;
		}
		int64_t mask;
		if (ParseInt(Get(sec, "mask"), &mask)) {
			v.mask = (uint32_t)mask;
			v.hasMask = true;
		}
		p->values.push_back(v);
	}

	std::vector<std::string> conds;
	SplitString(Get(prof, "active_when"), ',', conds);
	for (const auto &c : conds) {
		if (StripSpaces(c).empty()) {
			continue;
		}
		Cond cond;
		if (ParseCond(*p, c, &cond, &err)) {
			p->activeWhen.push_back(cond);
		} else {
			addError(err);
		}
	}

	for (const auto &secPtr : ini.Sections()) {
		const Section *sec = secPtr.get();
		if (!startsWithNoCase(sec->name(), "Rule.")) {
			continue;
		}
		RuleDef r;
		r.name = sec->name().substr(5);
		r.label = Get(sec, "label", r.name.c_str());
		r.value = FindValue(*p, Get(sec, "value"));
		if (r.value < 0) {
			addError("rule " + r.name + ": unknown value " + Get(sec, "value"));
			continue;
		}
		std::string ev = Get(sec, "event", "drop");
		if (equalsNoCase(ev, "drop")) {
			r.event = Event::DROP;
		} else if (equalsNoCase(ev, "rise")) {
			r.event = Event::RISE;
		} else if (equalsNoCase(ev, "change")) {
			r.event = Event::CHANGE;
		} else if (equalsNoCase(ev, "level")) {
			r.event = Event::LEVEL;
		} else {
			addError("rule " + r.name + ": unknown event " + ev);
			continue;
		}
		std::string rel = Get(sec, "relative_to");
		if (!rel.empty()) {
			r.relativeTo = FindValue(*p, rel);
			if (r.relativeTo < 0 && !ParseDouble(rel, &r.relativeConst)) {
				addError("rule " + r.name + ": relative_to is neither a value nor a number");
			}
		}
		auto num = [&](const char *key, double def) {
			double out = def;
			std::string s = Get(sec, key);
			if (!s.empty() && !ParseDouble(s, &out)) {
				addError("rule " + r.name + ": bad number for " + key);
				out = def;
			}
			return out;
		};
		r.minChange = num("min", 0.0);
		r.maxChange = num("max", 1e30);
		r.perSecond = ParseBool(Get(sec, "per_second", "off"), false);
		r.strength = (float)num("strength", 0.5);
		r.gain = (float)num("gain", 0.0);
		r.strengthMax = (float)num("strength_max", 1.0);
		r.durationMs = (int)num("duration", 150);
		r.big = num("big", -1.0);
		r.bigStrength = (float)num("big_strength", 1.0);
		r.bigDurationMs = (int)num("big_duration", 400);
		r.cooldownMs = (int)num("cooldown", 0);
		r.levelMin = num("level_min", 0.0);
		r.levelMax = num("level_max", 1.0);
		std::string feel = Get(sec, "feel", r.event == Event::LEVEL ? "engine" : "heavy");
		bool found = false;
		for (int i = 0; i < (int)Feel::COUNT; i++) {
			if (equalsNoCase(feel, FEEL_NAMES[i])) {
				r.feel = (Feel)i;
				found = true;
			}
		}
		if (!found) {
			addError("rule " + r.name + ": unknown feel " + feel);
		}
		p->rules.push_back(r);
	}
	return p;
}

// --- Per-game settings ---

static Path GameSettingsPath(const std::string &discID) {
	return RumbleDir() / "settings" / (discID + ".ini");
}

// Under g_mutex.
static void LoadGameSettingsLocked() {
	g_gameSettings = GameSettings();
	g_ruleEnabled.assign(g_profile->rules.size(), true);
	IniFile ini;
	if (ini.Load(GameSettingsPath(g_profile->discID))) {
		const Section *sec = ini.GetSection("Rumble");
		if (sec) {
			sec->Get("Strength", &g_gameSettings.strength);
			sec->Get("SoundSensitivity", &g_gameSettings.soundSensitivity);
			g_gameSettings.strength = std::clamp(g_gameSettings.strength, -1, (int)Strength::COUNT - 1);
			g_gameSettings.soundSensitivity = std::clamp(g_gameSettings.soundSensitivity, -1, 10);
			for (size_t i = 0; i < g_profile->rules.size(); i++) {
				bool on = true;
				if (sec->Get("Rule." + g_profile->rules[i].name, &on)) {
					g_ruleEnabled[i] = on;
				}
			}
		}
	}
	g_gameSettings.rules.clear();
	for (size_t i = 0; i < g_profile->rules.size(); i++) {
		g_gameSettings.rules.emplace_back(g_profile->rules[i].name, g_ruleEnabled[i]);
	}
}

GameSettings GetGameSettings() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_profile ? g_gameSettings : GameSettings();
}

void SetGameSettings(const GameSettings &settings) {
	std::lock_guard<std::mutex> guard(g_mutex);
	if (!g_profile) {
		return;
	}
	g_gameSettings = settings;
	for (size_t i = 0; i < g_profile->rules.size(); i++) {
		for (const auto &r : settings.rules) {
			if (r.first == g_profile->rules[i].name) {
				g_ruleEnabled[i] = r.second;
			}
		}
	}
	IniFile ini;
	Section *sec = ini.GetOrCreateSection("Rumble");
	sec->Set("Strength", settings.strength);
	sec->Set("SoundSensitivity", settings.soundSensitivity);
	for (const auto &r : settings.rules) {
		sec->Set("Rule." + r.first, r.second);
	}
	Path path = GameSettingsPath(g_profile->discID);
	File::CreateFullPath(path.NavigateUp());
	if (!ini.Save(path)) {
		WARN_LOG(Log::System, "Rumble: couldn't save %s", path.c_str());
	}
}

int ProfileSoundSensitivity() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_profile ? g_profile->soundSensitivity : 5;
}

static int EffectiveStrengthLocked() {
	if (g_profile && g_gameSettings.strength >= 0) {
		return g_gameSettings.strength;
	}
	return std::clamp(g_Config.iRumbleStrength, 0, (int)Strength::COUNT - 1);
}

int EffectiveStrength() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return EffectiveStrengthLocked();
}

// Under g_mutex.
static int SoundSensitivityLocked() {
	if (!g_profile || !g_profile->sound) {
		return 0;
	}
	return g_gameSettings.soundSensitivity >= 0 ? g_gameSettings.soundSensitivity : g_profile->soundSensitivity;
}

static std::string TimeStamp() {
	time_t t = time(nullptr);
	struct tm tm {};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[32];
	strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
	return buf;
}

// --- Memory ---

static bool ResolveValue(const ValueDef &def, uint32_t *addrOut, double *out) {
	uint32_t addr = def.address;
	for (const PathStep &s : def.steps) {
		switch (s.kind) {
		case PathStep::DEREF:
			if (!Memory::IsValidRange(addr, 4)) {
				return false;
			}
			addr = Memory::ReadUnchecked_U32(addr);
			break;
		case PathStep::ADD:
			addr = (uint32_t)((int64_t)addr + s.add);
			break;
		case PathStep::FIND:
		{
			bool found = false;
			for (uint32_t i = 0; i < s.count; i++) {
				const uint32_t at = addr + 4 * i;
				if (!Memory::IsValidRange(at, 4)) {
					break;
				}
				const uint32_t p = Memory::ReadUnchecked_U32(at);
				if (Memory::IsValidRange(p + s.keyOff, 4) && Memory::ReadUnchecked_U32(p + s.keyOff) == s.key) {
					addr = p;
					found = true;
					break;
				}
			}
			if (!found) {
				return false;
			}
			break;
		}
		}
	}
	static const uint32_t sizes[] = { 1, 2, 4, 1, 2, 4, 4 };
	if (!Memory::IsValidRange(addr, sizes[(int)def.type])) {
		return false;
	}
	double v = 0.0;
	switch (def.type) {
	case ValueType::U8: v = Memory::ReadUnchecked_U8(addr); break;
	case ValueType::U16: v = Memory::ReadUnchecked_U16(addr); break;
	case ValueType::U32: v = Memory::ReadUnchecked_U32(addr); break;
	case ValueType::S8: v = (int8_t)Memory::ReadUnchecked_U8(addr); break;
	case ValueType::S16: v = (int16_t)Memory::ReadUnchecked_U16(addr); break;
	case ValueType::S32: v = (int32_t)Memory::ReadUnchecked_U32(addr); break;
	case ValueType::FLOAT: v = Memory::ReadUnchecked_Float(addr); break;
	}
	if (def.hasMask) {
		v = (double)((uint32_t)(int64_t)v & def.mask);
	}
	if (std::isnan(v) || std::isinf(v)) {
		return false;
	}
	*addrOut = addr;
	*out = v;
	return true;
}

// --- Loading ---

// Under g_mutex.
static void ClearRuntimeLocked() {
	g_valueStates.clear();
	g_ruleLastFire.clear();
	g_levels.clear();
	g_effects.clear();
	g_soundQueue.clear();
	g_active = false;
	if (g_profile) {
		g_valueStates.resize(g_profile->values.size());
		g_ruleLastFire.assign(g_profile->rules.size(), -100.0);
		g_levels.assign(g_profile->rules.size(), 0.0f);
	}
}

static void CheckGame() {
	std::string discID = g_paramSFO.GetDiscID();
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		if (g_checked && discID == g_checkedDiscID) {
			return;
		}
	}
	std::unique_ptr<Profile> profile;
	Path path = RumbleDir() / (discID + ".ini");
	if (!discID.empty() && File::Exists(path)) {
		profile = LoadProfile(discID, path);
	}
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		g_profile = std::move(profile);
		g_checked = true;
		g_checkedDiscID = discID;
		g_fired = 0;
		if (g_profile) {
			LoadGameSettingsLocked();
		}
		ClearRuntimeLocked();
		g_wantsAudio = SoundSensitivityLocked() > 0;
	}
	g_frame = 0;
	g_settleFrames = 30;
	g_det = {};
	if (g_profile) {
		INFO_LOG(Log::System, "Rumble profile loaded: %s (%s), %d values, %d rules%s", g_profile->name.c_str(), path.c_str(),
			(int)g_profile->values.size(), (int)g_profile->rules.size(), g_profile->errors.empty() ? "" : " (with errors)");
		if (g_Config.bRumble && !g_profile->errors.empty()) {
			g_OSD.Show(OSDType::MESSAGE_WARNING, "Rumble: " + g_profile->name, g_profile->errors, 5.0f, "rumble_profile");
		}
	} else if (!discID.empty()) {
		INFO_LOG(Log::System, "No rumble profile for %s (%s)", discID.c_str(), path.c_str());
	}
}

void ReloadProfile() {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_checked = false;
}

void ResetGameState() {
	std::lock_guard<std::mutex> guard(g_mutex);
	for (auto &vs : g_valueStates) {
		vs = ValueState();
	}
	std::fill(g_levels.begin(), g_levels.end(), 0.0f);
	g_effects.clear();
	g_soundQueue.clear();
	g_settleFrames = 30;
}

void OnShutdown() {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_profile.reset();
	g_checked = false;
	g_checkedDiscID.clear();
	g_wantsAudio = false;
	ClearRuntimeLocked();
	g_musicNext = 0;
	memset(g_musicBufs, 0, sizeof(g_musicBufs));
}

// --- Rules (CPU thread) ---

// Under g_mutex.
static void AddEffectLocked(float strength, int durationMs, Feel feel, bool test = false) {
	double now = time_now_d();
	if (g_effects.size() >= 32) {
		g_effects.pop_front();
	}
	g_effects.push_back(Effect{ now, durationMs / 1000.0, std::clamp(strength, 0.0f, 1.0f), feel, test });
	g_fired++;
}

static bool CondTrue(const Cond &c) {
	if (c.value < 0 || c.value >= (int)g_valueStates.size() || !g_valueStates[c.value].valid) {
		return false;
	}
	double v = g_valueStates[c.value].v;
	switch (c.op) {
	case Op::GT: return v > c.rhs;
	case Op::LT: return v < c.rhs;
	case Op::GE: return v >= c.rhs;
	case Op::LE: return v <= c.rhs;
	case Op::EQ: return v == c.rhs;
	case Op::NE: return v != c.rhs;
	}
	return false;
}

static bool VideoPlaying(double now) {
	return now - g_lastVideoTime.load() < 0.35;
}

void OnVBlank() {
	const double now = time_now_d();
	g_lastVBlankTime = now;
	if (!g_checked) {
		CheckGame();
	} else if ((g_frame & 63) == 0) {
		// Picks up a game change without a shutdown (rare), cheaply.
		CheckGame();
	}
	g_frame++;

	std::lock_guard<std::mutex> guard(g_mutex);
	if (!g_profile) {
		return;
	}
	g_wantsAudio = SoundSensitivityLocked() > 0 && g_Config.bRumble;
	const Profile &p = *g_profile;

	for (size_t i = 0; i < p.values.size(); i++) {
		ValueState &vs = g_valueStates[i];
		vs.prevValid = vs.valid;
		vs.prevAddr = vs.addr;
		vs.prev = vs.v;
		vs.valid = ResolveValue(p.values[i], &vs.addr, &vs.v);
	}

	bool active = true;
	for (const Cond &c : p.activeWhen) {
		active = active && CondTrue(c);
	}
	if (p.muteDuringVideo && VideoPlaying(now)) {
		active = false;
	}
	if (g_settleFrames > 0) {
		g_settleFrames--;
		active = false;
	}
	g_active = active;
	if (!g_Config.bRumble) {
		std::fill(g_levels.begin(), g_levels.end(), 0.0f);
		return;
	}

	for (size_t i = 0; i < p.rules.size(); i++) {
		const RuleDef &r = p.rules[i];
		const ValueState &vs = g_valueStates[r.value];
		if (r.event == Event::LEVEL) {
			float level = 0.0f;
			if (active && g_ruleEnabled[i] && vs.valid && r.levelMax != r.levelMin) {
				level = (float)std::clamp((vs.v - r.levelMin) / (r.levelMax - r.levelMin), 0.0, 1.0) * r.strengthMax;
			}
			g_levels[i] = level;
			continue;
		}
		// The same value at the same address in both frames, or it's a new object (a level load): no event.
		if (!vs.valid || !vs.prevValid || vs.addr != vs.prevAddr) {
			continue;
		}
		double change = vs.v - vs.prev;
		if (r.event == Event::DROP) {
			change = -change;
		} else if (r.event == Event::CHANGE) {
			change = fabs(change);
		}
		if (change <= 0.0) {
			continue;
		}
		double rel = r.relativeConst;
		if (r.relativeTo >= 0) {
			const ValueState &rs = g_valueStates[r.relativeTo];
			if (!rs.valid || rs.v == 0.0) {
				continue;
			}
			rel = fabs(rs.v);
		}
		double m = change / (rel != 0.0 ? rel : 1.0);
		if (r.perSecond) {
			m *= 60.0;
		}
		if (m < r.minChange || m > r.maxChange) {
			continue;
		}
		if (!active || !g_ruleEnabled[i]) {
			continue;
		}
		if (now - g_ruleLastFire[i] < r.cooldownMs / 1000.0) {
			continue;
		}
		float strength;
		int duration;
		if (r.big >= 0.0 && m > r.big) {
			strength = r.bigStrength;
			duration = r.bigDurationMs;
		} else {
			strength = std::min(r.strength + r.gain * (float)m, r.strengthMax);
			duration = r.durationMs;
		}
		g_ruleLastFire[i] = now;
		AddEffectLocked(strength, duration, r.feel);
	}
}

// --- Sound (CPU thread) ---

void NoteMusicBuffer(uint32_t addr, uint32_t bytes) {
	for (auto &b : g_musicBufs) {
		if (b.addr == addr) {
			b.bytes = bytes;
			return;
		}
	}
	g_musicBufs[g_musicNext].addr = addr;
	g_musicBufs[g_musicNext].bytes = bytes;
	g_musicNext = (g_musicNext + 1) & 7;
}

bool IsMusicBuffer(uint32_t addr, uint32_t bytes) {
	for (const auto &b : g_musicBufs) {
		if (b.bytes && addr < b.addr + b.bytes && b.addr < addr + bytes) {
			return true;
		}
	}
	return false;
}

void NoteVideoFrame() {
	g_lastVideoTime = time_now_d();
}

bool WantsAudio() {
	return g_wantsAudio;
}

void FeedAudio(const int32_t *mono, int frames, int sampleRateHz) {
	if (sampleRateHz <= 0) {
		return;
	}
	const int64_t blockStartUs = g_pushedUs.load();
	g_pushedUs = blockStartUs + (int64_t)frames * 1000000 / sampleRateHz;
	if (!mono) {
		return;
	}
	if (g_det.rate != sampleRateHz) {
		// Two one-pole low-passes at ~150 Hz, and 10 ms blocks.
		g_det.rate = sampleRateHz;
		g_det.coef = 1.0f - expf(-2.0f * 3.14159265f * 150.0f / sampleRateHz);
		g_det.block = sampleRateHz / 100;
	}

	int sensitivity;
	bool gate;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		sensitivity = SoundSensitivityLocked();
		gate = g_active;
	}
	if (sensitivity <= 0) {
		return;
	}
	// Sensitivity 5 is gowco-sw's tuning: a burst 2.5 times the recent average and 0.04 above it.
	// Each step up or down changes the thresholds by a factor of 2^(1/2.5).
	const float f = powf(2.0f, (5 - sensitivity) / 2.5f);
	const float ratio = 1.0f + 1.5f * f;
	const float floorLevel = 0.04f * f;

	for (int i = 0; i < frames; i++) {
		float x = (float)mono[i] / 32768.0f;
		g_det.lp1 += g_det.coef * (x - g_det.lp1);
		g_det.lp2 += g_det.coef * (g_det.lp1 - g_det.lp2);
		g_det.sumLo += g_det.lp2 * g_det.lp2;
		g_det.sumAll += x * x;
		if (++g_det.n < g_det.block) {
			continue;
		}
		float lo = sqrtf(g_det.sumLo / g_det.block);
		float all = sqrtf(g_det.sumAll / g_det.block);
		float thump = (lo - ratio * g_det.avgLo - floorLevel) * 6.0f;
		float level = thump >= 0.1f ? 0.4f + 0.6f * std::min(thump, 1.0f) : 0.0f;
		g_det.avgLo += (lo - g_det.avgLo) * 0.05f;  // ~200 ms
		g_det.avgAll += (all - g_det.avgAll) * 0.05f;
		g_det.sumLo = g_det.sumAll = 0.0f;
		g_det.n = 0;
		if (level > 0.0f && gate && g_Config.bRumble) {
			const int64_t posUs = blockStartUs + (int64_t)(i + 1) * 1000000 / sampleRateHz;
			std::lock_guard<std::mutex> guard(g_mutex);
			if (g_soundQueue.size() < 256) {
				g_soundQueue.push_back(SoundEvent{ posUs, level });
			}
		}
	}
}

// --- Output ---

void OnAudioPlayed(int frames, int sampleRateHz) {
	if (sampleRateHz <= 0) {
		return;
	}
	g_lastPlayedTime = time_now_d();
	int64_t played = g_playedUs.load() + (int64_t)frames * 1000000 / sampleRateHz;
	const int64_t pushed = g_pushedUs.load();
	// The host's buffer drops or repeats samples now and then: keep the two in step.
	if (played > pushed) {
		played = pushed;
	} else if (pushed - played > 400000) {
		played = pushed - 100000;
	}
	g_playedUs = played;
}

void SetOutputAvailable(bool available) {
	g_outputAvailable = available;
}

bool OutputAvailable() {
	return g_outputAvailable;
}

void TestPulse(int kind) {
	std::lock_guard<std::mutex> guard(g_mutex);
	switch (kind) {
	case 0: AddEffectLocked(0.9f, 380, Feel::HEAVY, true); break;
	case 1: AddEffectLocked(0.5f, 70, Feel::SHARP, true); break;
	default: AddEffectLocked(0.8f, 60, Feel::THUMP, true); break;
	}
}

// Envelope of an effect at time t (seconds since its start), 0..1.
static float Envelope(const Effect &e, double t) {
	if (t < 0.0 || t >= e.duration) {
		return 0.0f;
	}
	const float x = (float)(t / e.duration);
	switch (e.feel) {
	case Feel::THUMP:
		// gowco-sw: a block's level decays to 0.6 of itself every 10 ms.
		return powf(0.6f, (float)(t / 0.010));
	case Feel::SHARP:
		return 1.0f - x;
	case Feel::HEAVY:
		// Hits at full strength, holds a little, then fades.
		return x < 0.3f ? 1.0f : powf((1.0f - x) / 0.7f, 1.5f);
	default:
		return x < 0.5f ? 1.0f : (1.0f - x) * 2.0f;
	}
}

MotorState Sample(double now) {
	MotorState out;
	std::lock_guard<std::mutex> guard(g_mutex);
	const bool emuRunning = now - g_lastVBlankTime.load() < 0.25;
	// Sound events whose samples are playing now. Without audio output, as soon as they come.
	const bool audioPlaying = now - g_lastPlayedTime.load() < 0.2;
	const int64_t played = g_playedUs.load();
	while (!g_soundQueue.empty() && (!audioPlaying || g_soundQueue.front().posUs <= played || !emuRunning)) {
		SoundEvent ev = g_soundQueue.front();
		g_soundQueue.pop_front();
		if (emuRunning) {
			if (g_effects.size() >= 32) {
				g_effects.pop_front();
			}
			g_effects.push_back(Effect{ now, 0.060, ev.level, Feel::THUMP, false });
		}
	}

	const int strength = EffectiveStrengthLocked();
	const float scale = g_Config.bRumble ? STRENGTH_SCALE[strength] : 0.0f;

	float bandAmp[2] = { 0.0f, 0.0f };
	float bandFreq[2] = { 160.0f, 320.0f };
	auto add = [&](float a, Feel feel) {
		const FeelDef &f = FEELS[(int)feel];
		const float lo = a * f.weightLow, hi = a * f.weightHigh;
		if (lo > bandAmp[0]) {
			bandAmp[0] = lo;
			bandFreq[0] = f.freqLow;
		}
		if (hi > bandAmp[1]) {
			bandAmp[1] = hi;
			bandFreq[1] = f.freqHigh;
		}
	};

	for (auto it = g_effects.begin(); it != g_effects.end();) {
		const double t = now - it->start;
		if (t >= it->duration || (!emuRunning && !it->test)) {
			it = g_effects.erase(it);
			continue;
		}
		add(it->strength * Envelope(*it, t), it->feel);
		++it;
	}
	if (emuRunning && g_profile) {
		for (size_t i = 0; i < g_levels.size(); i++) {
			if (g_levels[i] > 0.0f) {
				add(g_levels[i], g_profile->rules[i].feel);
			}
		}
	}

	// HD Rumble is barely felt below ~0.1: lift what's on above that.
	auto shape = [&](float a) {
		return a > 0.01f ? std::min(scale * (0.12f + 0.88f * a), 1.0f) : 0.0f;
	};
	out.ampLow = shape(bandAmp[0]);
	out.ampHigh = shape(bandAmp[1]);
	out.freqLow = bandFreq[0];
	out.freqHigh = bandFreq[1];
	return out;
}

// --- Status ---

static std::string FormatValue(double v) {
	if (v == floor(v) && fabs(v) < 1e9) {
		return StringFromFormat("%lld", (long long)v);
	}
	return StringFromFormat("%.3f", v);
}

Status GetStatus() {
	Status s;
	std::lock_guard<std::mutex> guard(g_mutex);
	s.inGame = g_checked && !g_checkedDiscID.empty();
	s.discID = g_checkedDiscID;
	if (!s.discID.empty()) {
		s.profilePath = (RumbleDir() / (s.discID + ".ini")).ToVisualString();
	}
	s.rumblesFired = g_fired;
	if (!g_profile) {
		return s;
	}
	s.profileFound = true;
	s.profileName = g_profile->name;
	s.profileError = g_profile->errors;
	s.hasSound = g_profile->sound;
	for (size_t i = 0; i < g_profile->rules.size(); i++) {
		s.rules.push_back(RuleInfo{ g_profile->rules[i].name, g_profile->rules[i].label, (bool)g_ruleEnabled[i] });
	}
	for (size_t i = 0; i < g_profile->values.size() && i < g_valueStates.size(); i++) {
		const ValueState &vs = g_valueStates[i];
		if (vs.valid) {
			s.valueLines.push_back(StringFromFormat("%s = %s  @ %08X", g_profile->values[i].name.c_str(), FormatValue(vs.v).c_str(), vs.addr));
		} else {
			s.valueLines.push_back(g_profile->values[i].name + " = (not found)");
		}
	}
	return s;
}

// --- Research snapshots ---

std::string SaveSnapshot(const std::string &label) {
	if (!Memory::IsActive()) {
		return "";
	}
	std::string discID;
	std::string values;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		discID = g_checkedDiscID.empty() ? "UNKNOWN" : g_checkedDiscID;
		if (g_profile) {
			for (size_t i = 0; i < g_profile->values.size() && i < g_valueStates.size(); i++) {
				const ValueState &vs = g_valueStates[i];
				values += vs.valid ? StringFromFormat("%s = %s @ %08X\n", g_profile->values[i].name.c_str(), FormatValue(vs.v).c_str(), vs.addr)
					: g_profile->values[i].name + " = (not found)\n";
			}
		}
	}
	std::string safeLabel;
	for (char c : label) {
		safeLabel += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
	}
	if (safeLabel.size() > 40) {
		safeLabel.resize(40);
	}
	Path dir = RumbleDir() / "snapshots";
	File::CreateFullPath(dir);
	std::string base = discID + "_" + TimeStamp() + StringFromFormat("_f%d", g_frame) + (safeLabel.empty() ? "" : "_" + safeLabel);
	Path bin = dir / (base + ".bin");
	Path txt = dir / (base + ".txt");

	// User RAM, 0x08000000 up (the same layout as the PC tools' dumps).
	const uint32_t start = 0x08000000;
	const uint32_t size = Memory::g_MemorySize;
	if (!Memory::IsValidRange(start, size)) {
		return "";
	}
	auto data = std::make_shared<std::vector<uint8_t>>(Memory::GetPointerUnchecked(start), Memory::GetPointerUnchecked(start) + size);
	std::string info = StringFromFormat("label: %s\ndisc: %s\nframe: %d\nram: %08X-%08X\n", label.c_str(), discID.c_str(), g_frame, start, start + size) + values;
	// Writing 32 MB to an SD card takes a moment: not on the emulator's thread.
	std::thread([data, info, bin, txt]() {
		File::WriteDataToFile(false, data->data(), data->size(), bin);
		File::WriteStringToFile(true, info, txt);
	}).detach();
	return bin.ToVisualString();
}

}  // namespace Rumble
