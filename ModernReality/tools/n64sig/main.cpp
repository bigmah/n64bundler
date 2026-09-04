// SPDX-License-Identifier: GPL-3.0-or-later
//
//   n64sig build <libultra.a> [more...] --out libultra.n64sig
//   n64sig list  <db.n64sig>
//
// `build` fingerprints every function in a MIPS ELF archive. The input is the
// `libultra*.a` a decompilation project ships -- those are the library
// binaries every commercial N64 game was linked against, so a fingerprint
// taken here matches the same function sitting inside any ROM.
//
// Nothing about this touches a game. An archive is Nintendo's library, not a
// game's code, and the database that comes out holds instruction patterns for
// library functions and their names.

#include "signature.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: n64sig build <archive.a|object.o> [more...] --out <db.n64sig>\n"
                 "       n64sig list <db.n64sig>\n");
}

int build(const std::vector<std::string> &inputs, const std::string &out_path) {
    n64sig::Database database;
    std::vector<std::string> warnings;

    for (const std::string &input : inputs) {
        const size_t before = database.size();
        std::string error;
        if (!n64sig::harvest(input, database, error, &warnings)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        std::printf("%-60s %5zu functions\n", input.c_str(), database.size() - before);
    }

    if (database.size() == 0) {
        std::fprintf(stderr, "error: nothing was fingerprinted; are these MIPS ELF objects?\n");
        return 1;
    }

    // A name appearing twice is normal -- the same function is in the debug and
    // the release build of the library -- but two different names on identical
    // code is not, and would let a match pick the wrong one.
    std::map<std::string, std::vector<const n64sig::Signature *>> by_bytes;
    size_t ambiguous = 0;
    for (const n64sig::Signature &signature : database.all()) {
        std::string key;
        key.reserve(signature.words.size() * 8);
        for (size_t i = 0; i < signature.words.size(); i++) {
            char buffer[20];
            std::snprintf(buffer, sizeof(buffer), "%08X%08X", signature.words[i], signature.masks[i]);
            key += buffer;
        }
        auto &bucket = by_bytes[key];
        for (const n64sig::Signature *other : bucket) {
            if (other->name != signature.name) {
                ambiguous++;
                break;
            }
        }
        bucket.push_back(&signature);
    }

    std::string error;
    if (!database.save(out_path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    std::printf("\n%zu signatures written to %s\n", database.size(), out_path.c_str());
    if (ambiguous > 0) {
        std::printf("%zu are byte-identical to a function with a different name; a match on "
                    "one of those names nothing\n",
                    ambiguous);
    }
    for (const std::string &warning : warnings) {
        std::fprintf(stderr, "warning: %s\n", warning.c_str());
    }
    return 0;
}

int list(const std::string &path) {
    n64sig::Database database;
    std::string error;
    if (!database.load(path, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    for (const n64sig::Signature &signature : database.all()) {
        std::printf("%-40s %5zu bytes  %s\n", signature.name.c_str(), signature.size_bytes(),
                    signature.source.c_str());
    }
    std::printf("\n%zu signatures\n", database.size());
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 2;
    }

    const std::string command = argv[1];

    if (command == "list") {
        return list(argv[2]);
    }

    if (command != "build") {
        std::fprintf(stderr, "unknown command: %s\n", command.c_str());
        usage();
        return 2;
    }

    std::vector<std::string> inputs;
    std::string out_path;
    for (int i = 2; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg.rfind("-", 0) == 0) {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return 2;
        } else {
            inputs.push_back(arg);
        }
    }

    if (inputs.empty() || out_path.empty()) {
        usage();
        return 2;
    }
    return build(inputs, out_path);
}
