// SPDX-License-Identifier: GPL-3.0-or-later
//
// The handful of symbols the recompiled code expects its host to define.
//
// recomp.h declares a few functions that librecomp does not implement, on the
// understanding that whatever project links a recompiled game supplies them.
// A decompilation-based project writes them by hand for its one game; a
// bundler writes them once for every game it will ever open.

#include "host.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>

namespace {

/// The game's own handler for a syscall exception, or 0 if it has none.
///
/// See n64b_module_v1::syscall_handler_address for what one is and why the
/// runtime has to be told. Nearly every cartridge leaves this zero.
uint32_t syscall_handler = 0;
uint32_t syscall_table = 0;
uint32_t syscall_table_size = 0;

/// Whether this address is one of the game's overlay stubs.
///
/// A `syscall` anywhere else means the same thing it means for every other
/// game -- the recompiler translated something that was not code -- and
/// dispatching one would take a misread word and follow it into the game's
/// overlay loader, which is a much worse way to fail than saying so.
bool is_a_stub(uint32_t vram) {
    return syscall_table_size != 0 && vram >= syscall_table &&
           vram < syscall_table + syscall_table_size;
}

/// The register file as an array, which an interpreter has to index.
///
/// A recomp_context is thirty-two 64-bit general registers and then the
/// floating point ones, so the general file is the front of it -- the same
/// thing the trace buffer relies on to print a game's registers.
gpr *registers(recomp_context *ctx) {
    return reinterpret_cast<gpr *>(ctx);
}

void write_register(recomp_context *ctx, unsigned index, gpr value) {
    if (index != 0) {
        registers(ctx)[index] = value;
    }
}

/// A console address as the memory macros want it.
///
/// MEM_W and its family subtract the sign-extended base of KSEG0 from a
/// register, because that is how the recompiler keeps an address: sign
/// extended into sixty-four bits, the way a MIPS load leaves one. A bare
/// 32-bit value would index the mapping two gigabytes too high.
gpr console_address(uint32_t address) {
    return gpr(int32_t(address));
}

/// What the console has, and so what a generated instruction is allowed to
/// reach. The interpreter checks, because it is running words that were only
/// ever guessed to be code: a thunk this has misread would otherwise leave the
/// mapping and take the process down with no idea why.
bool addressable(gpr address) {
    return uint32_t(address) - 0x80000000u < 8u * 1024u * 1024u;
}

/// Run one instruction that is not a jump. False if it is one this does not
/// know, which is a thunk worth looking at rather than guessing about.
bool run_one(uint8_t *rdram, recomp_context *ctx, uint32_t instruction, uint32_t pc) {
    const unsigned op = instruction >> 26;
    const unsigned rs = (instruction >> 21) & 0x1F;
    const unsigned rt = (instruction >> 16) & 0x1F;
    const unsigned rd = (instruction >> 11) & 0x1F;
    const unsigned shift = (instruction >> 6) & 0x1F;
    const unsigned funct = instruction & 0x3F;
    const int32_t immediate = int16_t(instruction & 0xFFFF);
    gpr *reg = registers(ctx);

    switch (op) {
        case 0x00: // SPECIAL
            switch (funct) {
                case 0x00: // sll, which with every field zero is also `nop`
                    write_register(ctx, rd, gpr(int32_t(uint32_t(reg[rt]) << shift)));
                    return true;
                case 0x02: // srl
                    write_register(ctx, rd, gpr(int32_t(uint32_t(reg[rt]) >> shift)));
                    return true;
                case 0x03: // sra
                    write_register(ctx, rd, gpr(int32_t(reg[rt]) >> shift));
                    return true;
                case 0x04: // sllv
                    write_register(ctx, rd,
                                   gpr(int32_t(uint32_t(reg[rt]) << (uint32_t(reg[rs]) & 0x1F))));
                    return true;
                case 0x06: // srlv
                    write_register(ctx, rd,
                                   gpr(int32_t(uint32_t(reg[rt]) >> (uint32_t(reg[rs]) & 0x1F))));
                    return true;
                case 0x07: // srav
                    write_register(ctx, rd,
                                   gpr(int32_t(reg[rt]) >> (uint32_t(reg[rs]) & 0x1F)));
                    return true;
                case 0x20: // add
                case 0x21: // addu
                    write_register(ctx, rd, gpr(int32_t(uint32_t(reg[rs]) + uint32_t(reg[rt]))));
                    return true;
                case 0x22: // sub
                case 0x23: // subu
                    write_register(ctx, rd, gpr(int32_t(uint32_t(reg[rs]) - uint32_t(reg[rt]))));
                    return true;
                case 0x24: // and
                    write_register(ctx, rd, reg[rs] & reg[rt]);
                    return true;
                case 0x25: // or
                    write_register(ctx, rd, reg[rs] | reg[rt]);
                    return true;
                case 0x26: // xor
                    write_register(ctx, rd, reg[rs] ^ reg[rt]);
                    return true;
                case 0x27: // nor
                    write_register(ctx, rd, ~(reg[rs] | reg[rt]));
                    return true;
                case 0x2A: // slt
                    write_register(ctx, rd, gpr(int64_t(reg[rs]) < int64_t(reg[rt]) ? 1 : 0));
                    return true;
                case 0x2B: // sltu
                    write_register(ctx, rd, gpr(reg[rs] < reg[rt] ? 1 : 0));
                    return true;
                default:
                    break;
            }
            break;
        case 0x08: // addi
        case 0x09: // addiu
            write_register(ctx, rt, gpr(int32_t(uint32_t(reg[rs]) + uint32_t(immediate))));
            return true;
        case 0x0A: // slti
            write_register(ctx, rt, gpr(int64_t(reg[rs]) < int64_t(immediate) ? 1 : 0));
            return true;
        case 0x0B: // sltiu
            write_register(ctx, rt, gpr(reg[rs] < gpr(int64_t(immediate)) ? 1 : 0));
            return true;
        case 0x0C: // andi
            write_register(ctx, rt, reg[rs] & gpr(instruction & 0xFFFF));
            return true;
        case 0x0D: // ori
            write_register(ctx, rt, reg[rs] | gpr(instruction & 0xFFFF));
            return true;
        case 0x0E: // xori
            write_register(ctx, rt, reg[rs] ^ gpr(instruction & 0xFFFF));
            return true;
        case 0x0F: // lui
            write_register(ctx, rt, gpr(int32_t(instruction << 16)));
            return true;
        case 0x20: // lb
        case 0x21: // lh
        case 0x23: // lw
        case 0x24: // lbu
        case 0x25: // lhu
        case 0x28: // sb
        case 0x29: // sh
        case 0x2B: // sw
            if (!addressable(reg[rs] + gpr(int64_t(immediate)))) {
                std::fprintf(stderr,
                             "note: generated code at 0x%08X reached 0x%08X, which is not the "
                             "console's memory. This is not a thunk.\n",
                             pc, uint32_t(reg[rs] + gpr(int64_t(immediate))));
                return false;
            }
            switch (op) {
                case 0x20: write_register(ctx, rt, gpr(MEM_B(immediate, reg[rs]))); break;
                case 0x21: write_register(ctx, rt, gpr(MEM_H(immediate, reg[rs]))); break;
                case 0x23: write_register(ctx, rt, gpr(MEM_W(immediate, reg[rs]))); break;
                case 0x24: write_register(ctx, rt, gpr(MEM_BU(immediate, reg[rs]))); break;
                case 0x25: write_register(ctx, rt, gpr(MEM_HU(immediate, reg[rs]))); break;
                case 0x28: MEM_B(immediate, reg[rs]) = int8_t(reg[rt]); break;
                case 0x29: MEM_H(immediate, reg[rs]) = int16_t(reg[rt]); break;
                default:   MEM_W(immediate, reg[rs]) = int32_t(reg[rt]); break;
            }
            return true;
        default:
            break;
    }

    // Said once per instruction word, because a thunk that is reached again is
    // reached thousands of times and the first report is the useful one.
    static std::set<uint32_t> reported;
    if (reported.insert(instruction).second) {
        std::fprintf(stderr,
                     "note: the game generated an instruction this runtime does not interpret: "
                     "0x%08X at 0x%08X\n",
                     instruction, pc);
    }
    return false;
}

/// Run the few instructions a game wrote into memory for itself.
///
/// A static recompilation translates the code that is in the cartridge. Code a
/// game assembles while it runs is in no cartridge, so there is nothing to
/// translate and nothing to look up -- and a game whose overlay system builds a
/// thunk per call is doing exactly that. What a thunk holds is a handful of
/// instructions ending in a jump to code that *was* translated, so running them
/// here costs a few interpreted instructions per overlay call and needs no
/// assumption about what the game generates.
///
/// `jal` calls a translated function and comes back, which is how a thunk of
/// "call the function, then call the routine that puts the overlay back" runs
/// both. `j` and `jr` are the end of the thunk: control passes to translated
/// code and returns to whoever called in here. Delay slots are honoured,
/// because generated code uses them.
bool run_generated_code(uint8_t *rdram, recomp_context *ctx, uint32_t address,
                        uint32_t *first_target = nullptr) {
    // Longer than any thunk seen, short enough that one that loops is reported
    // rather than hanging the game.
    constexpr unsigned kInstructionBudget = 64;

    if (!addressable(console_address(address))) {
        std::fprintf(stderr,
                     "note: the stub jumped to 0x%08X, which is not the console's memory. That "
                     "word was not a thunk.\n",
                     address);
        return false;
    }

    uint32_t pc = address;
    for (unsigned step = 0; step < kInstructionBudget; step++) {
        const uint32_t instruction = uint32_t(MEM_W(0, console_address(pc)));
        const unsigned op = instruction >> 26;
        const unsigned funct = instruction & 0x3F;
        const bool absolute = op == 0x02 || op == 0x03;                   // j, jal
        const bool by_register = op == 0x00 && (funct == 0x08 || funct == 0x09); // jr, jalr
        if (!absolute && !by_register) {
            if (!run_one(rdram, ctx, instruction, pc)) {
                return false;
            }
            pc += 4;
            continue;
        }

        // A jump reads its target before its delay slot runs, and a linking
        // one writes the return address before it too.
        const unsigned rs = (instruction >> 21) & 0x1F;
        const uint32_t target = absolute
                                    ? ((pc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2))
                                    : uint32_t(registers(ctx)[rs]);
        const bool links = op == 0x03 || (by_register && funct == 0x09);
        if (links) {
            write_register(ctx, 31, gpr(int32_t(pc + 8)));
        }
        if (!run_one(rdram, ctx, uint32_t(MEM_W(4, console_address(pc))), pc + 4)) {
            return false;
        }
        if (!addressable(console_address(target))) {
            std::fprintf(stderr,
                         "note: generated code at 0x%08X jumped to 0x%08X, which is not the "
                         "console's memory.\n",
                         pc, target);
            return false;
        }
        if (first_target != nullptr && *first_target == 0) {
            *first_target = target;
        }
        LOOKUP_FUNC(target)(rdram, ctx);
        if (!links) {
            return true;
        }
        // A call comes back here only if the callee returned to where it was
        // called from, and $ra is what says so: a function that returns
        // normally leaves it as it found it, and one that takes a different
        // return address and jumps through it does not.
        //
        // The last call in a thunk is the second kind. The routine that puts
        // an overlay back reads the word after its own call site, makes that
        // its return address and goes there -- which is the caller of the
        // whole thing, and here is where this function returns to anyway. So
        // the thunk is over, and the word after the call is the address it
        // read rather than an instruction.
        if (uint32_t(ctx->r31) != pc + 8) {
            return true;
        }
        pc += 8;
    }

    std::fprintf(stderr,
                 "note: the code the game generated at 0x%08X reached no jump in %u "
                 "instructions.\n",
                 address, kInstructionBudget);
    return false;
}

} // namespace

