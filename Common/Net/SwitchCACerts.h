// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// devkitPro's libcurl for the Switch uses mbedTLS and has no CA bundle, so with peer verification on,
// every HTTPS request fails ("Could not connect"). This hands curl the console's own trusted root
// certificates, read from the ssl service, so it stays as current as the firmware.

#pragma once

#include "ppsspp_config.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "Common/Data/Encoding/Base64.h"

// One DER certificate as PEM (64 characters a line, like OpenSSL writes it).
inline std::string CertificateDERToPEM(const uint8_t *der, size_t size) {
	const std::string b64 = Base64Encode(der, size);
	std::string pem = "-----BEGIN CERTIFICATE-----\n";
	for (size_t i = 0; i < b64.size(); i += 64) {
		pem.append(b64, i, 64);
		pem += '\n';
	}
	pem += "-----END CERTIFICATE-----\n";
	return pem;
}

#if PPSSPP_PLATFORM(SWITCH)

#include <mutex>
#include <vector>

#include <switch.h>
#include <curl/curl.h>

#include "Common/Log.h"

// The firmware's trusted roots as one PEM bundle. Read once; empty if the ssl service can't be used.
inline const std::string &SwitchSystemCAPem() {
	static std::once_flag once;
	static std::string pem;
	std::call_once(once, [] {
		if (R_FAILED(sslInitialize(1))) {
			ERROR_LOG(Log::IO, "HTTPS: ssl service unavailable, no CA certificates");
			return;
		}
		u32 ids[] = { (u32)SslCaCertificateId_All };
		u32 size = 0;
		u32 total = 0;
		std::vector<u8> buffer;
		if (R_SUCCEEDED(sslGetCertificateBufSize(ids, 1, &size)) && size > 0) {
			buffer.resize(size);
			if (R_FAILED(sslGetCertificates(buffer.data(), size, ids, 1, &total))) {
				total = 0;
			}
		}
		sslExit();
		const auto *infos = reinterpret_cast<const SslBuiltInCertificateInfo *>(buffer.data());
		int count = 0;
		for (u32 i = 0; i < total && (i + 1) * sizeof(SslBuiltInCertificateInfo) <= buffer.size(); i++) {
			const SslBuiltInCertificateInfo &info = infos[i];
			// cert_data was converted to a pointer into the buffer by libnx.
			const u8 *end = buffer.data() + buffer.size();
			if (info.status != SslTrustedCertStatus_EnabledTrusted || !info.cert_data || info.cert_size == 0 ||
				info.cert_data < buffer.data() || info.cert_data + info.cert_size > end) {
				continue;
			}
			pem += CertificateDERToPEM(info.cert_data, (size_t)info.cert_size);
			count++;
		}
		INFO_LOG(Log::IO, "HTTPS: %d trusted CA certificates from the system", count);
	});
	return pem;
}

// Call on every easy handle that verifies peers.
inline void SwitchCurlUseSystemCAs(CURL *curl) {
	const std::string &pem = SwitchSystemCAPem();
	if (pem.empty()) {
		return;
	}
	curl_blob blob{};
	blob.data = const_cast<char *>(pem.data());
	blob.len = pem.size();
	blob.flags = CURL_BLOB_NOCOPY;  // The bundle lives as long as the app.
	curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
}

#endif
