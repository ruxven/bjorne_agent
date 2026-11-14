# Design: C++ Agent Harness

## Purpose

This document drafts the design for the C++ agent harness and maps the design to the specifications.

## Design Principles

The design keeps the dependency footprint low. Version one uses no third-party C++ libraries. The runtime uses the standard library only. JSON and HTTP are hand written. The binary is statically linked where possible.

## Architecture

The binary is split into focused components. The design keeps the boundaries small.

### Entry Point

The entry point resolves the config file and the task from the command line, loads the configuration, and starts the agent loop. The command line is a small set of reserved verbs with `run` as the default, so the bare form keeps working. `sessions` lists the store and `resume` continues a stored session; `init` still reports that it is not implemented. Flags may appear before or after the verb, and `--` ends flag parsing. Help is answered before any config lookup. A path on the command line names the config file. Without one, the run reads the BJORNE_AGENT_CONFIG environment variable, and when that is unset it searches the project local path and then the user level path. The task comes from standard input when the command line names none, and an empty command line is a usage error rather than a wait on a terminal. The environment variable is the container path: the config is mounted where the search does not look, so the variable names it. A variable naming a file that is not there stops the run with an error that names the variable and the path.

### Configuration

The configuration loader implements the configuration specification. It reads JSON, uses a hand written parser, and uses snake_case keys. It validates the version, agent section, and models array. The agent section also carries `skills_path`, `session_dir`, and `interactive`, and the top level carries an `mcp_servers` map of stdio server definitions. The loader reads those fields now so later phases consume a stable configuration shape.

The config path search lives in its own module, separate from the loader, because finding the file and reading the file are different jobs.

### Context Fragments

Two context sources are assembled before the model runs, and both go through one fragment mechanism. The mechanism owns the section heading, the order the caller supplies, and the byte cap, so a runaway file cannot crowd out the task.

AGENTS.md discovery walks from the workspace up to the filesystem root, then reverses the list so the nearest file comes last. The global file comes before that list. Candidates are compared by canonical path, so a global file that sits inside the workspace is included once. A file that cannot be read is reported and skipped rather than failing the run.

Skills are scanned from `skills_path` one directory level deep. The frontmatter scanner accepts `key: value` lines, quotes, CRLF, and a UTF-8 byte order mark, which covers real skill directories without a YAML dependency. A skill without a name or a description is skipped with the reason. The catalog renders only names and descriptions into the system prompt; the instructions stay behind a `load_skill` tool that returns the body on demand. Registering that tool only when the catalog is non-empty keeps a tool with nothing to load out of the model's choices.

### Model Client

The model client implements the model interaction from the baseline specification. It calls an OpenAI compatible chat completions endpoint. It uses a hand written HTTP client and TLS through OpenSSL.

The endpoint is parsed once per request into scheme, host, port, and path prefix. The scheme picks the transport: https builds an OpenSSL transport, http builds a plain TCP transport. The port defaults from the scheme but an explicit port overrides it, which is what makes a local model reachable. Ports are passed explicitly through the transport interface rather than defaulted inside it, so the wire target is visible at the call site and the transports are testable against a loopback server. The path prefix from the endpoint is prepended to /chat/completions.

The build uses a CMake option for OpenSSL. The option has three states: static, shared, and disabled. Static is the default. Static links OpenSSL into the binary, which suits musl static builds. Shared links OpenSSL as a shared library. Disabled removes OpenSSL entirely: no headers, no library, static or shared. Everything that touches SSL is compiled out behind a `BJORNE_AGENT_HAS_OPENSSL` definition that the build sets for the core library and its consumers. A disabled build keeps the plain TCP transport, so http endpoints work, and the model client rejects an https endpoint before any connection with an error naming the endpoint. That state serves local models such as llama.cpp on a secure network, and it gives a container image with no crypto dependency at all.

A static OpenSSL build does not include the FIPS approved provider. That matters on FIPS systems.

