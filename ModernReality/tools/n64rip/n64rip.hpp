// SPDX-License-Identifier: GPL-3.0-or-later
//
// n64rip - recover from a bare N64 ROM the metadata N64Recomp needs.
//
// A GameCube DOL carries a section table, so DolRecomp is told where code
// lives and at what address. An N64 ROM is a flat image: a 64-byte header,
// 4KB of boot code, and then whatever the game's linker script decided. The
// recompiler cannot start from that, which is why every existing N64Recomp
// project starts from a decompilation's elf instead.
//
// This tool closes that gap. It recovers sections and function boundaries from
// the image itself and writes them as the symbol toml N64Recomp accepts
// alongside a raw ROM, so a ROM nobody has decompiled can still be recompiled.
//
// What it can and cannot do is in PLAN.md; the short version is that the boot
// segment comes out well and overlays are found only when the game builds the
// DMA table libultra games conventionally build.

#ifndef N64RIP_HPP
#define N64RIP_HPP

#include "signature.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace n64rip {

// ---------------------------------------------------------------------------
// The ROM

/// How the bytes were laid out in the file we were handed. Everything past
/// load() works on a normalised big-endian image.
enum class ByteOrder {
    Z64, // big endian, the native order
    N64, // little endian, every word reversed
    V64, // byte-swapped halfwords, the Doctor V64 dump order
};

const char *byte_order_name(ByteOrder order);

struct Header {
    uint32_t pi_config = 0;
    uint32_t clock_rate = 0;
    uint32_t entrypoint = 0; // as stored; see Rom::load_address
    uint32_t release = 0;
    uint32_t crc1 = 0;
    uint32_t crc2 = 0;
    std::string internal_name;  // 0x20..0x33, trailing space stripped
    std::string game_id;        // media + cartridge + country, e.g. "NSME"
    char media_format = 0;
    char country = 0;
    uint8_t version = 0;
};

/// The boot chip a ROM was mastered for. Its only consequence here is the
/// entry point: IPL3 for two of them adjusts the address it jumps to, so the
/// header value is not where the boot segment actually lands.
enum class Cic {
    Unknown,
    Cic6101,
    Cic6102, // by far the most common
    Cic6103,
    Cic6105,
    Cic6106,
    Cic7102, // 6102's PAL sibling
};

const char *cic_name(Cic cic);

/// How much IPL3 copies out of the ROM before it jumps to the entry point.
constexpr uint32_t kBootRomOffset = 0x1000;
constexpr uint32_t kBootCopySize = 0x100000;

struct Rom {
    std::vector<uint8_t> data; // normalised to big-endian
    ByteOrder original = ByteOrder::Z64;
    Header header;
    Cic cic = Cic::Unknown;
    /// Where the boot segment really lands, once IPL3's adjustment is applied.
    uint32_t load_address = 0;
    /// Whether load_address came from a confident reading or a guess that
    /// scored best; see resolve_load_address().
    bool load_address_verified = false;
    uint64_t hash = 0; // of the normalised image

    /// Big-endian word at a ROM offset. Out of range reads as zero.
    uint32_t word(size_t offset) const;
    size_t size() const { return data.size(); }
};

/// Read a ROM off disk, normalise its byte order, and parse everything the
/// header and boot code can tell us. Returns nullopt with `error` set if the
/// file is not an N64 image.
std::optional<Rom> load_rom(const std::string &path, std::string &error);

// ---------------------------------------------------------------------------
// Analysis

struct FunctionRange {
    uint32_t vram = 0;
    uint32_t size = 0; // bytes, always a multiple of 4
    /// The libultra name, when a signature matched here. Empty means the
    /// function gets named after its address, because nothing in the image
    /// says what it was called.
    std::string name;
    /// What a signature said this function is, when the name could not be
    /// used. The recompiler substitutes on a name it recognises, so a name the
    /// runtime does not implement has to be withheld or the build fails to
    /// link -- but knowing it is exactly what someone needs to judge whether
    /// stubbing this function costs the game anything, so it is kept for the
    /// report and never emitted.
    std::string known_as;
    /// Whether this function drives coprocessor 0 in ways the recompiler
    /// cannot translate, and has to be stubbed. See needs_stub() for why that
    /// is the right answer and when it stops being needed.
    bool stub = false;
};

struct SectionInfo {
    std::string name;
    uint32_t rom = 0;
    uint32_t vram = 0;
    uint32_t size = 0;
    /// Where the recovered code ends inside this section. Everything past it
    /// is rodata and data, which the recompiler has no use for.
    uint32_t text_size = 0;
    /// Whether a title record supplied this section rather than the analysis
    /// finding it. It changes one thing: a call from proven code to somewhere
    /// past the end of the sweep is followed, because the record has asserted
    /// that the whole range is one segment and a `jal` across a block of
    /// rodata inside it is still a call. Without a record saying so there is
    /// nothing to distinguish that from a call off the end of the section.
    bool from_record = false;

