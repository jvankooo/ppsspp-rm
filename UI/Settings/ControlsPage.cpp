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

#include <deque>
#include <string>

#include "ppsspp_config.h"

#include "Common/Data/Text/I18n.h"
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"
#include "Common/System/Request.h"
#include "Common/UI/PopupScreens.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Rumble/Rumble.h"
#include "UI/ControlMappingScreen.h"
#include "UI/GameSettingsScreen.h"
#include "UI/SettingsPage.h"
#include "UI/Settings/MainSettingsPages.h"
#include "UI/TiltAnalogSettingsScreen.h"
#include "UI/TouchControlLayoutScreen.h"
#if PPSSPP_PLATFORM(SWITCH)
#include "Core/ControlMapper.h"
#include "SDL/SDLJoystick.h"
#endif

// --- Rumble (Core/Rumble) ---

// Texts that only exist at runtime (rule labels, value names) are shown with SettingText::Raw, which keeps
// a pointer: they live here, for as long as the app does. Pages may be built again at any time (search).
static const char *KeepText(const std::string &text) {
	static std::deque<std::string> texts;
	for (const std::string &t : texts) {
		if (t == text) {
			return t.c_str();
		}
	}
	texts.push_back(text);
	return texts.back().c_str();
}

static Rumble::GameSettings g_rumbleGame;
static int g_rumbleStrengthChoice = 0;  // 0: the global setting, 1 + Rumble::Strength.
static int g_rumbleSensitivity = 5;

static bool RumbleProfileLoaded() {
	return Rumble::GetStatus().profileFound;
}

static void SaveRumbleGame() {
	g_rumbleGame.strength = g_rumbleStrengthChoice - 1;
	g_rumbleGame.soundSensitivity = g_rumbleSensitivity;
	Rumble::SetGameSettings(g_rumbleGame);
}