Static linking is a separate switch from the OpenSSL choice. `BJORNE_AGENT_STATIC` is ON for the static OpenSSL and disabled OpenSSL modes and OFF for shared OpenSSL and coverage builds. When ON, both executables link with `-static`, the deployment binary is stripped, and `ldd` reports no dynamic dependencies. The one thing a static glibc cannot do is call `getaddrinfo`: it needs the NSS modules, which are shared objects, and loading one from a static binary crashes. The static build therefore resolves names itself. Numeric addresses go straight through `inet_pton`. `/etc/hosts` is parsed directly, so aliases and comments behave as expected. Anything else goes to the libc stub resolver through `res_search`, which reads `/etc/resolv.conf`, applies the search domains, and speaks DNS to the configured nameservers. The trade is that `nsswitch.conf`, mDNS, and NSS modules are not consulted, which is a fair price for a binary that starts on a machine with nothing installed. The dynamic build keeps `getaddrinfo` and its full name service switch. Both paths return the same `DNS resolution failed` error shape, and neither crashes on a name that does not exist. The DNS answer parser is a separate unit with its own tests, so the wire format is exercised without a network.

### Context Manager

The context manager implements the context rules. The default overflow strategy is truncate. Compression can run before overflow. Compression is separate from truncation. Compression timing is per model. The compression instruction starts with summarization. A customized instruction offers more control than a hardcoded prompt. The customized instruction comes from a markdown file referenced by configuration, combined with the binary and the config file. The default compression prompt is "Summarize the conversation". A user-configured markdown file can override. Each model entry may carry a compression prompt path.

### Agent Loop

The agent loop implements the loop specification. It assembles context, calls the model, runs tools, and iterates.

The loop stops when the model returns a final response without tool calls. The loop also stops at the iteration cap.

The loop takes an optional observer. It reports each completed tool turn, the final answer, a failure, and an interrupt together with the message list at that moment. The default observer does nothing, so a one-shot run without persistence carries no session code in its path. The loop also takes an optional stop predicate, which it polls before and after each model call. That is the seam a shutdown signal uses, and later the seam an ACP cancellation uses.

### Session Store

The session store is a directory of `<id>.json` files, one per session. The design keeps it to a directory rather than a remote or pluggable backend because the deployment case is a container with a mounted volume: the volume survives the container, and nothing else is needed.

Every write goes to `<id>.json.tmp` and is then published with a rename. On POSIX that is `rename`, which replaces the destination atomically; on Windows it is `MoveFileEx` with the replace and write-through flags, because the C `rename` fails there when the target exists. A process that dies mid-write leaves the previous record intact. The record is rewritten after each turn, not only at exit, so a teardown loses at most the turn in flight.

A session id becomes a file name, so it is restricted to letters, digits, dash, underscore, and dot, and `.` and `..` are refused. The generated form is `<epoch-millis>-<pid>-<sequence>`. A load treats the file name as authoritative, so a damaged record cannot redirect the next save at another session.

The recorder is the loop observer that owns the persistence. It stamps the status and rewrites the file on each event. A failed write is logged and the run continues, and the next event tries again.

Signals are handled by a flag, never by file code. A POSIX signal handler may only set a `sig_atomic_t`, so the handler does that and the loop polls the flag between steps. When the loop sees it, it persists the session as interrupted and returns. Without that split a `SIGTERM` during a write, or during allocation, could corrupt the record it was trying to save.

Resume loads the record, refreshes the system message from the current fragments, and repairs dangling tool calls. A tool call with no result is the normal damage from a kill between the model response and the tool result; the API rejects the sequence, so each missing call gets a synthetic error result. The repaired record is saved before the first model call. A session that already completed prints its answer without contacting the model.

### Tool Layer

The tool layer provides the minimal set from the baseline specification: read file, write file, edit file, run shell command. Tool results feed back into the context.

The registry is dynamic. The four built-ins are registered first. A caller may add a tool, replace one by name, or remove one, and the definitions sent to the model are derived from whatever is registered. The built-in schemas are values assembled in code and serialized by the JSON writer rather than string concatenation. That registration path is what a configuration override or an MCP server uses later; name resolution is by map lookup, so a later registration wins over an earlier one. `stringParameters` and `parseToolArguments` are public for the same reason: a caller that registers `load_skill` or its own tool builds a schema and parses arguments without repeating the JSON work.

The JSON layer has both a parser and a writer. The writer emits compact JSON, escapes strings so a message never contains an embedded newline, prints integral numbers without a decimal point, and rejects a non-finite number because JSON cannot represent one.

