// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include <switch.h>

#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Rumble/Rumble.h"
#include "SDL/SwitchRumble.h"

namespace {

std::atomic<bool> g_running{ false };
std::thread g_thread;

// The handheld (both Joy-Cons attached) and player 1 (Pro Controller, both Joy-Cons, or one Joy-Con).
struct Devices {
	HidVibrationDeviceHandle handles[2][2]{};
	int counts[2]{};
	int n = 0;
	u32 style = 0xFFFFFFFF;
	double checkedAt = -100.0;
};

void Setup(Devices &d, double now) {
	if (d.n > 0 && now - d.checkedAt < 1.0) {
		return;
	}
	d.checkedAt = now;
	const u32 style = hidGetNpadStyleSet(HidNpadIdType_No1);
	if (d.n > 0 && style == d.style) {
		return;
	}
	d.style = style;
	d.n = 0;
	memset(d.counts, 0, sizeof(d.counts));
	if (R_SUCCEEDED(hidInitializeVibrationDevices(d.handles[d.n], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
		d.counts[d.n++] = 2;
	}
	HidNpadStyleTag tag = (HidNpadStyleTag)0;
	if (style & HidNpadStyleTag_NpadFullKey) {
		tag = HidNpadStyleTag_NpadFullKey;
	} else if (style & HidNpadStyleTag_NpadJoyDual) {
		tag = HidNpadStyleTag_NpadJoyDual;
	} else if (style & HidNpadStyleTag_NpadJoyLeft) {
		tag = HidNpadStyleTag_NpadJoyLeft;
	} else if (style & HidNpadStyleTag_NpadJoyRight) {
		tag = HidNpadStyleTag_NpadJoyRight;
	}
	const bool single = tag == HidNpadStyleTag_NpadJoyLeft || tag == HidNpadStyleTag_NpadJoyRight;
	if (tag && R_SUCCEEDED(hidInitializeVibrationDevices(d.handles[d.n], single ? 1 : 2, HidNpadIdType_No1, tag))) {
		d.counts[d.n++] = single ? 1 : 2;
	}
	INFO_LOG(Log::System, "Switch rumble: %d device groups (player 1 style %08x)", d.n, style);
}

void Send(const Devices &d, const Rumble::MotorState &m) {
	HidVibrationValue v{};
	v.amp_low = m.ampLow;
	v.freq_low = m.freqLow;
	v.amp_high = m.ampHigh;
	v.freq_high = m.freqHigh;
	HidVibrationValue values[2] = { v, v };
	for (int i = 0; i < d.n; i++) {
		hidSendVibrationValues(d.handles[i], values, d.counts[i]);
	}
}

void Loop() {
	SetCurrentThreadName("SwitchRumble");
	Devices devices;
	Rumble::MotorState last;
	double lastSent = 0.0;
	bool lastZero = true;
	while (g_running) {
		const double now = time_now_d();
		Rumble::MotorState m = Rumble::Sample(now);
		// Nothing while another applet (the HOME menu) has the focus.
		if (appletGetFocusState() != AppletFocusState_InFocus) {
			m = Rumble::MotorState();
		}
		const bool zero = m.ampLow <= 0.0f && m.ampHigh <= 0.0f;
		const bool changed = fabsf(m.ampLow - last.ampLow) > 0.01f || fabsf(m.ampHigh - last.ampHigh) > 0.01f ||
			m.freqLow != last.freqLow || m.freqHigh != last.freqHigh;
		// While it rumbles, refresh now and then in case a controller missed a value.
		if (changed || (zero != lastZero) || (!zero && now - lastSent > 0.1)) {
			Setup(devices, now);
			Rumble::SetOutputAvailable(devices.n > 0);
			if (devices.n > 0) {
				Send(devices, m);
			}
			last = m;
			lastSent = now;
			lastZero = zero;
		} else if (now - devices.checkedAt > 2.0) {
			Setup(devices, now);
			Rumble::SetOutputAvailable(devices.n > 0);
		}
		// 200 Hz: HD Rumble's own rate is about that.
		svcSleepThread(5000000ULL);
	}
	Setup(devices, time_now_d());
	if (devices.n > 0) {
		Send(devices, Rumble::MotorState());
	}
}

}  // namespace

void SwitchRumble_Start() {
	if (g_running) {
		return;
	}
	Rumble::SetOutputAvailable(true);
	g_running = true;
	g_thread = std::thread(Loop);
}

void SwitchRumble_Stop() {
	if (!g_running) {
		return;
	}
	g_running = false;
	if (g_thread.joinable()) {
		g_thread.join();
	}
}