    std::vector<FunctionRange> functions;
};

/// Counters worth printing, and worth keeping in a title record so a change to
/// the analyser can be measured rather than guessed at.
struct AnalysisReport {
    size_t words_scanned = 0;
    size_t functions_found = 0;
    size_t from_calls = 0;     // reached by following calls from the entry point
    size_t from_sweep = 0;     // found by sweeping the code those calls proved
    size_t calls_outside = 0;  // `jal` targets no section covers
    size_t invalid_words = 0;  // words inside .text that did not decode

    /// The check that says whether the boundaries are right.
    ///
    /// Every `jal` names the first instruction of a function. So each one whose
    /// target is inside a section we recovered should land exactly on a
    /// function we recovered. One that lands in the middle of a function is a
    /// boundary this analysis got wrong, and the ratio between the two is the
    /// closest thing to a score available without a decompilation to compare
    /// against.
    size_t calls_on_boundary = 0;
    size_t calls_off_boundary = 0;

    /// Functions a libultra signature named. These are the ones that matter
    /// most: the recompiler substitutes the runtime's own implementation for
    /// each name it recognises, so naming them is what keeps libultra's
    /// hardware code from being translated and run.
    size_t named_functions = 0;
    /// Signature matches that landed somewhere the walk had not identified as
    /// a function at all.
    size_t named_new_boundaries = 0;
    /// Functions named by their shape rather than by a signature: the libultra
    /// accessors that are three instructions long and so cannot be
    /// fingerprinted, but are decided by the register they read.
    size_t named_by_shape = 0;
    /// Functions stubbed because they drive hardware -- coprocessor 0, or the
    /// RCP's registers -- and no signature named them. Each one is a place a
    /// wider signature database would do better: with a name, the runtime's
    /// own implementation stands in and the game keeps the behaviour instead
    /// of losing it.
    size_t stubbed_functions = 0;
    /// Signature matches whose name was thrown away because the runtime has no
    /// implementation to put in the function's place. These are recompiled
    /// from the ROM like any other function.
    size_t names_without_implementations = 0;
    /// Boundaries created because a call landed in the middle of a function.
    /// Each one is two functions the sweep ran together.
    size_t split_boundaries = 0;
    /// Boundaries dissolved because a branch crossed them. Each one was a
    /// switch statement whose cases the walk mistook for separate functions.
    size_t merged_boundaries = 0;
    /// Functions stubbed because a branch still leaves them after merging.
    /// These are hand-written assembly with several entry points sharing a
    /// body -- libultra's exception preamble is the one every game has -- and
    /// they do not divide into functions at all.
    size_t stubbed_unstructured = 0;
    /// Functions that nearly match a libultra signature without matching it.
    /// Each one is a routine the runtime could implement, in a revision of
    /// libultra this database does not carry -- a name, an address, and how
    /// much of it agreed, which is a line somebody can put in a title record.
    struct Resemblance {
        uint32_t vram = 0;
        std::string name;
        double share = 0.0;
        bool implemented = false;
    };
    std::vector<Resemblance> resemblances;
    /// Functions given their body back after a boundary cut them off from it.
    /// Each one is an entry point into a block that a later entry point also
    /// starts in, so the two overlap and each carries its own copy of the
    /// shared tail. Without this they would be counted above and stubbed.
    size_t extended_over_shared_tail = 0;
    /// Two-instruction functions cut out of a game's syscall stub table. Zero
    /// for a game that has no such table, which is nearly all of them.
    size_t syscall_stubs = 0;

    /// Functions that were stubbed even though a signature knew what they are,
    /// because the runtime has no implementation to put in their place. Each
    /// one is behaviour the game had and no longer has.
    std::vector<std::string> stubbed_by_name;

    std::vector<std::string> notes;
};

/// Which save chip the cartridge had. The names are librecomp's, because they
/// end up in the module as a `recomp::SaveType`.
///
/// Nothing in a ROM states this. What the image does show is which libultra
/// save routines are linked into it, and that narrows it to a family --
/// EEPROM, FlashRAM, SRAM -- but not to a size. `AllowAll` is the honest
/// answer for a ROM nobody has written a record for: it lets every save path
/// work and reports the larger EEPROM, which is right for more games than
/// either specific guess would be.
enum class SaveType {
    None,
    Eep4k,
    Eep16k,
    Sram,
    Flashram,
    AllowAll,
};

const char *save_type_name(SaveType type);
bool save_type_from_name(const std::string &name, SaveType &out);