void n64b::set_syscall_handler(uint32_t handler, uint32_t table, uint32_t table_size) {
    syscall_handler = handler;
    syscall_table = table;
    syscall_table_size = table_size;
}

/// A `syscall` instruction in the recompiled code.
///
/// The N64's own libultra uses it for nothing a game reaches -- the exception
/// handler is hand-written assembly this analysis stubs -- so for most games one
/// of these means the recompiler translated something that was not code, and
/// the address says where. Reporting it and carrying on is right: the game may
/// well never touch that path, and taking the process down would turn a
/// possible black frame into a certain crash.
///
/// A game that reaches between its overlays through the exception is the other
/// case, and there a syscall is the whole mechanism rather than a symptom. The
/// console's path is: the stub traps; libultra's preamble hands the exception
/// to the game's own handler with the address of the trapping instruction in
/// $t0; the handler loads the overlay the wanted function is in, rewrites the
/// stub into a jump to a thunk it has just assembled, and returns to the stub;
/// and the stub, now a jump, runs the thunk.
///
/// All of that happens here too, because all of it but the exception is
/// ordinary code the recompiler translated. What cannot happen is executing
/// the two things the game wrote after it was compiled -- the rewritten stub
/// and the thunk -- so those are read instead. The recursion ends for the same
/// reason it does on the console: the handler rewrites the stub before
/// returning to it, so the second time through, the word is a jump.
/// While this is non-zero the runtime will not take the thread away. The
/// console's own version is that an exception handler runs with interrupts
/// off, and the whole of what happens below is an exception handler.
extern "C" thread_local unsigned recomp_no_preempt;

