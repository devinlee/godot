#pragma once
namespace MetonUiSupport {
inline constexpr unsigned LEASES = 16;
inline constexpr unsigned NODES = 128;
inline constexpr unsigned EVENTS = 256;
inline constexpr unsigned COORDINATE = 1048576;
inline constexpr unsigned FONT_SIZE = 1024;
inline constexpr unsigned TEXT_UNITS = 4096;
inline constexpr int PRIMARY_BYTES = 64100;
inline constexpr const char *PRIMARY_SHA256 = "55809808749613ed9ef9ad4f61424a14d2aa265d74369ad43403b9761aa6bac4";
inline constexpr int FALLBACK_BYTES = 1192544;
inline constexpr const char *FALLBACK_SHA256 = "b41f0f026055325a1c78c7f380e811ab0a5122d1c26a85ec1243f3c024c66002";
inline constexpr unsigned FALLBACK_SFNT_LIMIT = 4194304;
inline constexpr const char *FALLBACK_PATH = "res://meton_runtime/ui/DroidSansFallback.woff2";
}