/// A block of RSP microcode inside the ROM.
///
/// The signal processor runs its own instruction set, and a game hands the
/// task a pointer to the microcode it wants run. ultramodern intercepts the
/// graphics task and draws it with the renderer, so the only microcode that
/// has to be translated is whatever else the game submits -- the audio list,
/// in practice, which is what a silent game is missing.
///
/// Nothing in a ROM says where its microcode is, and unlike a code segment
/// there is no call graph to follow into one: the CPU never executes a word of
/// it. What the image does show is that RSP code is the only code in a
/// cartridge that uses coprocessor 2 -- the R4300 has none -- so a block of it
/// is visible by shape even when its address is not. That is enough to check a
/// record's numbers and not enough to find them, so they are recorded, and the
/// host prints the line to record after the first task it could not run.
struct MicrocodeInfo {
    /// What to call the recompiled function. Not a claim about which microcode
    /// this is; the name is for reading the generated code.
    std::string name;
    uint32_t rom = 0;
    uint32_t size = 0;
    /// Where the game loads it, which is how the module picks it at runtime:
    /// the task names its microcode by address.
    uint32_t vram = 0;
    /// Where the text lands in the signal processor's instruction memory.
    /// Every libultra task loads its microcode 0x80 bytes into IMEM, past the
    /// boot microcode that put it there.
    uint32_t text_address = 0x04001080;
    /// The microcode's data blob, which the boot microcode copies into data
    /// memory before the text runs. Optional, and worth recording because it
    /// is where a microcode keeps the table it dispatches its commands
    /// through: see harvest_branch_targets().
    uint32_t data_rom = 0;
    uint32_t data_size = 0;
    /// Where the data blob lives in console memory, for the same reason `vram`
    /// exists: a cartridge that unpacks itself has no rom offset to record.
    uint32_t data_vram = 0;
    /// Addresses the text jumps to through a register. The recompiler has to
    /// know them: it turns an indirect jump into a switch over the labels it
    /// emitted, and a target with no label is a microcode that stops.
    std::vector<uint32_t> branch_targets;
    /// How much of the block decodes as coprocessor 2, which is the evidence
    /// that this is microcode at all. Reported, not acted on.
    double cop2_density = 0.0;
};

struct Analysis {
    std::vector<SectionInfo> sections;
    std::vector<MicrocodeInfo> microcode;
    AnalysisReport report;
    /// The game's own syscall dispatch, if it has one -- the handler, and the
    /// table of stubs that reach it. Zero for every game that leaves the
    /// exception to libultra. See SyscallDispatch.
    uint32_t syscall_handler = 0;
    uint32_t syscall_stubs = 0;
    uint32_t syscall_stubs_size = 0;
    /// What the module tells the runtime to use.
    SaveType save_type = SaveType::AllowAll;
    /// The family the libultra names in the image point at, before the record
    /// or the AllowAll default has a say. Printed, not acted on.
    std::string save_type_evidence;
};

/// What the runtime can stand in for.
///
/// Naming a libultra function is only useful if something implements it. The
/// recompiler reacts to a name it recognises by not emitting the function's
/// body at all, on the understanding that the runtime supplies one -- so a name
/// the runtime does not implement turns a working translation into a link
/// error, which is a worse outcome than never having named it.
///
/// The set is not guessed. build.sh reads it out of the built runtime with
/// `nm`, so it is exactly the list of `<name>_recomp` symbols that exist.
using RuntimeProvides = std::unordered_set<std::string>;

/// Read that list, one name per line. Returns false with `error` set if the
/// file cannot be read.
bool load_runtime_provides(const std::string &path, RuntimeProvides &out, std::string &error);

// ---------------------------------------------------------------------------
// Title records
//
// What the analysis cannot recover from the image, written down once per game
// and checked into the repo. A record holds addresses, sizes and a hash --
// measurements of a cartridge, never bytes of one.

/// A segment that is not in the cartridge at all.
///
/// Some cartridges hold their game compressed and unpack it at boot. There is
/// nothing to recover from the image for those -- until the loader has run,
/// the code does not exist anywhere -- so the loader is recompiled on its own,
/// run once under `n64b-run --unpack`, and what it put in memory is written
/// out. This says which part of that image is the game.
///
/// The addresses are of console memory, not of the cartridge. A record can
/// hold them for the same reason it can hold a section's: they are two numbers
/// somebody measured, and the bytes they describe stay with the player's own
/// dump.
struct UnpackedSection {
    std::string name;
    uint32_t vram = 0;
    uint32_t size = 0;
};