static void BuildRumbleSettings(SettingsPage *page) {
	using namespace UI;
	SettingsPageHost *host = page->Host();
	const Rumble::Status status = Rumble::GetStatus();
	if (status.profileFound) {
		g_rumbleGame = Rumble::GetGameSettings();
		g_rumbleStrengthChoice = g_rumbleGame.strength + 1;
		g_rumbleSensitivity = g_rumbleGame.soundSensitivity >= 0 ? g_rumbleGame.soundSensitivity : Rumble::ProfileSoundSensitivity();
	}

	page->Header("Rumble");
	page->Description({ I18NCat::CONTROLS, "RumbleAbout", "The PSP has no rumble. PPSSPP infers it from the game's sound and, with a rumble profile for the game, from what happens in it (hits taken, blows landed). Rumble only runs for games that have a profile in PSP/RUMBLE." });
	page->Setting(&g_Config.bRumble);
	page->Setting(&g_Config.iRumbleStrength).Enabled(&g_Config.bRumble).Hint({ I18NCat::CONTROLS, "RumbleStrengthHint", "For all games, unless a game has its own" });
	page->Choice("Test rumble").OnClick([](UI::EventParams &) {
		Rumble::TestPulse(0);
	}).Enabled(&g_Config.bRumble);
	if (!Rumble::OutputAvailable()) {
		page->Info("Rumble output", T(I18NCat::CONTROLS, "Not available on this device"));
	}

	if (!status.inGame) {
		page->Header("This game");
		page->Description({ I18NCat::CONTROLS, "RumbleNoGame", "Open these settings from the pause menu while a game runs to set the rumble for that game." });
		page->Info("Rumble profiles folder", Rumble::RumbleDirectory());
		std::vector<File::FileInfo> files;
		File::GetFilesInDir(Path(Rumble::RumbleDirectory()), &files, "ini");
		std::string ids;
		for (const auto &f : files) {
			ids += (ids.empty() ? "" : ", ") + f.name.substr(0, f.name.size() - 4);
		}
		page->Info("Installed profiles", ids.empty() ? std::string(T(I18NCat::CONTROLS, "None")) : ids);
		return;
	}

	page->Header("This game");
	page->Info("Game ID", status.discID);
	if (!status.profileFound) {
		page->Info("Rumble profile", T(I18NCat::CONTROLS, "Not found: rumble is off for this game"));
		page->Info("Put the profile here", status.profilePath);
		// The NRO carries the profiles made so far: copy one to the memory stick on request.
		std::string bundled = "rumble/" + status.discID + ".ini";
		size_t size = 0;
		uint8_t *data = g_VFS.ReadFile(bundled, &size);
		if (data) {
			delete[] data;
			std::string discID = status.discID;
			page->Choice("Install the bundled rumble profile").OnClick([host, discID](UI::EventParams &) {
				size_t size = 0;
				uint8_t *data = g_VFS.ReadFile("rumble/" + discID + ".ini", &size);
				if (!data) {
					return;
				}
				Path dir(Rumble::RumbleDirectory());
				File::CreateFullPath(dir);
				bool ok = File::WriteDataToFile(true, data, size, dir / (discID + ".ini"));
				delete[] data;
				if (ok) {
					Rumble::ReloadProfile();
					g_OSD.Show(OSDType::MESSAGE_SUCCESS, T(I18NCat::CONTROLS, "Rumble profile installed. It loads when the game continues."), 4.0f);
				} else {
					g_OSD.Show(OSDType::MESSAGE_ERROR, T(I18NCat::CONTROLS, "Couldn't write the rumble profile"), 4.0f);
				}
			});
		}
	} else {
		page->Info("Rumble profile", status.profileName);
		if (!status.profileError.empty()) {
			page->Info("Profile problems", status.profileError);
		}
	}

	auto enabled = [] { return g_Config.bRumble && RumbleProfileLoaded(); };
	const SettingText noProfile(I18NCat::CONTROLS, "RumbleNeedsProfile", "Needs a rumble profile for this game");

	page->Custom("Rumble strength for this game", [host](UI::ViewGroup *parent) -> UI::View * {
		static const char *names[] = { "Use the global setting", "None", "Light", "Medium", "Strong", "Hard" };
		auto co = GetI18NCategory(I18NCat::CONTROLS);
		PopupMultiChoice *choice = parent->Add(new PopupMultiChoice(&g_rumbleStrengthChoice, co->T("Rumble strength for this game"), names, 0, ARRAY_SIZE(names), I18NCat::CONTROLS, host->PageScreenManager()));
		choice->OnChoice.Add([](UI::EventParams &) {
			SaveRumbleGame();
		});
		return choice;
	}).EnabledFunc(enabled).DisabledReason(noProfile);

	page->IntSlider(&g_rumbleSensitivity, 0, 10, "Sound rumble sensitivity").Default(status.profileFound ? Rumble::ProfileSoundSensitivity() : 5)
		.Setup([](UI::View *view) {
			static_cast<PopupSliderChoice *>(view)->SetZeroLabel(T(I18NCat::CONTROLS, "Off"));
		})
		.OnChange([](UI::EventParams &) {
			SaveRumbleGame();
		})
		.EnabledFunc([status] { return g_Config.bRumble && RumbleProfileLoaded() && status.hasSound; }).DisabledReason(noProfile)
		.Hint({ I18NCat::CONTROLS, "RumbleSensitivityHint", "Rumble from loud, sudden low sounds (impacts, explosions). Higher: more of them" });

	for (size_t i = 0; i < status.rules.size(); i++) {
		const std::string name = status.rules[i].name;
		page->Bool(SettingText::Raw(KeepText(status.rules[i].label)), [name]() {
			for (const auto &r : g_rumbleGame.rules) {
				if (r.first == name) {
					return r.second;
				}
			}
			return true;
		}, [name](bool on) {
			for (auto &r : g_rumbleGame.rules) {
				if (r.first == name) {
					r.second = on;
				}
			}
			SaveRumbleGame();
		}).EnabledFunc(enabled);
	}
	if (status.profileFound) {
		page->Choice("Test: hit taken").OnClick([](UI::EventParams &) { Rumble::TestPulse(0); }).EnabledFunc(enabled);
		page->Choice("Test: blow landed").OnClick([](UI::EventParams &) { Rumble::TestPulse(1); }).EnabledFunc(enabled);
		page->Choice("Test: impact sound").OnClick([](UI::EventParams &) { Rumble::TestPulse(2); }).EnabledFunc(enabled);
	}

	page->Header("Profile tools");
	page->Description({ I18NCat::CONTROLS, "RumbleToolsAbout", "For making rumble profiles: RAM snapshots (PSP/RUMBLE/snapshots) to compare on a PC. See docs/rumble.md." });
	page->Choice("Save RAM snapshot...").OnClick([host](UI::EventParams &) {
		System_InputBoxGetString(host->PageRequesterToken(), T(I18NCat::CONTROLS, "Snapshot label (what is happening now)"), "", false, [](std::string_view label, int) {
			std::string file = Rumble::SaveSnapshot(std::string(label));
			if (file.empty()) {
				g_OSD.Show(OSDType::MESSAGE_ERROR, T(I18NCat::CONTROLS, "Couldn't save the snapshot"), 3.0f);
			} else {
				g_OSD.Show(OSDType::MESSAGE_SUCCESS, T(I18NCat::CONTROLS, "Snapshot saved"), file, 4.0f);
			}
		});
	}).EnabledFunc([] { return Rumble::GetStatus().inGame; });
	page->Choice("Reload rumble profile").OnClick([](UI::EventParams &) {
		Rumble::ReloadProfile();
		g_OSD.Show(OSDType::MESSAGE_INFO, T(I18NCat::CONTROLS, "The rumble profile reloads when the game continues"), 3.0f);
	});
}

