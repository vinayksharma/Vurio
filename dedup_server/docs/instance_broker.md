# BROKER-1: concurrent server+UI pairs and how they find each other

Several Vurio instances — one server + one UI each, typically one per
worktree/branch — run side by side on one machine. This note is the contract
between the pieces. Owner decisions (2026-09-06): broker embedded in the
server and elected by port; broker on TCP loopback, servers on their native
transport; a server is single-use and exits when its UI leaves; a UI retries
discovery for ~30 s before giving up with a reason.

## Pieces

| Piece | Where | Role |
|---|---|---|
| `broker.proto` | `src/api/` | Discovery contract. Independent of `dedup.proto`: broker and registrants are builds of *different* commits, so it must stay wire compatible — add fields only. |
| `InstanceBroker` | `core/broker/instance_broker.*` | One per server process. Elects, registers, heartbeats, hosts the registry when it is the broker. |
| `InstancePaths` | `core/instance_paths.hpp` | `~/.vurio/instances/<pid>/{grpc.sock,grpc.cookie}` and the stale-dir sweep. |
| `SecurityService.Session` | `grpc_server/security_service.hpp` | The pairing lock (server-streaming RPC). |
| `VurioGrpcClient` | `swift_ui/.../GrpcClient.swift` | Discovery, candidate ordering, pairing, lock holding, failure alert. |

## Server side

1. **Instance directory.** `InstancePaths::pruneStale()` removes leftovers of
   dead pids, then the server creates `~/.vurio/instances/<pid>/` (0700) and
   puts its Unix socket and 0600 pairing cookie there. Nothing is shared
   between instances any more; the old `/tmp/vurio_dedup.sock` and
   `~/.vurio/grpc.cookie` are gone (`./killall` still sweeps them).
2. **Election.** `InstanceBroker::start()` tries to bind
   `127.0.0.1:<server.broker.port>` (default 47911) with `SO_REUSEPORT`
   disabled — the bind *must* fail when the port is taken, that is the whole
   election. The winner hosts `BrokerService` and lists itself; a loser
   registers with it and heartbeats every 2 s. Entries without a heartbeat
   for 10 s are pruned. A registrant whose heartbeat gets `UNAVAILABLE`
   retries the bind on its next tick, so the role moves within ~2 s of the
   broker process exiting. `NOT_FOUND` on heartbeat means a *new* broker:
   re-register.
3. **Lock.** `Handshake` still issues the session token (cookie + VER-1
   build check). The lock is `Session`: whoever holds that stream owns the
   instance. While held, `Handshake` answers `BUSY: …` and a second `Session`
   gets `ALREADY_EXISTS`; the busy flag is pushed to the broker at once.
   When the stream ends (UI quit or crashed — surfaces as a cancelled
   context / failed write within ~1 s) the server deregisters and shuts down
   through the same path as the scheduler's failure shutdown. A stream ended
   by the server's *own* `stop()` is not a departed client.
4. **Mismatch is a plain rejection.** Pre-BROKER-1, a version mismatch made
   both sides exit (dated from when exactly one pair could exist). Now a UI
   probes foreign builds as a matter of course, so the server logs a
   warning, answers `VERSION_MISMATCH: …`, and stays up.
5. **Same-binary guard.** `InstanceChecker` only refuses to start when
   another process is running *this exact executable path* (a duplicate of
   this worktree); siblings from other worktrees are ignored.

## UI side

1. Ask the broker (`127.0.0.1:$VURIO_BROKER_PORT`, default 47911) for
   `ListInstances`. No answer is normal for a moment (first server still
   starting, role changing hands): retry every second.
2. Keep instances whose `version`+`commit` equal `VersionInfo`, drop busy
   ones, and order them: same `launch_group` first (the restart script
   exports `VURIO_INSTANCE_GROUP=<worktree>` to both processes), then any
   other free match — they are the same build, so it does not matter.
3. For each candidate: read its cookie, open a channel to its `endpoint`
   (`unix:<path>` today; `ipv4:host:port` is already parsed for Windows),
   `Handshake`, then `Session`. The first `SessionEvent` proves the lock is
   ours; `BUSY`/`ALREADY_EXISTS` means someone got there first — next
   candidate.
4. After 30 s without a partner, one alert that says which it was: no
   broker/servers at all, servers but none of this build (lists them), or
   matching servers all busy. Then quit.
5. If the held stream ends (server died), drop the clients and rediscover;
   the same alert appears if nothing turns up.

`connect(takeLock: false)` handshakes without the lock — the Swift unit test
uses it, because the restart script hands the same backend to the real UI
right after the tests.

## Operating

- `./restart` per worktree; each pair finds itself through the broker.
- `./killall` is still the machine-wide sweep after a crash (also removes
  `~/.vurio/instances/*`).
- Log lines to look for: `InstanceBroker: This instance is the broker` /
  `Registered with broker`, `GrpcSecurity: Session started: paired with …`,
  `Session ended: … is gone`, and the UI's `discovery: N instance(s), M same
  build, K free`.
