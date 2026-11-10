# bjorne_agent

[![codecov](https://codecov.io/gh/ruxven/bjorne_agent/branch/master/graph/badge.svg)](https://codecov.io/gh/ruxven/bjorne_agent)
[![complexity](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/ruxven/bjorne_agent/gh-pages/complexity/badges/complexity.json)](https://ruxven.github.io/bjorne_agent/complexity/complexity.html)
[![codechecker](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/ruxven/bjorne_agent/gh-pages/security-reports/badges/codechecker.json)](https://ruxven.github.io/bjorne_agent/security-reports/html/index.html)
[![codeql](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/ruxven/bjorne_agent/gh-pages/security-reports/badges/codeql.json)](https://github.com/ruxven/bjorne_agent/security/code-scanning)

A coding agent harness in C++20. It takes a task, calls an OpenAI compatible
model endpoint, runs the tools the model asks for, and repeats until the model
answers.

The binary carries no third party runtime libraries. JSON parsing, HTTP, and
the agent loop are all in the source tree. TLS is the only optional external
dependency, and it can be linked statically, shared, or left out entirely.

The design goal is a single binary plus a JSON file that a container can
receive and run without a package manager in the image.

## Contents

- [Requirements](#requirements)
- [Build](#build)
- [Install](#install)
- [Usage](#usage)
- [Configuration](#configuration)
- [Tools](#tools)
- [Containers](#containers)
- [Development](#development)
- [License](#license)

## Requirements

| | |
| --- | --- |
| CMake | 3.16 or newer |
| Compiler | GCC or Clang with C++20 |
| OpenSSL | optional, required for https endpoints |
| Build tools | Ninja is used below, but Make works too |

The only other dependency is Catch2, vendored under `third_party/catch2`. It is
used for the test binary and is not part of the shipped artifact.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local/opt/bjorne_agent" \
  -DBJORNE_AGENT_OPENSSL=shared
cmake --build build
```

Four options control the link.

| Option | Values | Default | Effect |
| --- | --- | --- | --- |
| `BJORNE_AGENT_OPENSSL` | `static`, `shared`, `disabled` | `static` | Which OpenSSL to link. `disabled` compiles out every TLS code path. |
| `BJORNE_AGENT_TLS` | `openssl`, `none` | `openssl` | Whether a TLS backend exists. `none` removes OpenSSL from the link. |
| `BJORNE_AGENT_STATIC` | `ON`, `OFF` | auto | Links `-static -s`. Defaults to `ON` for `static` and `disabled` OpenSSL, `OFF` for `shared` and coverage builds. Needs a GNU or Clang toolchain; other toolchains turn it off with a warning. |
| `BJORNE_AGENT_COVERAGE` | `ON`, `OFF` | `OFF` | Instruments with gcov. Keeps the symbol table, so the binary is not stripped. |

`BJORNE_AGENT_OPENSSL=disabled` and `BJORNE_AGENT_TLS=none` both remove OpenSSL. The
first says the library is not there, the second says the backend is not there.
Either one leaves a binary that speaks plain HTTP only.

Two behaviors follow from the link mode.

**A static binary resolves names differently.** Static glibc cannot call
`getaddrinfo`, because NSS modules are shared objects that a static binary
cannot load. The static build reads `/etc/hosts` and queries the nameservers
directly through the libc stub resolver. `nsswitch.conf` and mDNS are not
consulted.

**A binary without TLS rejects https endpoints.** The error names the endpoint
and says the build has no TLS support. It never silently downgrades to plain
text, because the API key would cross the wire in the clear.

On a host that ships only shared OpenSSL, `BJORNE_AGENT_OPENSSL=static` fails at
configure time with a missing `OPENSSL_CRYPTO_LIBRARY`. Use `shared` there.

## Install

```sh
cmake --install build
```

The layout under the prefix:

```
<prefix>/bin/bjorne_agent
<prefix>/lib/libbjorne_agent_core.a
<prefix>/share/bjorne_agent/prompts/compression.md
```

`bjorne_agent` is the executable. `libbjorne_agent_core.a` is the library behind it, for
embedding the harness in another program. The prompts directory holds the
default compression instruction.

Two install components are defined. `runtime` covers the binary and the
library. `prompts` covers the prompt files.

```sh
cmake --install build --component runtime
cmake --install build --component prompts
```

### Package

CPack is configured, so the build directory can produce archives directly:

```sh
cmake --build build --target package
```

That writes a `.tar.gz`, a `.deb` and a `.rpm` into the build directory. The
Debian and RPM generators need `dpkg-dev` and `rpm` on the machine that runs
the packaging step.

### Cross build for Windows

A 64 bit MinGW-w64 cross build from Linux uses the toolchain file in
`cmake/toolchains/`:

```sh
sudo apt-get install g++-mingw-w64-x86-64
cmake -S . -B build_windows -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBJORNE_AGENT_OPENSSL=disabled \
  -DBJORNE_AGENT_STATIC=OFF
cmake --build build_windows
```

The executable lands in `build_windows/bin/bjorne_agent.exe`.

A native MSVC build works too, but MSVC rejects the GNU link options, so
`BJORNE_AGENT_STATIC` is turned off for it and `BJORNE_AGENT_OPENSSL=disabled`
removes the OpenSSL dependency.

### Cross build for ARM (Raspberry Pi)

Two toolchain files cover the Raspberry Pi. `linux-aarch64.cmake` builds for
the 64 bit Pi 3, 4 and 5, and `linux-armhf.cmake` builds for the 32 bit Pi 2
and later:

```sh
sudo apt-get install g++-aarch64-linux-gnu
cmake -S . -B build_arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBJORNE_AGENT_STATIC=ON
cmake --build build_arm64
```

Swap `aarch64` for `armhf` and the compiler package for
`g++-arm-linux-gnueabihf` for the 32 bit build. The Pi 1, Zero and Zero W are
ARMv6, which the armhf toolchain does not target by default; add
`-DCMAKE_CXX_FLAGS="-march=armv6 -mfpu=vfp -mfloat-abi=hard"` or build on the
Pi itself.

The static build needs no libraries on the target, and the packages record the
target architecture (`arm64` or `armhf`) rather than the build host's.

A cross build with TLS needs the target OpenSSL. Debian's static `libssl.a` is
built with zlib and zstd, so the link has to name both, and the OpenSSL paths
are passed explicitly because the cross sysroot does not contain them:

```sh
sudo dpkg --add-architecture arm64
sudo apt-get update
sudo apt-get install libssl-dev:arm64 zlib1g-dev:arm64 libzstd-dev:arm64
cmake -S . -B build_arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBJORNE_AGENT_STATIC=ON \
  -DOPENSSL_INCLUDE_DIR=/usr/include \
  -DOPENSSL_SSL_LIBRARY=/usr/lib/aarch64-linux-gnu/libssl.a \
  -DOPENSSL_CRYPTO_LIBRARY=/usr/lib/aarch64-linux-gnu/libcrypto.a \
  -DCMAKE_CXX_STANDARD_LIBRARIES="-lz -lzstd"
```

For armhf the library directory is `/usr/lib/arm-linux-gnueabihf`.

To run the cross built tests on the build machine, install `qemu-user-static`
and run the test binary through it. The binary is static, so no sysroot is
needed:

```sh
qemu-aarch64-static build_arm64/bin/bjorne_agent_tests
```

## Usage

```
bjorne_agent [--verbose|--quiet] [config] [task]
```

The config path and the task are both optional, and the binary works out
which is which.

| Invocation | Config | Task |
| --- | --- | --- |
| `bjorne_agent config.json "fix the tests"` | `config.json` | `fix the tests` |
| `bjorne_agent "fix the tests"` | discovered | `fix the tests` |
| `bjorne_agent config.json` | `config.json` | standard input |
| `bjorne_agent` | discovered | standard input |

A single argument that names an existing file is the config path. Anything
else is the task. With no config path, the binary searches:

1. `./.bjorne_agent/bjorne_agent.json`, the project local file
2. `$XDG_CONFIG_HOME/bjorne_agent/bjorne_agent.json`, the user level file. When
   `XDG_CONFIG_HOME` is unset or relative, the path is
   `~/.config/bjorne_agent/bjorne_agent.json`.

The project local file wins, so a repository can carry its own settings
without touching the user configuration. When no candidate exists, the run
stops and names every path it checked.

The final answer goes to standard output. Progress goes to standard error, so
`bjorne_agent "task" > answer.txt` keeps only the answer.

### Output levels

| Level | Shows |
| --- | --- |
| `quiet` | Nothing. Errors that end the run still print. |
| `normal` | Each step: config load, model selection, iteration count, request, the model's reasoning and narration, each tool's arguments and result, completion. |
| `verbose` | Adds the endpoint internals: DNS resolution, TCP connect, TLS handshake, certificate verification, response size. |

Set the level with `--verbose` or `--quiet`, or with the `BJORNE_AGENT_LOG`
environment variable. A command line flag wins over the variable.

```sh
bjorne_agent --verbose config.json "what is in this repository"
BJORNE_AGENT_LOG=quiet bjorne_agent config.json "what is in this repository"
```

Every progress line is stamped with the elapsed time, so a run that stalls on
a slow endpoint shows how long it has been waiting and where. The model's own
words appear as indented `thinking` and `assistant` blocks, and each tool call
shows its arguments and a preview of its result:

```text
[bjorne_agent 3.1s] thinking:
  The user wants proof the shell works.
  I should echo a word and read the output.
[bjorne_agent 3.1s] assistant:
  I will run a command to check the shell.
[bjorne_agent 3.1s] info: Running tool: run_shell
[bjorne_agent 3.1s] arguments:
  {"command": "echo hello"}
[bjorne_agent 3.2s] result:
  hello
```

Long reasoning and tool output are cut at a few thousand characters, with a
note that says how much was left out. The final answer still goes to stdout
only, so piping the answer stays clean.

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | The run produced an answer. |
| `1` | The config failed to load, no model was configured, the compression prompt could not be read, the task was empty, or the model or the loop failed. |

### Setting up a config

`setup.sh` asks for the settings and writes both a config and a `.env`:

```sh
./setup.sh
```

It writes to `./.bjorne_agent/bjorne_agent.json` or `~/.config/bjorne_agent/bjorne_agent.json`, the
same two paths the binary searches, and puts a `.env` beside the config with
the API key variable stubbed out. Fill in the key and source it:

```sh
source .bjorne_agent/.env
bjorne_agent "hello"
```

## Configuration

The config is JSON with `snake_case` keys. `config/bjorne_agent.example.json` is a
complete example.

```json
{
  "version": 1,
  "agent": {
    "system_prompt": "You are a coding agent. Help the user with their task.",
    "workspace": "/workspace"
  },
  "models": [
    {
      "provider": "openai-compatible",
      "endpoint": "https://api.example.com/v1",
      "model": "example-model",
      "context_window": 128000,
      "max_tokens": 4096,
      "temperature": 0.7,
      "api_key_env": "OPENAI_API_KEY",
      "priority": 1,
      "compression_prompt_path": "prompts/compression.md",
      "ignore_self_signed": false,
      "hostname": ""
    }
  ]
}
```

### Agent

| Key | Type | Meaning |
| --- | --- | --- |
| `system_prompt` | string | Instruction content sent to the model. |
| `workspace` | string | Root for tool operations. Relative tool paths resolve against it. |

### Model

| Key | Type | Meaning |
| --- | --- | --- |
| `provider` | string | Provider identifier, for example `openai-compatible`. |
| `endpoint` | string | Base URL. The path is a prefix, so `https://api.example.com/v1` posts to `/v1/chat/completions`. |
| `model` | string | Model identifier. |
| `context_window` | integer | Token count for the model context. |
| `max_tokens` | integer | Token cap per completion. |
| `temperature` | number | Sampling parameter. |
| `api_key_env` | string | Name of the environment variable holding the API key. The key stays out of the file. |
| `priority` | integer | Lower runs first. One is the first choice. |
| `compression_prompt_path` | string, optional | Markdown file holding the compression instruction. |
| `ignore_self_signed` | boolean, optional | Defaults to `false`. When `true`, peer verification and the hostname check are both off. |
| `hostname` | string, optional | Name sent in the TLS SNI extension and matched against the peer certificate. Empty means the endpoint host. |

The endpoint scheme picks the transport. `https` uses TLS, `http` uses plain
TCP, which is what a local model such as llama.cpp needs. A host may carry an
explicit port, as in `http://127.0.0.1:11434/v1`. IPv6 hosts use brackets, as
in `http://[::1]:8080/v1`.

A relative `compression_prompt_path` resolves against the directory holding
the config file, not the working directory. When the key is absent, the
compression instruction is `Summarize the conversation`.

### TLS

Certificate verification is on by default. The chain is checked against the
system trust store, and the expected hostname is the `hostname` key when
present, otherwise the host from the endpoint. A rejected certificate ends the
run with the reason, for example `Certificate verification failed:
self-signed certificate`.

`ignore_self_signed` turns off both the chain check and the hostname check.
Use it only against a host with a self-signed certificate. The API key travels
over this connection, so a build that trusts arbitrary certificates can hand
the key to a wrong peer.

Set `hostname` when the endpoint host is an address or an internal name but
the certificate carries a different name.

## Tools

The model requests tools as JSON function calls. The set is fixed.

| Tool | Arguments | Behavior |
| --- | --- | --- |
| `read_file` | `path` | Reads a file from the workspace. |
| `write_file` | `path`, `content` | Writes a file in the workspace. |
| `edit_file` | `path`, `old`, `new` | Replaces the first occurrence of `old` in the file. |
| `run_shell` | `command` | Runs a shell command in the workspace. Output is capped at 256 KiB and marked when truncated. |

Relative paths resolve against `agent.workspace`. A path that traverses a
parent directory is rejected. A tool that fails does not stop the loop: the
error becomes tool result text and the model decides what to do next.

## Containers

`container/README.md` covers the podman work: the three build environments and
the runtime image.

The build environments are a fully static musl build, a dynamic glibc build
with shared OpenSSL, and a no-TLS build. The script builds, tests, and installs
inside the container and leaves the artifacts on the host.

```sh
container/build.sh                # all three
container/build.sh static         # one mode
```

Each mode writes `build-container-<mode>/`, which holds the build outputs and
an `install/` tree with the same layout as a host install.

The runtime image is what the harness runs in. It is a `scratch` image holding
the static binary, a CA bundle, busybox for the `run_shell` tool, and the
compression prompt. The binary is the entrypoint, so the task is the only
argument.

```sh
container/build-runtime.sh         # builds the image tagged "bjorne_agent"
```

The config arrives either as a mount at the default path or through the
`BJORNE_AGENT_CONFIG` environment variable. The API key arrives through the
environment variable the config names.

```sh
# Config as a mount.
podman run --rm \
  -v "$PWD/bjorne_agent.json:/etc/bjorne_agent/bjorne_agent.json:ro" \
  -v "$PWD:/workspace" \
  -e GAB_API_KEY \
  bjorne_agent "your task"

# Config through the environment.
podman run --rm \
  -e BJORNE_AGENT_CONFIG=/run/conf/bjorne_agent.json \
  -e GAB_API_KEY \
  -v "$PWD/bjorne_agent.json:/run/conf/bjorne_agent.json:ro" \
  -v "$PWD:/workspace" \
  bjorne_agent "your task"
```

A relative `compression_prompt_path` resolves against the directory holding
the config file, so a config mounted outside `/etc/bjorne_agent` needs the absolute
path `/usr/local/share/bjorne_agent/prompts/compression.md`.

## Development

Run the tests:

```sh
cmake --build build
cd build && ctest --output-on-failure
```

The suite covers the JSON parser, the config loader, config discovery, CLI
argument resolution, URL parsing, DNS, the HTTP client, TLS, the model
client, context management, the tools, the agent loop, and the log module.

The static analysis gates used by this project:

```sh
lizard src/ -l cpp -C 15 -L 100 -a 5
```

Complexity stays under 15 per function, 100 lines per function, and 5
parameters per function.

```sh
CodeChecker analyze build/compile_commands.json \
  --output reports \
  --analyzers clangsa clang-tidy \
  --ctu \
  --skip .codechecker.skip
```

`.codechecker.skip` restricts the analysis to `src/` and leaves the vendored
Catch2 out of it.

The documents in this directory describe the intended behavior:

| File | Contents |
| --- | --- |
| `SPEC.md` | The baseline specification. |
| `SPEC.CONFIG.md` | The configuration file format. |
| `SPEC.LOOP.md` | The agent loop. |
| `DESIGN.md` | How the implementation maps to the specifications. |
| `ROADMAP.md` | What is done and what is next. |

## License

MIT. See [LICENSE](LICENSE).

The vendored Catch2 sources under `third_party/catch2/` carry their own license,
Boost Software License 1.0. They are used for the test binary only and are not
part of the shipped artifact.