namespace {
struct HoldTheThread {
    HoldTheThread() { recomp_no_preempt++; }
    ~HoldTheThread() { recomp_no_preempt--; }
};
} // namespace

extern "C" void recomp_syscall_handler(uint8_t *rdram, recomp_context *ctx,
                                       int32_t instruction_vram) {
    const HoldTheThread held;
    if (syscall_handler == 0 || !is_a_stub(uint32_t(instruction_vram))) {
        static std::set<uint32_t> reported;
        if (reported.size() < 16 && reported.insert(uint32_t(instruction_vram)).second) {
            std::fprintf(stderr,
                         "note: the game executed a syscall at 0x%08X, which is not one of its "
                         "overlay stubs. Nothing in libultra does that, so this is most likely "
                         "data the analysis read as code.\n",
                         uint32_t(instruction_vram));
        }
        return;
    }

    const uint32_t stub = uint32_t(instruction_vram);
    const uint32_t word = uint32_t(MEM_W(0, console_address(stub)));
    if ((word >> 26) != 0x02) { // still a syscall, so the overlay is not loaded
        ctx->r8 = gpr(instruction_vram);
        LOOKUP_FUNC(syscall_handler)(rdram, ctx);
        return;
    }

    // The stub is a jump now. Its delay slot is the stub's second word, which
    // is what tells the thunk which function of the overlay was asked for.
    const uint32_t thunk = (stub & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
    if (!run_one(rdram, ctx, uint32_t(MEM_W(4, console_address(stub))), stub + 4)) {
        return;
    }
    // What the call resolved to and what it gave back, when someone is
    // looking. Kept on the stack rather than in a static, because the function
    // a thunk calls very often makes an overlay call of its own.
    static const bool announce = std::getenv("N64B_OVERLAYS") != nullptr;
    uint32_t called = 0;
    const bool ran = run_generated_code(rdram, ctx, thunk, &called);
    if (announce) {
        std::fprintf(stderr,
                     "syscall: stub 0x%08X -> thunk 0x%08X -> 0x%08X, %s, v0 = 0x%08X\n", stub,
                     thunk, called, ran ? "ran" : "REFUSED", uint32_t(ctx->r2));
    }
    if (ran) {
        return;
    }

    // A thunk this could not run is the one thing worth showing in full: the
    // stub it came from, and the words the game put there. Said once.
    static bool shown = false;
    if (!shown && addressable(console_address(thunk))) {
        shown = true;
        std::fprintf(stderr, "note: stub 0x%08X (0x%08X 0x%08X) jumps to 0x%08X, which holds:\n",
                     stub, word, uint32_t(MEM_W(4, console_address(stub))), thunk);
        for (unsigned i = 0; i < 8; i++) {
            std::fprintf(stderr, "        %08X: %08X\n", thunk + i * 4,
                         uint32_t(MEM_W(int32_t(i * 4), console_address(thunk))));
        }
    }
}
