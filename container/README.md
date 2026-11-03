# Containers

Two kinds of podman work live here:

- the **build environments**, one per supported link mode, which build, test,
  and install bjorne_agent inside a container and leave the artifacts on the host
- the **runtime image**, which is what the harness actually runs in

| Mode | Base | C library | OpenSSL | Result |
| --- | --- | --- | --- | --- |
| `static` | Alpine 3.22 | musl | static (`libssl.a`, `libcrypto.a`) | fully static, no dynamic dependencies |
| `dynamic` | Debian bookworm-slim | glibc | shared (`libssl.so.3`) | dynamically linked |
| `notls` | Alpine 3.22 | musl | none | fully static, https endpoints rejected |

## Runtime image

`Containerfile.runtime` builds the image the harness runs in. The first stage
is Alpine with the build tools and static OpenSSL. The second stage is
`scratch` and holds only what the run needs.

```sh
container/build-runtime.sh          # builds the image tagged "bjorne_agent"
container/build-runtime.sh mytag    # builds under a different tag
```

The script builds the image, prints the image size and entrypoint, and runs
`bjorne_agent --help` as a smoke test.

### What the image holds

| Path | Why |
| --- | --- |
| `/usr/local/bin/bjorne_agent` | the fully static binary, and the entrypoint |
| `/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt` | the trust store the TLS transport verifies the peer against |
| `/bin/busybox` and its applet symlinks | `run_shell` calls `popen`, which runs `/bin/sh` |
| `/lib/ld-musl-x86_64.so.1`, `/lib/libc.musl-x86_64.so.1` | busybox is linked against musl, so the loader comes with it |
| `/etc/bjorne_agent/prompts/compression.md` | the compression prompt, next to the default config mount point |
| `/usr/local/share/bjorne_agent/prompts/compression.md` | the same prompt in the install layout, for configs mounted elsewhere |
| `/etc/passwd`, `/etc/group` | scratch has no account database, so the image names its own user |
| `/workspace` | the working directory, owned by uid 1000 |
| `/tmp` | mode 1777, for tools that need scratch space |

The container runs as uid 1000, named `bjorne_agent`.

Podman writes `/etc/resolv.conf` and `/etc/hosts` into the container even
when the image is `scratch`. The static build resolves names through
`/etc/hosts` and the libc stub resolver, so DNS works without anything in the
image.

The image comes to about 9 MB.

### Delivering the config

The config arrives either as a mount or through the environment. The binary is
the entrypoint, so the task is the only argument.

The default `BJORNE_AGENT_CONFIG` value is `/etc/bjorne_agent/bjorne_agent.json`. A mount at
that path needs no extra variable:

```sh
podman run --rm \
  -v "$PWD/bjorne_agent.json:/etc/bjorne_agent/bjorne_agent.json:ro" \
  -v "$PWD:/workspace" \
  -e GAB_API_KEY \
  bjorne_agent "your task"
```

For any other mount point, name it with `BJORNE_AGENT_CONFIG`:

```sh
podman run --rm \
  -e BJORNE_AGENT_CONFIG=/run/conf/bjorne_agent.json \
  -e GAB_API_KEY \
  -v "$PWD/bjorne_agent.json:/run/conf/bjorne_agent.json:ro" \
  -v "$PWD:/workspace" \
  bjorne_agent "your task"
```

`BJORNE_AGENT_CONFIG` wins over the config search path. A value that names a file
that is not there stops the run with an error naming the variable and the
path, rather than falling back to the search.

The API key comes from the environment variable the config names, through
`api_key_env`. The value never goes in the config file.

The workspace is a volume mount, and the config's `agent.workspace` should
name the path as it appears inside the container. `/workspace` is the default
in the image.

### The compression prompt path

A relative `compression_prompt_path` in the config resolves against the
directory holding the config file, not the working directory. That means a
config mounted at `/etc/bjorne_agent/bjorne_agent.json` can use the relative path
`prompts/compression.md` and find the prompt that sits next to it. A config
mounted anywhere else needs the absolute path:

```json
"compression_prompt_path": "/usr/local/share/bjorne_agent/prompts/compression.md"
```

Leaving `compression_prompt_path` out works too. The default prompt is
"Summarize the conversation".