## Fulfilment Mapping

The design meets the specifications:

- Minimal dependencies: no third-party C++ libraries, hand written JSON and HTTP, static OpenSSL where needed.
- Static binary: musl static build with -static -s and stripped symbols.
- Configuration: JSON, snake_case, version, agent, models, env var key.
- Context: truncate default, compress before overflow, per model, custom compression instruction from markdown file.
- Loop: stages, stopping conditions, error handling.
- Sessions: local directory, atomic rewrite after each turn, signal flush, resume repair.
- Container: scratch or distroless base, static binary, config via mount or env, binary entrypoint.

## Build and Container Design

- C++20, GCC or Clang.
- Flags: -O2, -std=c++20, -static, -s.
- musl for a true static binary.
- CMake OpenSSL option: static, shared, disabled.
- CMake static link option: `BJORNE_AGENT_STATIC`, ON for the deployment modes.
- Name resolution: `getaddrinfo` when dynamic, `/etc/hosts` plus the stub resolver when static.
- Container base: scratch or distroless. With a fully static binary, scratch.
- Config: mount as a file or pass via environment.
- Entry point: binary.

### Build Environments

Three podman environments, one per link mode, live in `container/`:

- `static`: Alpine 3.22 (musl), static OpenSSL from `openssl-libs-static`, fully static artifact.
- `dynamic`: Debian bookworm-slim (glibc), shared OpenSSL from `libssl-dev`, dynamic artifact.
- `notls`: Alpine 3.22 (musl), no OpenSSL, fully static artifact that rejects https.

Each image creates a `builder` user from `--build-arg UID/GID`, and the run
passes `--userns=keep-id`. Rootless podman maps the caller's uid to container
uid 0, so an image user with the caller's uid only exists inside the container
if that uid is mapped back; `keep-id` does that. The bind mount stays writable,
nothing compiles as root, and the artifacts belong to the caller. The driver
refuses to run as root rather than silently producing root owned output.

The `notls` image still contains shared OpenSSL, because Alpine's `cmake`
package depends on it directly. That does not reach the build: `disabled` skips
`find_package(OpenSSL)`, so no OpenSSL path appears in the cache or the compile
and link lines.

### Runtime Image

`container/Containerfile.runtime` builds the image the harness runs in. It has
two stages. The first is Alpine with the build tools and static OpenSSL. It
compiles a fully static binary and assembles the runtime root. The second stage
is scratch and holds only that root.

The runtime root holds the binary at `/usr/local/bin/bjorne_agent`, the compression
prompt next to the config mount point and in the install layout, a CA bundle at
both `/etc/ssl/cert.pem` and `/etc/ssl/certs/ca-certificates.crt`, busybox with
its applet symlinks, the musl loader and libc that busybox links against, and a
passwd and group file that name a user with uid 1000. The container runs as that
user.

Three things in that root come from the tools rather than the binary. The CA
bundle is needed because the TLS transport calls
`SSL_CTX_set_default_verify_paths`, and the static OpenSSL build looks in
`/etc/ssl`. The busybox shell is needed because the `run_shell` tool calls
`popen`, which runs `/bin/sh`. The passwd file is needed because scratch has no
account database, and the image must not run as root.

Podman writes `/etc/resolv.conf` and `/etc/hosts` into the container even when
the image is scratch, so the stub resolver in the static build reaches the
nameservers without anything in the image.

The binary is the entrypoint, so a run takes the task as its only argument.
The config arrives either as a mount at `/etc/bjorne_agent/bjorne_agent.json`, which is the
default `BJORNE_AGENT_CONFIG` value, or through `BJORNE_AGENT_CONFIG` naming any other
path. A relative `compression_prompt_path` resolves against the directory
holding the config file, so a config mounted outside `/etc/bjorne_agent` needs the
absolute path `/usr/local/share/bjorne_agent/prompts/compression.md`.

## Design Decisions

- Tool source: fixed set for version one. The tool layer provides read file, write file, edit file, run shell command. A config driven tool source is deferred.
- Iteration cap: default 25. The cap may change before release.

## Status

Design draft. This document maps the design to the specifications.
