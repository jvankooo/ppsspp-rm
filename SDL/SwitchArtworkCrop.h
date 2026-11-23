// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// Cropping launcher backgrounds to the 16:9 the Classic theme shows. SteamGridDB's heroes are
// mostly 1920 x 620 or 3840 x 1240, which a full-screen fill would cut down to their middle.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

struct ArtworkRect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
};

// The largest 16:9 frame that fits in a width x height image, centred.
inline ArtworkRect LargestFrame16x9(int width, int height) {
	ArtworkRect frame;
	if ((int64_t)width * 9 >= (int64_t)height * 16) {
		frame.h = height;
		frame.w = std::min(width, (int)((int64_t)height * 16 / 9));
	} else {
		frame.w = width;
		frame.h = std::min(height, (int)((int64_t)width * 9 / 16));
	}
	frame.w = std::max(frame.w, 1);
	frame.h = std::max(frame.h, 1);
	frame.x = (width - frame.w) / 2;
	frame.y = (height - frame.h) / 2;
	return frame;
}

// Copies crop out of an RGBA image, shrunk to at most maxWidth wide (each output pixel averages the
// source pixels it covers). Never enlarges.
inline std::vector<uint8_t> CropAndShrinkRGBA(const uint8_t *src, int srcWidth, int srcHeight, ArtworkRect crop, int maxWidth,
											   int *outWidth, int *outHeight) {
	crop.x = std::clamp(crop.x, 0, srcWidth - 1);
	crop.y = std::clamp(crop.y, 0, srcHeight - 1);
	crop.w = std::clamp(crop.w, 1, srcWidth - crop.x);
	crop.h = std::clamp(crop.h, 1, srcHeight - crop.y);
	const int width = std::min(crop.w, maxWidth);
	const int height = std::max(1, (int)((int64_t)crop.h * width / crop.w));
	std::vector<uint8_t> out((size_t)width * height * 4);
	for (int j = 0; j < height; j++) {
		const int y0 = crop.y + (int)((int64_t)j * crop.h / height);
		const int y1 = std::max(y0 + 1, crop.y + (int)((int64_t)(j + 1) * crop.h / height));
		for (int i = 0; i < width; i++) {
			const int x0 = crop.x + (int)((int64_t)i * crop.w / width);
			const int x1 = std::max(x0 + 1, crop.x + (int)((int64_t)(i + 1) * crop.w / width));
			uint32_t sum[4] = {};
			for (int y = y0; y < y1; y++) {
				const uint8_t *row = src + ((size_t)y * srcWidth + x0) * 4;
				for (int x = x0; x < x1; x++, row += 4) {
					sum[0] += row[0];
					sum[1] += row[1];
					sum[2] += row[2];
					sum[3] += row[3];
				}
			}
			const uint32_t count = (uint32_t)((x1 - x0) * (y1 - y0));
			uint8_t *dst = &out[((size_t)j * width + i) * 4];
			for (int c = 0; c < 4; c++) {
				dst[c] = (uint8_t)((sum[c] + count / 2) / count);
			}
		}
	}
	*outWidth = width;
	*outHeight = height;
	return out;
}