void BuildControlsSettings(SettingsPage *page) {
	using namespace UI;
	SettingsPageHost *host = page->Host();

	int deviceType = System_GetPropertyInt(SYSPROP_DEVICE_TYPE);

	page->Header({ I18NCat::MAINSETTINGS, "Controls" });
	page->Choice("Control mapping").OpensScreen().OnClick([host](UI::EventParams &e) {
		host->PageScreenManager()->push(new ControlMappingScreen(host->PageGamePath()));
	});
	page->Choice("Calibrate analog stick").OpensScreen().OnClick([host](UI::EventParams &e) {
		host->PageScreenManager()->push(new AnalogCalibrationScreen(host->PageGamePath()));
	});
	page->Link("Rumble", I18NCat::CONTROLS, &BuildRumbleSettings);

#if (PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)) || (PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID))
	page->Setting(&g_Config.bSystemControls);
#endif
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
	page->Setting(&g_Config.bGamepadOnlyFocused);
#endif

	if (System_GetPropertyBool(SYSPROP_HAS_ACCELEROMETER)) {
		page->Custom("Tilt control setup", [host](UI::ViewGroup *parent) -> UI::View * {
			// Show the tilt type on the item.
			Choice *customizeTilt = parent->Add(new ChoiceWithCallbackValueDisplay(T(I18NCat::CONTROLS, "Tilt control setup"), []() -> std::string {
				if (g_Config.bTiltInputEnabled && (u32)g_Config.iTiltInputType < (u32)g_numTiltTypes) {
					auto co = GetI18NCategory(I18NCat::CONTROLS);
					return std::string(co->T(g_tiltTypes[g_Config.iTiltInputType]));
				} else {
					auto di = GetI18NCategory(I18NCat::DIALOG);
					return std::string(di->T("Disabled"));
				}
			}));
			customizeTilt->OnClick.Add([host](UI::EventParams &e) {
				host->PageScreenManager()->push(new TiltAnalogSettingsScreen(host->PageGamePath()));
			});
			return customizeTilt;
		}).OpensScreen();
	} else if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_VR) {  // TODO: This seems like a regression
		page->Setting(&g_Config.bHapticFeedback);
	}

	page->Header("Analog to digital mapping");
#if PPSSPP_PLATFORM(SWITCH)
	page->Bool(&g_Config.bRightStickFaceButtons, "Right stick controls face buttons").OnChange([](UI::EventParams &) {
		if (joystick) {
			joystick->releaseRightStickFaceButtons();
		}
		g_controlMapper.ReleaseAll();
	}).Hint("Up: Triangle, Down: Cross, Left: Square, Right: Circle");
