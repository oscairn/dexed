/*
 * Octet presets: the voices of DX7 cartridge files, through CLAP preset discovery.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#include "Presets.h"
#include "Parameters.h"

#include <oscairn-clap/preset-discovery-plugin.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace oscairn_fm {

namespace {

constexpr size_t dumpSize = 4104; // F0, 5 header bytes, 32 x 128 packed bytes, checksum, F7
constexpr size_t maxFileSize = 65535; // as much as the desktop Dexed reads of a cartridge

// One packed voice to 155 bytes, as the desktop Dexed's Cartridge::unpackProgram (whose
// first 11 bytes of each operator end up copied unmasked).
void unpack(const uint8_t *bulk, uint8_t *ret) {
    for (int op = 0; op < 6; op++) {
        const uint8_t *b = bulk + op * 17;
        uint8_t *r = ret + op * 21;
        std::memcpy(r, b, 11);
        const uint8_t curves = b[11] & 0xF;
        r[11] = curves & 3;
        r[12] = (curves >> 2) & 3;
        const uint8_t detuneRs = b[12] & 0x7F;
        r[13] = detuneRs & 7;
        const uint8_t kvsAms = b[13] & 0x1F;
        r[14] = kvsAms & 3;
        r[15] = (kvsAms >> 2) & 7;
        r[16] = b[14] & 0x7F;
        const uint8_t coarseMode = b[15] & 0x3F;
        r[17] = coarseMode & 1;
        r[18] = (coarseMode >> 1) & 0x1F;
        r[19] = b[16] & 0x7F;
        r[20] = (detuneRs >> 3) & 0x7F;
    }
    auto norm = [](uint8_t v, int max) { return v <= max ? v : (uint8_t)((float)v / 255 * max); };
    for (int i = 0; i < 8; i++)
        ret[126 + i] = norm(bulk[102 + i] & 0x7F, 99);
    ret[134] = norm(bulk[110] & 0x1F, 31);
    const uint8_t oksFb = bulk[111] & 0xF;
    ret[135] = oksFb & 7;
    ret[136] = oksFb >> 3;
    for (int i = 0; i < 4; i++)
        ret[137 + i] = bulk[112 + i] & 0x7F;
    const uint8_t lfo = bulk[116] & 0x7F;
    ret[141] = lfo & 1;
    ret[142] = (lfo >> 1) & 7;
    ret[143] = lfo >> 4;
    ret[144] = bulk[117] & 0x7F;
    for (int i = 0; i < 10; i++)
        ret[145 + i] = bulk[118 + i] & 0x7F;
}

std::string &pluginPath() {
    static std::string path;
    return path;
}

bool isFolder(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

std::string env(const char *name) {
    const char *v = std::getenv(name);
    return v != nullptr ? v : "";
}

// The desktop Dexed's cartridge folder (source/PluginData.cpp, resolvAppDir).
std::string dexedCartridges() {
#if defined(_WIN32)
    const std::string base = env("APPDATA");
    return base.empty() ? "" : base + "\\DigitalSuburban\\Dexed\\Cartridges";
#elif defined(__APPLE__)
    const std::string home = env("HOME");
    return home.empty() ? "" : home + "/Library/Application Support/DigitalSuburban/Dexed/Cartridges";
#else
    std::string base = env("XDG_DATA_HOME");
    if (base.empty()) {
        const std::string home = env("HOME");
        if (home.empty())
            return "";
        base = home + "/.local/share";
    }
    return base + "/DigitalSuburban/Dexed/Cartridges";
#endif
}

// The built-in cartridges: Octet/Cartridges beside the plugin file.
std::string builtinCartridges() {
    const std::string &path = pluginPath();
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
        return "";
    return path.substr(0, slash + 1) + "Octet" + path[slash] + "Cartridges";
}

struct Presets {
    static const clap_preset_discovery_provider_descriptor_t descriptor;
    static const char *const pluginId;

    static void declare(oscairn::presets::Declarations &d) {
        const std::string builtin = builtinCartridges(), user = dexedCartridges();
        if (!builtin.empty() && isFolder(builtin))
            d.files("Built-in cartridges", builtin.c_str(), "syx");
        if (!user.empty() && isFolder(user))
            d.files("Dexed cartridges", user.c_str(), "syx", CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
    }

    static void list(uint32_t kind, const char *location, oscairn::presets::Listing &l) {
        if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE)
            return;
        Cartridge cart;
        std::string error;
        if (!readCartridge(location, cart, error)) {
            l.error(error.c_str());
            return;
        }
        char key[4];
        for (int i = 0; i < 32; i++) {
            std::snprintf(key, sizeof key, "%d", i);
            if (!l.add(presetName(cart.voices[i], i).c_str(), key))
                return;
        }
    }
};

const clap_preset_discovery_provider_descriptor_t Presets::descriptor = { CLAP_VERSION_INIT, "org.oscairn.fm.cartridges",
                                                                          "Octet cartridges", "Oscairn" };
const char *const Presets::pluginId = "org.oscairn.fm";

} // namespace

bool parseCartridge(const uint8_t *data, size_t size, Cartridge &cart, std::string &error) {
    // the first system exclusive message that is a 32-voice bulk dump
    for (size_t at = 0; at < size;) {
        if (data[at] != 0xF0) {
            error = "not a DX7 cartridge (no system exclusive message)";
            return false;
        }
        const uint8_t *end = (const uint8_t *)std::memchr(data + at, 0xF7, size - at);
        if (end == nullptr)
            break;
        const uint8_t *m = data + at;
        const size_t length = (size_t)(end - m) + 1;
        if (length == dumpSize && m[1] == 0x43 && (m[2] & 0xF0) == 0 && m[3] == 0x09 && m[4] == 0x20 && m[5] == 0x00) {
            // a checksum that does not match is accepted, as the desktop Dexed does
            for (int i = 0; i < 32; i++)
                unpack(m + 6 + i * 128, cart.voices[i]);
            return true;
        }
        at += length;
    }
    error = "not a DX7 cartridge (no 32-voice bulk dump in the file)";
    return false;
}

bool readCartridge(const char *path, Cartridge &cart, std::string &error) {
    FILE *f = path != nullptr ? std::fopen(path, "rb") : nullptr;
    if (f == nullptr) {
        error = "cannot open the cartridge file";
        return false;
    }
    std::vector<uint8_t> data(maxFileSize);
    data.resize(std::fread(data.data(), 1, data.size(), f));
    std::fclose(f);
    return parseCartridge(data.data(), data.size(), cart, error);
}

std::string presetName(const uint8_t voice[155], int index) {
    std::string name;
    for (int i = 0; i < 10; i++) {
        char c = (char)(voice[145 + i] & 0x7F);
        // as the desktop Dexed shows names (Program::normalizePgmName)
        c = c == 92 ? 'Y' : c == 126 ? '>' : c == 127 ? '<' : c < 32 ? ' ' : c;
        name += c;
    }
    name.erase(name.find_last_not_of(' ') + 1);
    return name.empty() ? "Voice " + std::to_string(index + 1) : name;
}

bool readPreset(uint32_t kind, const char *location, const char *loadKey, std::vector<double> &values, uint8_t name[10],
                std::string &error) {
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || location == nullptr) {
        error = "presets are voices of cartridge files";
        return false;
    }
    // "0".."31", nothing else
    const size_t n = loadKey != nullptr ? std::strlen(loadKey) : 0;
    if (n == 0 || n > 2 || !std::all_of(loadKey, loadKey + n, [](char c) { return c >= '0' && c <= '9'; }) ||
        (n == 2 && loadKey[0] == '0') || std::atoi(loadKey) > 31) {
        error = "load key must be a voice number from 0 to 31";
        return false;
    }
    Cartridge cart;
    if (!readCartridge(location, cart, error))
        return false;
    const uint8_t *v = cart.voices[std::atoi(loadKey)];
    values.resize(params().size());
    for (size_t i = 0; i < params().size(); i++) {
        const Param &p = params()[i];
        values[i] = p.kind == Kind::Byte ? std::clamp((double)v[p.target] - p.offset, p.min, p.max)
                    : p.kind == Kind::OpSwitch ? 1.0
                                               : p.def;
    }
    std::memcpy(name, v + 145, 10);
    return true;
}

void setPluginPath(const char *path) { pluginPath() = path != nullptr ? path : ""; }

const clap_preset_discovery_factory_t *presetDiscoveryFactory() { return &oscairn::presets::Factory<Presets>::factory; }

} // namespace oscairn_fm
