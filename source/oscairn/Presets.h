/*
 * Octet presets: the voices of DX7 cartridge files, through CLAP preset discovery.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * A preset is (CLAP_PRESET_DISCOVERY_LOCATION_FILE, a .syx cartridge, load key "0".."31"):
 * one voice of a 32-voice bulk dump. Locations: the built-in cartridges beside the plugin
 * (Octet/Cartridges next to the .clap) and the desktop Dexed's cartridge folder.
 */
#pragma once

#include <clap/factory/preset-discovery.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oscairn_fm {

// The 32 voices of a cartridge, unpacked to 155 bytes each.
struct Cartridge {
    uint8_t voices[32][155];
};

// Reads a cartridge from bytes or a file; false with a reason when it is not one.
bool parseCartridge(const uint8_t *data, size_t size, Cartridge &cart, std::string &error);
bool readCartridge(const char *path, Cartridge &cart, std::string &error);

// A voice's name as listed: its ten characters, made printable, trailing spaces removed.
std::string presetName(const uint8_t voice[155], int index);

// The sound of a preset: every parameter at its default except the voice's bytes (held to
// each parameter's range) and the operator switches (all on), so the Classic profile; values
// are by params() index. name gets the voice's ten name bytes.
bool readPreset(uint32_t kind, const char *location, const char *loadKey, std::vector<double> &values, uint8_t name[10],
                std::string &error);

// The path clap_entry.init was given, for the built-in cartridges beside the plugin.
void setPluginPath(const char *path);

const clap_preset_discovery_factory_t *presetDiscoveryFactory();

} // namespace oscairn_fm
