/*
 * Reading, writing and checking the per-case render hashes.
 *
 * Copyright (c) 2026 the Oscairn contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "dx7_cases.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

// --write FILE: render every case and write "name hash" lines.
// --check FILE: render every case and compare with FILE; --reverse renders in reverse order
// (a determinism check: state left behind by one case must not change the next).
template <typename Render>
int runBaseline(int argc, char **argv, Render render) {
    if (argc < 3 || (std::strcmp(argv[1], "--write") != 0 && std::strcmp(argv[1], "--check") != 0)) {
        std::fprintf(stderr, "usage: %s --write|--check FILE [--reverse]\n", argv[0]);
        return 2;
    }
    const bool write = std::strcmp(argv[1], "--write") == 0;
    const bool reverse = argc > 3 && std::strcmp(argv[3], "--reverse") == 0;
    auto all = dx7cases::cases();
    if (reverse)
        std::reverse(all.begin(), all.end());

    std::map<std::string, uint64_t> expected;
    if (!write) {
        std::ifstream in(argv[2]);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 2;
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream ls(line);
            std::string name, hex;
            ls >> name >> hex;
            expected[name] = std::strtoull(hex.c_str(), nullptr, 16);
        }
    }

    std::ostringstream out;
    out << "# dx7 render hashes: case, FNV-1a 64 of both output channels\n";
    int failures = 0;
    for (const auto &c : all) {
        const uint64_t h = render(c);
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016" PRIx64, h);
        out << c.name << " " << hex << "\n";
        if (!write) {
            auto it = expected.find(c.name);
            if (it == expected.end()) {
                std::fprintf(stderr, "MISSING %s\n", c.name.c_str());
                failures++;
            } else if (it->second != h) {
                std::fprintf(stderr, "DIFF %s: got %s, expected %016" PRIx64 "\n", c.name.c_str(), hex, it->second);
                failures++;
            }
        }
    }
    if (write) {
        std::ofstream f(argv[2]);
        f << out.str();
        std::printf("wrote %zu cases to %s\n", all.size(), argv[2]);
        return f ? 0 : 1;
    }
    std::printf("%zu cases, %d differ\n", all.size(), failures);
    return failures == 0 ? 0 : 1;
}
