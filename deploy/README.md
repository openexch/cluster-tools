# Deploying the cluster-tools processes

Units, launchers and an image definition for the external media driver (`aeronmd`) and the workers (`ct-bridge`,
`ct-forwarder`) on hosts you manage. The unit files are the same on every host.

| Path | What |
|---|---|
| `systemd/aeronmd@.service` | the external media driver of one process: `aeronmd@<name>.service` (core, match, core-replica, ct-bridge, oe-gateway, ...) |
| `systemd/ct-worker@.service` | a worker with the external driver (production): `ct-worker@ct-bridge.service` |
| `systemd/ct-worker-embedded@.service` | a worker with the driver in the process (development and demo) |
| `bin/aeronmd-launch` | runs aeronmd on `driver.properties` plus the `driver-<profile>.properties` overlay; `wait-cnc` waits for a usable driver |
| `bin/worker-launch` | sets the worker's driver mode from the unit, refuses an env file that says otherwise, execs the worker |
| `env/*.env.example` | the driver and worker env files |
| `media-driver/` | the dev and prod driver overlays |
| `oci/Dockerfile`, `oci/smoke.sh` | the image (published set on ubuntu:24.04, non-root, read-only root) and its smoke test |
| `render-verify.sh` | renders three hosts and checks the result |

## Install

1. Create the user every process and its driver run as: `useradd --system --no-create-home --shell /usr/sbin/nologin openexchange`.
2. Put the published set (aeronmd, `libaeron.so`, `libaeron_driver.so`, the three driver properties files, the
   workers) at `/opt/openexchange/cluster-tools/` and `bin/*` at `/opt/openexchange/cluster-tools/bin/` (0755).
3. Copy the units to `/etc/systemd/system/`.
4. Driver knobs (optional) in `/etc/openexchange/aeronmd/<name>.env`: `AERON_DIR` (default
   `/dev/shm/aeron-openexchange-<name>`, the same default the client unit uses), `AERON_DRIVER_PROFILE` (dev|prod,
   default prod), `DRIVER_CPU_SET`.
5. Worker settings in `/etc/openexchange/cluster-tools/<worker>.env` (the worker checks them all and exits 78 on any
   problem), credentials in `<worker>.secrets.env` (root:root 0600).
6. `systemctl enable --now ct-worker@ct-bridge.service`.

## Driver wiring and restarts

- `aeronmd@<name>` turns active only once `cnc.dat` exists (ExecStartPost `wait-cnc`), so a client ordered `After=` it
  starts against a usable driver. It restarts always (1 s to 30 s backoff) except on 78.
- A client unit `BindsTo=` its driver: systemd stops it when the driver stops or dies. The client is also
  `WantedBy=` its driver, so a driver that systemd restarts brings it back. Stopping only the client keeps it stopped.
- Workers: `Restart=on-failure`, backoff 2 s to 60 s, `RestartPreventExitStatus=78`.
- cgroup limits (`MemoryMax`, `CPUQuota`) are unit properties: set them in a per-host drop-in.

## Image

```sh
docker build -f deploy/oci/Dockerfile --build-context deploy=deploy -t openexchange/cluster-tools <published set dir>
docker run --read-only --tmpfs /tmp --shm-size 1g -e AERON_DIR=/dev/shm/aeron-core -e AERON_DRIVER_PROFILE=prod \
    openexchange/cluster-tools /opt/openexchange/cluster-tools/bin/aeronmd-launch run
```

A process using this driver shares its network namespace (`--network container:<driver>`), its driver directory (a
shared tmpfs volume, `AERON_DIR` in both) and its uid (10001).

## Tests

- `deploy/render-verify.sh [--self-test]`: renders three hosts and checks unit identity, the restart and driver
  wiring, `systemd-analyze verify` on every instance, and both launchers (syntax, shellcheck, refusals, the command
  they exec, `wait-cnc` with a late `cnc.dat`). `--self-test` feeds each check a broken render and requires it to fail
  with its message.
- `deploy/oci/smoke.sh --set <published set dir>`: builds the image, runs aeronmd to a `cnc.dat` (read-only root,
  non-root), and requires 78 from the workers without settings, from a contradicting driver mode and from no command.
