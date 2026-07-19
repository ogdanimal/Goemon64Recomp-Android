#include <atomic>
#include <cstdio>
#include <filesystem>

#include "ultramodern/ultramodern.hpp"
#include "goemon_save_rollback.h"

// The `.manual.bak` rollback point.
//
// WHY THIS EXISTS
// ---------------
// librecomp rotates `current -> .bak` on every flush
// (librecomp/src/files.cpp:39-45), and every autosave is a flush. That rotation
// exists for torn-write protection, which it still provides -- but it also used
// to mean, incidentally, that `.bak` held your last *manual* save, purely
// because flushes were rare. Autosave destroys that incidental property: after
// two autosaves, `.bak` is an autosave-of-an-autosave. So a save-class rollback
// point has to be maintained deliberately rather than relied on as a side
// effect. See docs/autosave.md.
//
// HOW IT DECIDES
// --------------
// librecomp reports every guest mutation of the save buffer and every completed
// flush; neither hook knows what an autosave is. The policy is entirely here:
//
//   a guest pak write that is NOT the autosave's own
//     => a deliberate, player-initiated save-class operation
//     => arm a one-shot; copy the save file to `.manual.bak` on the next flush.
//
// Anchoring on the *write* rather than on the game's marshal routine is what
// makes this cheap and safe. Every guest pak write already arrives here through
// the Controller Pak shim (librecomp/src/pak.cpp:60-77) with the guest's own
// offset and count, so no game function has to be replaced to observe it. It
// also means the RAM-only "suspend" -- which marshals save data but never
// touches the pak -- cannot arm the one-shot at all, because it never reaches
// this path.
//
// Correctness does NOT depend on having enumerated the game's pak writers. An
// unmapped deliberate writer arms the one-shot and gets its own state copied,
// which degrades the contract gracefully instead of violating it. The mapped
// set is a bonus, not a premise.
//
// ACCEPTED RESIDUAL RACE (seen and priced, not missed)
// ----------------------------------------------------
// librecomp fires the write hook BEFORE signalling the saving thread, so a
// flush can never race ahead of the arming it should observe. That makes the
// ordinary paths sound, but one window survives: if an autosave's writes land
// in the save buffer AFTER a deliberate write armed the one-shot but BEFORE
// that arming write's flush completes, the flush carries autosave content and
// the armed one-shot copies it.
//
// Not fixed, deliberately. Reaching it requires a deliberate save and an
// autosave inside roughly one flush latency (~10ms coalescing window), while
// the guest frame cadence is 16.7ms and the safety gate refuses to autosave
// during the dialogue a manual save requires. It needs frame-boundary alignment
// that is close to unconstructible in practice.
//
// Closing it properly would mean tagging the flush with the buffer generation
// that armed it, which is a librecomp-side change to carry weight this hazard
// does not have. If the rollback point is ever observed holding autosave
// content, this is the first thing to re-examine.
//
// CONTRACT
// --------
// `.manual.bak` holds the save file as of the last deliberate save-class
// operation -- NOT "the last manual save". The looser wording is the honest
// one: a Diary erase or copy is deliberate and is captured too. That is correct
// behaviour, since the erase was the player's own act.

namespace {

// Set and cleared on the guest thread across the whole of goemon_save_now();
// read on the guest thread from the write observer.
std::atomic<bool> autosave_in_progress{false};

// Armed on the guest thread by the write observer, consumed on the saving
// thread by the flush observer. This is the thread crossing, hence the atomic.
std::atomic<bool> rollback_armed{false};

constexpr const char* manual_backup_suffix = ".manual.bak";
constexpr const char* manual_backup_temp_suffix = ".manual.bak.tmp";

void on_save_write(uint32_t offset, uint32_t count) {
    (void)offset;
    (void)count;

    // The autosave's own traffic is not a rollback point. Note this must cover
    // the autosave's *entire* body, not just its slot write: the save routine
    // also pushes a 0x100-byte header block through this same path, and an
    // unbracketed header write would arm the one-shot and copy autosave content
    // into `.manual.bak` -- precisely the state the rollback point exists to
    // roll back from.
    if (autosave_in_progress.load(std::memory_order_relaxed)) {
        return;
    }

    rollback_armed.store(true, std::memory_order_relaxed);
}

void on_save_flush(const std::filesystem::path& path) {
    // One-shot: exchange so a single armed write produces a single copy, and so
    // subsequent autosave flushes cannot overwrite the rollback point.
    if (!rollback_armed.exchange(false, std::memory_order_relaxed)) {
        return;
    }

    std::filesystem::path backup_path{path};
    backup_path += manual_backup_suffix;

    std::filesystem::path temp_path{path};
    temp_path += manual_backup_temp_suffix;

    // Copy to a temp file and rename, rather than copying straight onto the
    // rollback point. A process kill midway through a direct copy would leave
    // `.manual.bak` truncated -- and a rollback point that is only trustworthy
    // when nothing went wrong is worthless, since "something went wrong" is the
    // entire circumstance it exists for. The rename is atomic within a
    // filesystem, so the rollback point is either the old complete file or the
    // new complete one, never a partial. Same standard librecomp already holds
    // the main save file to (temp-then-finalize, librecomp/src/files.cpp).
    //
    // Safe to copy from `path` here: this runs on the saving thread, which has
    // just finished finalizing that exact file.
    std::error_code ec;
    std::filesystem::copy_file(path, temp_path, std::filesystem::copy_options::overwrite_existing, ec);

    if (!ec) {
        std::filesystem::rename(temp_path, backup_path, ec);
    }

    if (ec) {
        // Non-fatal: the save itself already succeeded, only the rollback point
        // is missing. Report it rather than failing the save. Any temp left
        // behind by a failed copy is cleaned up so it cannot be mistaken for a
        // rollback point.
        std::error_code cleanup_ec;
        std::filesystem::remove(temp_path, cleanup_ec);

        fprintf(stderr, "[autosave] failed to write rollback copy %s: %s\n",
                backup_path.string().c_str(), ec.message().c_str());
    }
}

} // namespace

void goemon64::init_save_rollback() {
    // Deliberately unconditional -- NOT gated on the autosave setting. A player
    // who enables autosave and immediately triggers one would otherwise have no
    // pre-autosave rollback point, because nothing would have been armed before
    // the first autosave flush. Maintaining it always means the rollback point
    // already exists at the moment autosave is switched on.
    ultramodern::set_save_write_callback(on_save_write);
    ultramodern::set_save_flush_callback(on_save_flush);
}

void goemon64::set_autosave_in_progress(bool in_progress) {
    autosave_in_progress.store(in_progress, std::memory_order_relaxed);
}