#endif
	page->Setting(&g_Config.fAnalogTriggerThreshold);
	page->Setting(&g_Config.fAnalogStickThreshold);

	// TVs don't have touch control, at least not yet.
	if ((deviceType != DEVICE_TYPE_TV) && (deviceType != DEVICE_TYPE_VR)) {
		page->Header("On-screen touch controls");
		page->Setting(&g_Config.bShowTouchControls);
		page->Choice("Edit touch control layout").OpensScreen().OnClick([host](UI::EventParams &e) {
			host->PageScreenManager()->push(new TouchControlLayoutScreen(host->PageGamePath()));
		}).Enabled(&g_Config.bShowTouchControls);
		page->Choice("Gesture mapping").OpensScreen().OnClick([host](UI::EventParams &e) {
			host->PageScreenManager()->push(new GestureMappingScreen(host->PageGamePath()));
		}).Enabled(&g_Config.bShowTouchControls);

		page->Setting(&g_Config.iTouchButtonStyle).Enabled(&g_Config.bShowTouchControls);

		page->Setting(&g_Config.iTouchButtonOpacity).Enabled(&g_Config.bShowTouchControls);
		page->Setting(&g_Config.iTouchButtonHideSeconds).Enabled(&g_Config.bShowTouchControls);

		page->Setting(&g_Config.bTouchGliding).Enabled(&g_Config.bShowTouchControls);

		TouchControlConfig &touch = g_Config.GetTouchControlsConfig(g_display.GetDeviceOrientation());

		// Hide stick background, useful when increasing the size
		page->Setting(&touch.bHideStickBackground).Enabled(&g_Config.bShowTouchControls);

		// Sticky D-pad.
		page->Setting(&g_Config.bStickyTouchDPad).Enabled(&g_Config.bShowTouchControls).Hint("Easier sweeping movements");

		// Re-centers itself to the touch location on touch-down.
		page->Setting(&g_Config.bAutoCenterTouchAnalog).Enabled(&g_Config.bShowTouchControls);

		if (System_GetPropertyInt(SYSPROP_DEVICE_TYPE) == DEVICE_TYPE_MOBILE) {
			page->Setting(&g_Config.bHapticFeedback);
		}

		// The pause button is now a regular on-screen button.

		page->Setting(&g_Config.bDisableDpadDiagonals).Enabled(&g_Config.bShowTouchControls);
	}

#if PPSSPP_PLATFORM(WINDOWS)
	page->Header("Control input sources");
	page->Setting(&g_Config.bAllowHIDInput);
	page->Setting(&g_Config.bAllowXInput);
	page->Setting(&g_Config.bAllowDInput);
#endif

	if (deviceType != DEVICE_TYPE_VR) {
		page->Header({ I18NCat::CONTROLS, "Keyboard", "Keyboard Control Settings" });
#if PPSSPP_PLATFORM(WINDOWS) && !PPSSPP_PLATFORM(UWP)
		page->Setting(&g_Config.bIgnoreWindowsKey);
#endif
		page->Setting(&g_Config.fAnalogLimiterDeadzone).Hint({ I18NCat::CONTROLS, "AnalogLimiter Tip", "When the analog limiter button is pressed" });
		page->Setting(&g_Config.iRapidFireInterval);
#if PPSSPP_PLATFORM(WINDOWS) || defined(SDL) || PPSSPP_PLATFORM(ANDROID)
		bool enableMouseSettings = true;
#if PPSSPP_PLATFORM(ANDROID)
		if (System_GetPropertyInt(SYSPROP_SYSTEMVERSION) < 12) {
			enableMouseSettings = false;
		}
#endif
#else
		bool enableMouseSettings = false;
#endif
		if (enableMouseSettings) {
			// The mousewheel button-release setting is independent of actual mouse delta control.
			page->Header({ I18NCat::CONTROLS, "Mouse", "Mouse settings" });
			page->Setting(&g_Config.iMouseWheelUpDelayMs);

			page->Setting(&g_Config.bMouseControl).Hint({ I18NCat::CONTROLS, "MouseControl Tip", "You can now map mouse in control mapping screen by pressing the 'M' icon." });

#if !PPSSPP_PLATFORM(ANDROID)
			page->Setting(&g_Config.bMouseConfine).Enabled(&g_Config.bMouseControl);
#endif
			page->Setting(&g_Config.fMouseSensitivity).Enabled(&g_Config.bMouseControl).LiveUpdate();
			page->Setting(&g_Config.fMouseSmoothing).Enabled(&g_Config.bMouseControl).LiveUpdate();
		}
	}
}
