// SPDX-License-Identifier: GPL-3.0-or-later
// Indexing, matching, and storing function fingerprints.

#include "signature.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace n64sig {
namespace {

constexpr char kMagic[8] = {'N', '6', '4', 'S', 'I', 'G', '0', '1'};

void put32(std::ostream &out, uint32_t value) {
    const char bytes[4] = {char(value >> 24), char(value >> 16), char(value >> 8), char(value)};
    out.write(bytes, 4);
}

bool get32(std::istream &in, uint32_t &value) {
    unsigned char bytes[4];
    if (!in.read(reinterpret_cast<char *>(bytes), 4)) {
        return false;
    }
    value = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
            uint32_t(bytes[3]);
    return true;
}

} // namespace

uint64_t Database::key_for(const uint32_t *words, const uint32_t *masks, size_t count) {
    // FNV-1a over the leading words, each reduced to the bits its mask keeps.
    // Words the linker rewrote contribute their surviving bits and nothing
    // else, so a function whose first instruction is a relocated `lui` still
    // lands in a stable bucket.
    uint64_t hash = 0xCBF29CE484222325ull;
    const size_t used = std::min(count, kKeyWords);
    for (size_t i = 0; i < used; i++) {
        const uint32_t value = words[i] & masks[i];
        for (int byte = 3; byte >= 0; byte--) {
            hash ^= uint8_t(value >> (byte * 8));
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

void Database::add(Signature signature) {
    if (signature.words.size() < kMinimumSignatureWords ||
        signature.words.size() != signature.masks.size()) {
        return;
    }
    const uint64_t key =
        key_for(signature.words.data(), signature.masks.data(), signature.words.size());

    std::array<uint32_t, kKeyWords> pattern{};
    for (size_t i = 0; i < kKeyWords; i++) {
        pattern[i] = signature.masks[i];
    }
    if (std::find(masks_seen_.begin(), masks_seen_.end(), pattern) == masks_seen_.end()) {
        masks_seen_.push_back(pattern);
    }

    by_key_[key].push_back(signatures_.size());
    signatures_.push_back(std::move(signature));
}

std::vector<const Signature *> Database::candidates(const uint32_t *words, size_t count) const {
    std::vector<const Signature *> found;
    if (count < kKeyWords) {
        return found;
    }

    // The bucket key depends on the candidate's own masks, which we do not know
    // for arbitrary code. So try each distinct mask pattern the database uses
    // for its leading words. In practice there are a handful: all-bits, and the
    // few shapes a relocated prologue takes.
    for (const auto &entry : masks_seen_) {
        const uint64_t key = key_for(words, entry.data(), std::min(count, kKeyWords));
        auto bucket = by_key_.find(key);
        if (bucket == by_key_.end()) {
            continue;
        }
        for (size_t index : bucket->second) {
            found.push_back(&signatures_[index]);
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

bool Database::matches(const Signature &signature, const uint32_t *code, size_t available) {
    if (available < signature.words.size()) {
        return false;
    }
    for (size_t i = 0; i < signature.words.size(); i++) {
        if ((code[i] & signature.masks[i]) != signature.words[i]) {
            return false;
        }
    }
    return true;
}

double Database::resemblance(const Signature &signature, const uint32_t *code, size_t available,
                             size_t function_words) {
    const size_t theirs = signature.words.size();
    const size_t mine = std::min(function_words, available);
    if (theirs == 0 || mine == 0) {
        return 0.0;
    }
    // Lengths that are not in the same neighbourhood are not the same
    // function, whatever their instructions do; comparing them would fill the
    // report with a long routine that happens to start like a short one.
    const size_t longer = std::max(theirs, mine);
    const size_t shorter = std::min(theirs, mine);
    if (shorter * 4 < longer * 3) {
        return 0.0;
    }
    size_t agree = 0;
    for (size_t i = 0; i < shorter; i++) {
        if ((code[i] & signature.masks[i]) == signature.words[i]) {
            agree++;
        }
    }
    return double(agree) / double(longer);
}

bool Database::save(const std::string &path, std::string &error) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "could not write " + path;
        return false;
    }
    out.write(kMagic, sizeof(kMagic));
    put32(out, uint32_t(signatures_.size()));
    for (const Signature &signature : signatures_) {
        put32(out, uint32_t(signature.name.size()));
        out.write(signature.name.data(), std::streamsize(signature.name.size()));
        put32(out, uint32_t(signature.source.size()));
        out.write(signature.source.data(), std::streamsize(signature.source.size()));
        put32(out, uint32_t(signature.words.size()));
        for (size_t i = 0; i < signature.words.size(); i++) {
            put32(out, signature.words[i]);
            put32(out, signature.masks[i]);
        }
    }
    return out.good();
}

bool Database::load(const std::string &path, std::string &error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not open " + path;
        return false;
    }
    char magic[sizeof(kMagic)];
    if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
        error = path + " is not a signature database";
        return false;
    }
    uint32_t count = 0;
    if (!get32(in, count)) {
        error = path + " is truncated";
        return false;
    }

    signatures_.clear();
    by_key_.clear();
    masks_seen_.clear();
    signatures_.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        Signature signature;
        uint32_t length = 0;
        if (!get32(in, length)) {
            error = path + " is truncated";
            return false;
        }
        signature.name.resize(length);
        in.read(signature.name.data(), length);
        if (!get32(in, length)) {
            error = path + " is truncated";
            return false;
        }
        signature.source.resize(length);
        in.read(signature.source.data(), length);
        if (!get32(in, length)) {
            error = path + " is truncated";
            return false;
        }
        signature.words.resize(length);
        signature.masks.resize(length);
        for (uint32_t w = 0; w < length; w++) {
            if (!get32(in, signature.words[w]) || !get32(in, signature.masks[w])) {
                error = path + " is truncated";
                return false;
            }
        }
        add(std::move(signature));
    }
    return true;
}

} // namespace n64sig
