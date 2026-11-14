# SPEC: C++ Agent Harness

## Purpose

The project creates an agent harness in C++ with minimal dependencies. The goal is a binary and a config file that a docker or podman container can receive and run inside with a minimum of effort. The approach stands apart from general purpose harnesses built on Python, Node, or Rust, which pull large external dependency trees online.

## Goals

- Statically link the binary where possible.
- Keep runtime dependencies minimal, ideally zero third party libraries.
- Ship a binary plus a config file for container use.
- Cover model calls, tool execution, and context handling.

## Version One Scope

- Deliver a vertical slice: task in, model call, tools, result out.
- No third party C++ libraries.
- No dynamic linking.
- No plugin system.
- No broad command line surface.

## Language and Toolchain

- C++20 or later, with version one targeting C++20.
- Compiler: GCC or Clang.
- Static linking via -static -s.
- musl for a true static binary. Static glibc is possible but brings warnings and licensing baggage.
- Strip symbols at build time.

## Dependencies

- No third party runtime libraries.
- JSON: hand rolled parser. The config shape is small enough.
- HTTP: hand rolled client. A static musl binary has no libcurl. The model API is plain REST over HTTPS, so a minimal HTTP/1.1 client plus TLS covers the need.

## Command Line

Reserved verbs: `init`, `run`, `sessions`, `resume`. The bare form is `run`, so `bjorne_agent [config] [task]` and `bjorne_agent run [config] [task]` do the same thing. A first positional argument that matches a reserved verb exactly is a command, so a task that begins with one is passed after `run`: `bjorne_agent run "init the project"`. `--help` prints the usage for the selected command and needs no config file. `--` ends flag parsing. An empty command line is an error: the binary prints the usage and exits `1`.

## Configuration

### Format

JSON. Hand rolled parser. snake_case keys.

### Location

A path given on the command line names the config file. Without one, the run searches `./.bjorne_agent/bjorne_agent.json` and then `$XDG_CONFIG_HOME/bjorne_agent/bjorne_agent.json`, which defaults to `~/.config/bjorne_agent/bjorne_agent.json`. The project local file wins. When no candidate exists, the run stops and names every path it checked.

### Top level

- version: integer, starts at one.
- agent: object.
- models: array of objects.
- mcp_servers: object, optional. Each key is a server name; each value is an object with `command` (string), `args` (array of strings, optional), and `env` (object of strings, optional).

## MCP

Each configured server is spawned as a child process and speaks newline delimited JSON-RPC 2.0 over standard input and standard output. There is no `Content-Length` framing, which is the MCP stdio transport.

The client performs the `initialize` handshake with the protocol version, sends the initialized notification, and lists the tools. A tool is exposed to the model as `mcp__<server>__<tool>`, so it cannot collide with a built-in, and its input schema is passed through unchanged.

A call returns the text content of the response. A tool that reports `isError` becomes a tool error rather than a failed run. A server that crashes or times out is restarted once and the call is retried. A server that cannot start is logged and skipped, and the run continues with the remaining tools.

The runtime image carries no package manager, so a server binary is baked into the image or mounted into `/usr/local/share/bjorne_agent/mcp`, and the configuration names it by an absolute path.

### Agent

- system_prompt: string.
- workspace: string.
- skills_path: string, optional. Directory holding `skills/<name>/SKILL.md`.
- session_dir: string, optional. Directory for persisted sessions; empty disables persistence.
- interactive: boolean, optional, defaults to false. Enables the console interaction channel when standard input is a terminal. With it false, or with piped input, the `request_input` tool fails fast instead of waiting.

### Model

- provider: string, for example openai-compatible.
- endpoint: string, base URL, for example https://api.example.com/v1. The scheme selects the transport: https uses TLS, http uses plain TCP. The host may carry an explicit port, as in http://127.0.0.1:11434/v1. Without a port, https uses 443 and http uses 80.
- model: string, model identifier.
- context_window: integer token count.
- max_tokens: integer token cap per completion.
- temperature: number.
- api_key_env: string, environment variable name holding the key. The key itself stays out of the file.
- priority: integer. Lower value means higher preference. Priority one is the first choice, priority two is the second, priority three is the third, priority thirty is the thirtieth.