## Build environments

Three podman environments, one per supported link mode.

### Running

```sh
container/build.sh                # all three modes
container/build.sh static         # one mode
container/build.sh dynamic notls  # several modes
```

`container/build.sh` refuses to run as root and exits 1. There is no root path
that silently works, because a root build would produce artifacts the caller
cannot write to. An unknown mode name exits 2.

Each mode builds an image tagged `bjorne_agent-build-<mode>`, then runs it against
the repository bind-mounted at `/src`. The images are cached, so a second run
of the same mode skips the image build.

### What lands on the host

Every mode writes to `build-container-<mode>/`, owned by the caller:

```
build-container-static/bjorne_agent            the executable
build-container-static/bjorne_agent_tests      the test binary
build-container-static/libbjorne_agent_core.a  the core library
```

The install tree sits in a subdirectory of the same place:

```
build-container-static/install/bin/bjorne_agent
build-container-static/install/lib/libbjorne_agent_core.a
build-container-static/install/share/bjorne_agent/prompts/compression.md
```

That is the same layout `cmake --install` produces on a host build, so the
container output can be copied to a target machine as it is. The script prints
`file` and `ldd` for each executable when it finishes.

`ldd` reports "not a dynamic executable" and exits 1 for the static builds.
The script treats that as information rather than an error.

### How the uid mapping works

Rootless podman maps the caller's uid to container uid 0 by default, so a
container user with the caller's uid would not exist. Each image therefore
creates a `builder` user from `--build-arg UID/GID`, and the run passes
`--userns=keep-id` so that uid is mapped to the caller's on the host. The
bind mount at `/src` stays writable without root, and nothing compiles as root.

### What each mode exercises

#### static

`BJORNE_AGENT_OPENSSL=static` turns on `BJORNE_AGENT_STATIC` by default, which selects
the stub resolver. Name resolution then runs in this order:

1. numeric addresses are used directly
2. `/etc/hosts` is consulted
3. a direct DNS query goes out through `res_search`

`getaddrinfo` is never called. A statically linked binary cannot load the NSS
modules `getaddrinfo` needs, so the stub path exists to avoid the call
entirely. The direct DNS query is what makes the static build usable against
a real endpoint: it resolves names that are not in `/etc/hosts`. An
unresolvable name fails with `DNS resolution failed: Host not found`.

#### dynamic

Shared OpenSSL and glibc, so the full NSS path and `getaddrinfo` are compiled
in. The binary needs `libssl.so.3`, `libcrypto.so.3`, `libstdc++.so.6`, and a
compatible glibc at run time. OpenSSL pulls in `libz`, `libzstd`, and the
brotli libraries as well.

#### notls

`BJORNE_AGENT_OPENSSL=disabled` skips `find_package(OpenSSL)` entirely, so there is
no OpenSSL path in `CMakeCache.txt`, nothing in `flags.make`, and no `ssl` or
`crypto` on the link line. The binary is static, so nothing can be resolved at
run time either.

An https endpoint fails at the model client with a clear error rather than
being downgraded to plain text:

```
Endpoint https://gab.ai/v1 uses https, but this build has no TLS support
```

The `notls` image itself still carries `libssl.so.3` and `libcrypto.so.3`,
because Alpine's `cmake` package depends on both directly and the image
installs cmake. The build never links against them.

### Running an artifact

The static binary has no runtime dependencies and runs on any x86-64 Linux
machine:

```sh
./build-container-static/bjorne_agent bjorne_agent.json "say hello"
```

The dynamic binary needs the shared libraries listed above on the target. The
notls binary talks to http endpoints only.

Progress output goes to stderr and the final answer to stdout, so the answer
can be piped:

```sh
./build-container-static/bjorne_agent --quiet bjorne_agent.json "say hello" > answer.txt
```

### Coverage

Coverage builds are separate, because gcov needs the symbol table and the
shared runtime, and the static build strips both. To build in the container with the
coverage flag set directly:

```sh
cmake -S . -B build-coverage -DBJORNE_AGENT_COVERAGE=ON -DBJORNE_AGENT_OPENSSL=shared
cmake --build build-coverage --target bjorne_agent_tests
./build-coverage/bjorne_agent_tests
```
