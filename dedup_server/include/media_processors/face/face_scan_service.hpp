#pragma once

#include "media_processors/face/face_detector.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace MediaDedup
{
    class DatabaseManager;
    class FaceClusterer;

    /**
     * @brief FACE-1 sweep: the trailing background job that detects, aligns
     * and embeds faces for processed images (docs/todo_face_recognition.md
     * §2). One runTick() = one bounded batch; the scheduler provides the
     * cadence, so the same code is the one-time library backfill and the
     * steady-state trailer for new files.
     *
     * Seams: the detector is the FaceDetector interface (Vision in the
     * server, fakes in tests) and the embedder is a function (ONNX in the
     * server) — CI never loads Vision or a model.
     *
     * File dispositions per tick:
     * - offline/cloud-evicted: skipped WITHOUT marking (retried when the
     *   volume returns), remembered in-memory so a stuck head of the
     *   selector cannot starve the batch.
     * - unreadable/undetectable: marked scanned with face_count NULL —
     *   attempted, answer unknown; never retried until a version bump.
     * - readable: faces stored (all of them; FACE-2's clustering applies
     *   the quality gate), marked scanned with the real count (0 is a real
     *   answer: scanned, no faces).
     */
    class FaceScanService
    {
    public:
        /// (nchw 3x112x112 floats) -> embedding floats; false on failure.
        using EmbedFn = std::function<bool(const std::vector<float> &, std::vector<float> &)>;

        struct Options
        {
            int batch = 200;       // files per tick
            int max_dim = 2048;    // decode cap (longest side)
            int embedding_dim = 512;
        };

        struct TickStats
        {
            int scanned = 0;   // files marked this tick
            int faces = 0;     // faces stored this tick
            int offline = 0;   // skipped, not marked
            int unreadable = 0;
            int clustered = 0; // faces assigned to clusters this tick
            int merges = 0;    // consolidation merges this tick
            int purged = 0;    // FACE-8: misfits shed by a purge sweep this tick
            bool exhausted = false; // selector returned fewer than the batch
        };

        /// `clusterer` (FACE-2) is optional: without it, faces are stored
        /// unclustered and the catch-up path adopts them once it exists.
        FaceScanService(DatabaseManager *db, std::unique_ptr<FaceDetector> detector,
                        EmbedFn embed, Options options,
                        std::shared_ptr<FaceClusterer> clusterer = nullptr);

        /// Process one batch. Safe to call on a scheduler thread.
        TickStats runTick();

        /// FACE-8: request a one-shot purge pass over ALL clusters at
        /// `cutoff` on the next tick — the config-change adaptation path.
        /// Thread-safe (the config event fires on a gRPC thread; the
        /// clusterer is single-threaded by contract, so the value latches
        /// into an atomic the tick consumes). Detached faces re-enter the
        /// unclustered pool and the same sweep re-homes them.
        void requestPurgeSweep(double cutoff) { pending_purge_cutoff_.store(cutoff); }

        /// ArcFace preprocessing of an aligned 112x112 RGB crop:
        /// (v - 127.5) / 128, channel-planar RGB (NCHW). Exposed for tests.
        static std::vector<float> preprocessToNchw(const std::vector<std::uint8_t> &rgb112);

    private:
        DatabaseManager *db_;
        std::unique_ptr<FaceDetector> detector_;
        std::shared_ptr<FaceClusterer> clusterer_;
        long long tick_count_ = 0;
        EmbedFn embed_;
        Options options_;
        // Files skipped as offline this process lifetime — keeps the ordered
        // selector from feeding the same disconnected volume every tick.
        std::unordered_set<int> skipped_offline_;
        // FACE-8: cutoff latched by requestPurgeSweep; < 0 = nothing pending.
        std::atomic<double> pending_purge_cutoff_{-1.0};
    };
}