## Context Fragments

- AGENTS.md detection: the run searches `~/.config/bjorne_agent/AGENTS.md` plus the workspace root and every parent directory. The global file is first and the nearest file last, so the most specific instruction lands last in the context.
- The total fragment size is capped. Content past the cap is truncated and marked, and later files are dropped.
- Skills use the opencode and Claude Code format: `skills/<name>/SKILL.md` with YAML frontmatter carrying `name` and `description`. The frontmatter scan is minimal and needs no YAML library.
- Only the name and description appear in the system prompt. A `load_skill` tool returns the full instructions on demand.
- AGENTS.md and skills share one fragment mechanism, so ordering, headings, and the size cap are applied the same way.

## Context Management

- Default overflow strategy: truncate.
- Compression is available and can be initiated before overflow. It is a separate path from truncation.
- Summarization is good. A customized compression instruction may be better than a hardcoded prompt.
- A customized instruction comes from a markdown file referenced by configuration, combined with the binary and the config file. The default compression prompt is "Summarize the conversation". A user-configured markdown file can override.
- Compression timing, earlier or later, is per model, because models differ in context performance.

## Build and TLS

- CMake option for OpenSSL: static, shared, or disabled. Static is the default. `BJORNE_AGENT_TLS=none` also disables TLS; either switch removes OpenSSL from the link.
- static: static OpenSSL linked in, suitable for musl static builds.
- shared: shared OpenSSL.
- disabled: no OpenSSL at all. No OpenSSL headers, no OpenSSL library, static or shared. The binary links nothing crypto related. Plain TCP sockets only. http endpoints work; https endpoints are rejected with an error that names the endpoint and the missing support. This state serves local models such as llama.cpp on a secure network.
- CMake option for static linking: `BJORNE_AGENT_STATIC`. It is ON for the static OpenSSL and disabled OpenSSL modes, and OFF for shared OpenSSL and coverage builds. When ON, the binary is linked `-static -s` and has no dynamic dependencies.
- Name resolution: a dynamic build uses `getaddrinfo`. A static build cannot, because static glibc loads the NSS modules as shared objects, so it resolves through `/etc/hosts` and the libc stub resolver, which reads `/etc/resolv.conf` and queries the nameservers directly. `nsswitch.conf`, mDNS, and NSS modules are not consulted in that mode.
- An unresolvable host produces a `DNS resolution failed` error naming the reason. It never crashes the process.

- FIPS note: a static OpenSSL build may not include the FIPS approved provider. This matters on FIPS systems.

## Container

- Base image: scratch or minimal distroless. With a fully static binary, scratch is the target.
- Config: mount as a file or pass via environment. One of the two.
- Entrypoint: the binary.

The runtime image is `scratch`. The config arrives as a mount at `/etc/bjorne_agent/bjorne_agent.json`, which is the default value of the `BJORNE_AGENT_CONFIG` environment variable, or through `BJORNE_AGENT_CONFIG` naming any other path. The API key arrives through the environment variable the config names. The workspace arrives as a volume mount.

The image carries three things beyond the binary, each because a tool needs it: a CA bundle, because the TLS transport verifies the peer against the system trust store; busybox, because the `run_shell` tool calls `popen`, which runs a shell; and a passwd file, because scratch has no account database and the image must not run as root.

## Next Steps

- The configuration and agent loop specifications are finished and the implementation is complete through Phase 3.
- Deployment: publish the runtime image and verify it against the supported endpoints.

## Open Questions

- Priority ordering: lower number means higher preference and earlier attempt. One is first, two is second, three is third, thirty is thirtieth.
- Compression timing: per model.
- Customized compression instruction: resolved. Source is a markdown file referenced by configuration, combined with the binary and the config file. The default compression prompt is "Summarize the conversation". A user-configured markdown file can override.
