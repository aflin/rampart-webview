# rampart-webview docker oven

Builds **`rampart-webview.so`** in a **Debian 11** oven and installs it into
`/usr/local/rampart-ml/modules`.

**Debian 11 is the build base because it's the earliest distro with the WebKit/
GLib *dev headers* the source needs** — `GUri` (GLib ≥ 2.66) and JSC typed-array/
ArrayBuffer (webkit2gtk-4.0 ≥ 2.38). CentOS 7's webkit 2.28 / GLib 2.56 are too
old to compile against; older bases (Debian 10 / RHEL 8, Ubuntu 20.04) ship
GLib < 2.66 (no `GUri`).

**The build base does *not* set the floor.** The resulting `.so` references no
glibc symbol newer than **`GLIBC_2.14`** (the `GUri`/JSC calls are webkit/GLib
symbols, resolved at runtime — not glibc), so glibc is *not* the constraint. The
real requirement is the **runtime stack** the `.so` `NEED`s — `libwebkit2gtk-4.0`,
`libgtk-3`, `libglib-2.0`, `libsoup`, plus `libstdc++` ≥ gcc 5.1
(`GLIBCXX_3.4.21`) — all **target-supplied, never bundled** (inherent to a
webview). Any distro new enough to *have* webkit satisfies all of them.

```
docker/build.sh <stage>
```

## Commands

| Command | What it does |
|---|---|
| `docker/build.sh build` | Compile → `build/oven/rampart-webview.so` |
| `docker/build.sh install` | Install it into `/usr/local/rampart-ml/modules` |
| `docker/build.sh shell` | Interactive shell in the oven |
| `docker/build.sh save-image` | Persist the oven image to a `.tar.gz` (see below) |
| `docker/build.sh --rebuild-image [...]` | Force a fresh image first (after a `Dockerfile` edit) |

No `test` stage: webview tests need a virtual X display + a glibc-compatible
rampart binary; run those on a real desktop via `headless.sh`.

## Mounted directories

Nothing host-facing is baked into the image — it's all bind-mounted at
`docker run` time. `$REPO` is the repo root (`/usr/local/src/rampart-webview`).

| Stage | Host path → container path | Mode |
|---|---|---|
| **build** | `/usr/local/src/rampart-webview` → `/webview` | rw |
| | `/usr/local/rampart-ml/include` → `/usr/local/rampart-ml/include` | **ro** |
| | `/etc/passwd` → `/etc/passwd` | ro |
| | `/etc/group` → `/etc/group` | ro |
| **install** | `/usr/local/src/rampart-webview` → `/webview` | rw |
| | `/usr/local/rampart-ml` → `/usr/local/rampart-ml` | rw |
| **shell** | `/usr/local/src/rampart-webview` → `/webview` | rw |
| | `/usr/local/rampart-ml/include` → `/usr/local/rampart-ml/include` | ro |

Why each one:

- **Repo (`/webview`)** — always rw: oven artifacts go to `build/oven/`. `build/`
  is gitignored.
- **`/usr/local/rampart-ml/include`** (ro at build) — the compile only needs
  rampart's headers (`-DRAMPART_INCLUDE=…`); no rampart binary is run.
- **`/usr/local/rampart-ml`** (rw at install) — `cmake --install` drops the module
  into `…/modules`.
- **`/etc/passwd` + `/etc/group`** (ro) — only on `build`, which runs as your uid
  (`--user`) so the uid resolves to a name. `install` runs as root.

Everything else (gcc 10, cmake, GTK3 + webkit2gtk-4.0 dev) lives **inside** the
Debian 11 image.

## The oven image

The image (`rampart-webview-oven`) lives in your local docker store and persists
there across reboots and container runs. `build.sh` finds it automatically.

`save-image` additionally writes it to `build/rampart-webview-oven.image.tar.gz`
(for `docker load` after a prune, or to move it to another machine). If that
tarball exists, `ensure_image` restores it with `docker load` instead of
rebuilding. After editing the `Dockerfile`, rebuild with `--rebuild-image`.