/// A game that dispatches through the CPU's syscall exception.
///
/// A cartridge too big to hold in memory at once splits its code into overlays
/// and needs a way for one to call into another that may not be loaded. A game
/// can do that with a table of stubs and its own exception handler: every call
/// between overlays goes to a two-instruction stub -- `syscall <n>` and a word
/// that carries the rest of the index -- and the handler works out from the
/// address of the trapping instruction which function was wanted, loads the
/// overlay it lives in, and calls it.
///
/// Nothing in the image can be followed into that. The stubs are reached only
/// through tables of pointers in the game's own data, so no `jal` names one;
/// the handler is reached only through the exception vector, which is
/// hand-written assembly this analysis stubs; and the two are tied together by
/// a constant inside that assembly. So it is written down, and checked by
/// shape: every eight bytes of the recorded range has to be a `syscall`.
///
/// `handler` is the address libultra's exception preamble hands control to for
/// an exception of type Sys, with the trapping address in $t0 -- which is what
/// the game's own handler arranges by writing EPC before it returns.
struct SyscallDispatch {
    bool present = false;
    /// The first stub. Also the base the handler subtracts to get an index,
    /// so it is exact rather than approximate.
    uint32_t stubs = 0;
    /// How far the table runs. A multiple of eight.
    uint32_t size = 0;
    /// The game's handler, which the runtime calls in the exception's place.
    uint32_t handler = 0;
};

struct TitleRecord {
    /// Where it was read from, for the log line that says a record was used.
    std::string path;
    std::string game_id;
    /// Overrides the name derived from the ROM header.
    std::string display_name;
    /// Empty when the record does not pin one down.
    std::string save_type;
    /// The image the record was measured against. Zero means "any dump of this
    /// cartridge", which is the right default for anything but a bug workaround.
    uint64_t rom_hash = 0;
    /// Segments the analyser could not find, each with its own functions if the
    /// record lists them. Usually it does not, and the sweep fills them in.
    std::vector<SectionInfo> sections;
    /// Function boundaries the analyser got wrong, matched by vram. A size of
    /// zero deletes the boundary instead of correcting it.
    std::vector<FunctionRange> functions;
    /// Blocks of RSP microcode, which nothing in the image points at.
    std::vector<MicrocodeInfo> microcode;
    /// Segments the game's own loader unpacks into memory, which are in no
    /// part of the cartridge a reader can point at.
    std::vector<UnpackedSection> unpacked;
    /// The game's own syscall dispatch, for a game that has one.
    SyscallDispatch syscall;
    std::vector<std::string> notes;
};

/// Splice the segments a record calls unpacked onto the end of the ROM image.
///
/// `image` is console memory as `n64b-run --unpack` wrote it: eight megabytes
/// from 0x80000000, each 32-bit word in the host's order rather than the
/// cartridge's. Each segment is byte-swapped back and appended to `rom.data`,
/// and a section naming where it landed is added to `record.sections`, so that
/// everything downstream -- function recovery, the symbol file, the recompiler
/// reading the ROM -- treats it as though the cartridge had carried it all
/// along. The ROM's hash is left alone: it identifies the cartridge, and the
/// cartridge has not changed.
bool splice_unpacked(Rom &rom, TitleRecord &record, const std::vector<uint8_t> &image,
                     std::string &error);

/// Read a title record. Returns false with `error` set if the file is not
/// readable or not a record; a missing file is an error, since the caller only
/// asks for one it decided exists.
bool load_title_record(const std::string &path, TitleRecord &out, std::string &error);

/// What the game is called in the library and on the .app, derived from the
/// header's shouted internal name unless a record says otherwise.
std::string display_name(const Rom &rom, const TitleRecord *record = nullptr);

/// Recover sections and functions from a loaded ROM. With a signature database,
/// libultra functions are additionally named, which is what lets the recompiler
/// substitute the runtime's implementations for them. With a title record, what
/// the analysis could not find is filled in from it, and the record wins.
Analysis analyze(const Rom &rom, const n64sig::Database *signatures = nullptr,
                 const RuntimeProvides *provides = nullptr,
                 const TitleRecord *record = nullptr);

// ---------------------------------------------------------------------------
// Output

/// The symbol toml N64Recomp reads with `symbols_file_path`.
std::string emit_symbols_toml(const Rom &rom, const Analysis &analysis);

/// The recompiler configuration that points at it.
std::string emit_recomp_toml(const Rom &rom, const Analysis &analysis,
                             const std::string &symbols_path,
                             const std::string &rom_path,
                             const std::string &output_dir);

/// Everything the pipeline needs to know about the ROM, for the library entry
/// and for the cache key.
std::string emit_info_json(const Rom &rom, const Analysis &analysis,
                           const TitleRecord *record = nullptr);

} // namespace n64rip

#endif
