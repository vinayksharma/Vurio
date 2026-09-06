#pragma once

namespace MediaDedup
{
    /**
     * @brief Kill media subprocesses orphaned by a previous server run.
     *
     * The server enforces per-subprocess timeouts only while it is alive; a
     * server that dies mid-processing (crash, SIGKILL, Ctrl+C's _Exit)
     * orphans its in-flight magick children to PID 1, where a child wedged on
     * a vanished input spins at 100% CPU. run.sh/killall sweep these, but
     * only launches that go through those scripts benefit — the Swift UI
     * starts the binary directly. So the binary sweeps for itself at startup.
     *
     * Sweeps only when no other media_dedup_server process is running
     * anywhere (worktree instances included): with no server alive, any
     * process matching our app-specific argv markers is an orphan by
     * definition, never a healthy instance's in-flight child.
     */
    void sweepOrphanedMediaSubprocesses();

} // namespace MediaDedup
