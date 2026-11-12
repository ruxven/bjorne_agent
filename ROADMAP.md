# ROADMAP: C++ Agent Harness

## Phase 1: Specification Development

- [x] Baseline specification (complete)
- [x] Configuration specification (complete, compression instruction mechanism resolved)
- [x] Agent loop specification (complete draft, error handling closed)

## Phase 2: Design

- [x] Design document (complete draft, open design items closed)

## Phase 3: Implementation

- [x] Build the static binary: `BJORNE_AGENT_STATIC=ON` (default for the deployment modes) links `-static` and `-s`; `ldd` reports no dynamic dependencies.
- [x] Three podman build environments in `container/`: static (Alpine, musl, static OpenSSL), dynamic (Debian, shared OpenSSL), and no TLS (Alpine, no OpenSSL). None compile as root: the builder user takes the caller's uid and gid, and the run uses `--userns=keep-id` so the artifacts belong to the caller.
- [x] Runtime image: `container/Containerfile.runtime` builds a fully static binary in an Alpine stage and copies it into a `scratch` stage with the CA bundle, busybox and the musl loader for `run_shell`, the compression prompt, and a named non-root user. The binary is the entrypoint. `container/build-runtime.sh` builds and smoke tests it.
- [x] Implement hand rolled JSON parser and config loader.
- [x] Implement hand rolled HTTP client and TLS.
- [x] TLS peer verification and SNI: `OpenSslTransport` verifies the peer certificate against the system trust store by default, checks the certificate hostname, and sends the SNI name. `ignore_self_signed` in the model config turns verification off, and `hostname` overrides the name used for SNI and for the hostname check.
- [x] Optional OpenSSL: a `disabled` build links no OpenSSL at all and rejects https endpoints.
- [x] Static name resolution: `/etc/hosts` plus the libc stub resolver, because static glibc cannot call `getaddrinfo`.
- [x] Implement model client.
- [x] Implement context manager.
- [x] Implement agent loop.
- [x] Implement tool layer.
- [x] Config file discovery: the run searches `./.bjorne_agent/bjorne_agent.json` and then `$XDG_CONFIG_HOME/bjorne_agent/bjorne_agent.json` when the command line names no config file. The project local file wins. A single command line argument that names an existing file is the config path, otherwise it is the task.
- [x] Progress output: the run reports each step on stderr, so a slow or stalled request is visible. Levels come from `--verbose`, `--quiet`, and `BJORNE_AGENT_LOG`.
- [x] Package for container use: the config arrives as a mount at `/etc/bjorne_agent/bjorne_agent.json` or through the `BJORNE_AGENT_CONFIG` environment variable, the API key through the variable the config names, and the workspace through a volume mount. The binary is the entrypoint, so the task is the only argument.

## Phase 4: Evolution

The agent is currently a one-shot binary: `main` reads a task, `AgentLoop::run` holds every
message in a local vector, the result is printed, the process exits. The items below assume a
long-lived, stateful, interactive agent, so they are sequenced by dependency rather than by appeal.

Three prerequisites are shared by most of them: a subcommand dispatch, a JSON writer, and a
dynamic tool set. The configuration gains `agent.skills_path`, `agent.session_dir`,
`agent.interactive`, and an `mcp_servers` map.

### Phase A: Foundation

- [x] Subcommand dispatch. Reserved verbs: `init`, `run`, `sessions`, `resume`. The bare form stays
  valid, and a task that begins with a reserved word is spelled `bjorne_agent run "init the project"`.
- [x] JSON writer for `json::Value`, which today has the full type set but no serializer. The tool
  schema builder in `Tools.cpp` should use it instead of string concatenation.
- [x] Dynamic tool set. `ToolRegistry::definitionsJson` becomes an instance method and the tool
  list is assembled at runtime from the configuration plus the registered extensions.
- [x] Configuration extension for the new fields.

### Phase B: Context fragments

- [ ] AGENTS.md detection. Search the workspace root and its parents, plus a global
  `~/.config/bjorne_agent/AGENTS.md`. Concatenate global first and nearest last, so the most
  specific instruction lands last in the context, and cap the total size.
- [ ] Skills from a configured directory. The format matches opencode and Claude Code:
  `skills/<name>/SKILL.md` with YAML frontmatter carrying `name` and `description`, so an existing
  skills directory works unmodified. Only the name and description pairs go into the system prompt;
  a `load_skill` tool returns the full body on demand. The frontmatter needs a minimal `key: value`
  scanner rather than a YAML library, to keep the no dependency guarantee.
- [ ] A shared fragment mechanism, so AGENTS.md and skills are both assembled the same way.

### Phase C: Session persistence

- [ ] A local session store on a directory, by default a mounted volume. No remote store and no
  pluggable backend interface.
- [ ] Atomic writes through a temporary file and a rename, because abrupt teardown is the case
  being designed for.
- [ ] Incremental writes after each turn rather than only at exit, so a killed container does not
  lose the whole session.
- [ ] A flush on SIGTERM and SIGINT.
- [ ] Repair of dangling tool calls on resume. If the container dies after the model emitted
  `tool_calls` but before the tool result was recorded, the stored assistant message has no matching
  `tool` response and the API rejects the sequence.
- [ ] Graceful degradation. With no `session_dir` the persistence is off and the behaviour matches
  the current one-shot run.

### Phase D: Interaction

- [ ] A pluggable interaction channel, because in a container stdin is the task and there is no
  human. The default channel is unavailable and the tool fails fast with a clear message.
- [ ] A console channel that prompts on the terminal, with `isatty(STDIN_FILENO)` detection. A
  hanging container is worse than a failed one, so the tool never blocks indefinitely.
- [ ] A request shape of `question` plus `options`, so the model can offer a constrained choice and
  the answer returns as the tool result.
- [ ] Plain prompts rather than a full-screen TUI, which suits a constrained console and keeps the
  static minimal build.

### Phase E: MCP

- [ ] A stdio client speaking newline delimited JSON-RPC. No Content-Length framing and no
  dependencies.
- [ ] The `initialize` handshake, then `tools/list` and `tools/call`.
- [ ] Namespaced tool names of the form `mcp__<server>__<tool>`, so they cannot collide with the
  built-in tools.
- [ ] Bidirectional pipes and process spawning. The existing `run_shell` uses a one way `popen` and
  cannot be reused.
- [ ] Recovery from a server crash, a timeout and a restart, so a dying server does not take the
  agent down.
- [ ] The server binaries baked into or mounted into the scratch runtime image. This affects the
  image build and not only the code.
- [ ] The JSON-RPC transport comes from the shared core described below, so the MCP client and the
  ACP server are not two separate implementations of the same wire format.

### Phase F: ACP client mode

The Agent Client Protocol is what puts the agent in front of an editor such as Zed, Neovim or
JetBrains. The editor launches the agent as a subprocess and drives it over JSON-RPC 2.0 on stdin
and stdout, with messages delimited by newlines.

- [ ] An `acp` subcommand. It replaces the one-shot entry point with a request loop that reads
  JSON-RPC from stdin and writes JSON-RPC to stdout.
- [ ] The agent side methods: `initialize`, `session/new`, `session/prompt`, `session/cancel`, and
  `session/load` when a session store is configured.
- [ ] The client side methods the agent calls back into: `fs/read_text_file`,
  `fs/write_text_file`, `session/request_permission`, `elicitation/create`, and the `terminal/*`
  family when the client advertises them.
- [ ] The `session/update` notifications: `agent_message_chunk`, `agent_thought_chunk`, `tool_call`,
  `tool_call_update`, `plan` and `available_commands_update`.
- [ ] Version negotiation in `initialize`. Answer with the requested protocol version when it is
  supported, otherwise the latest one the agent supports. The draft v2 is not implemented.
- [ ] Capability advertisement driven by the configuration: `loadSession` only when a session store
  exists, the prompt capabilities that are genuinely supported, and no MCP HTTP or SSE capability
  while only the stdio MCP transport exists.
- [ ] stdout stays reserved for protocol messages. The progress output already goes to stderr, and
  the result printing of the one-shot mode is suppressed in this mode.
- [ ] The workspace comes from `session/new` and overrides the configured workspace. The MCP servers
  the client supplies override the configured ones for that session.
- [ ] The file tools route through the client when it advertises the file system capability, and
  fall back to the local filesystem when it does not. The unsaved editor buffer is only reachable
  through the client.
- [ ] A permission request maps onto the interaction channel from Phase D, so the constrained choice
  shape is reused rather than reinvented.
- [ ] Cancellation. `session/cancel` has to interrupt a turn that is already running, which needs a
  cooperative cancellation flag in the agent loop. The loop is single threaded today, so this is
  the first place where a running turn has to observe outside input.
- [ ] Session continuity. `session/load` and the stable `session/resume` map onto the session store
  from Phase C, and the ACP session identifier maps onto a stored session identifier.
- [ ] The stable `session/list` and `session/delete` methods map onto the same store and are cheap
  once it exists.
- [ ] Session configuration options expose the configured models, so the user picks the model from
  the editor the same way the model list is picked today.
- [ ] A conformance check against the ACP test compatibility kit, so the implementation is
  verified against the protocol rather than against a hand written expectation.

### Shared JSON-RPC core

Phase E and Phase F both speak JSON-RPC 2.0 and both need the same transport mechanics. They should
share one implementation rather than two.

- [ ] One newline delimited JSON-RPC codec. The ACP framing and the MCP stdio framing are the same,
  which is why the codec is written once.
- [ ] One endpoint component with a reader thread, a map of pending requests and a write mutex. It
  serves both roles, as the client towards an MCP server and the server towards an ACP editor.
- [ ] The JSON writer escapes newlines inside strings, since a protocol message must not contain an
  embedded newline.

### Tool set customization

- [ ] The built-in tools stay overridable. The configuration may disable a built-in tool, or
  replace it with an implementation of the user's own, and the MCP tools sit alongside them in the
  same namespace.
- [ ] A single resolution order for the tool set: the built-ins, then the configuration overrides,
  then the MCP tools. A configured override wins over the built-in of the same name, and the
  resolved set is what the model is shown.
- [ ] Overrides are expressed without a scripting runtime, so the guarantee that the binary links no
  new dependencies still holds.

### Deliberately out of scope

- [ ] A full-screen TUI. The dependency surface is large, it breaks the static minimal goal, and a
  constrained console does not want one.
- [ ] Subagents. The loop stays single threaded per turn.
- [ ] A remote session store or a pluggable backend interface. The local directory is enough for
  now, and the interface would cost more than it saves.
- [ ] The ACP v2 draft. The v1 protocol is implemented and the draft is watched, not followed.
- [ ] The ACP HTTP and WebSocket transport. The stdio transport is the one the editors use today.
- [ ] Terminal authentication and ACP agent registry registration. The agent authenticates through
  the API key environment variable and is launched by the editor directly.

### Spec impact

- [ ] `SPEC.md` gains the subcommand semantics, the MCP scope and the ACP mode.
- [ ] `SPEC.CONFIG.md` gains `skills_path`, `session_dir`, `interactive`, `mcp_servers` and the tool
  override section.
- [ ] `SPEC.LOOP.md` gains the session lifecycle, the resume semantics, the dangling call repair and
  the cooperative cancellation.
- [ ] `DESIGN.md` gains the shift from a one-shot to a long-lived stateful agent, along with the
  fragment mechanism, the interaction channel abstraction, the shared JSON-RPC core and the two
  protocol roles the agent plays in it.

## Quality Gates

- [x] Catch2 suite: `bjorne_agent_tests` (150 cases, 552 assertions with TLS, 148 cases and 556 assertions without TLS).
- [x] Lizard complexity gate: `-C 15 -L 100 -a 5`, no thresholds exceeded in any configuration.
- [x] Line and function coverage: `./run_coverage.sh`, reports in `bjorne_agent/coverage-reports/`. Line coverage is 96% with TLS and 97% in both no TLS configurations, and the function level coverage agrees: 96% with TLS and 97% in both no TLS configurations. Set `BJORNE_AGENT_OPENSSL=disabled` to measure the no TLS configuration, and add `BJORNE_AGENT_STATIC=ON` to measure the static one. Below 80% function coverage only the two out-of-line virtual destructors and the TLS transport helpers remain: the TLS handshake and send paths need a peer that completes the session, and the no TLS builds cannot run the OpenSSL constructor and factory at all. Residual line gaps are the OS allocation failures inside the transports and the getaddrinfo success branches that need a live name server.
- [x] Transport tests against a loopback HTTP/TLS server, including read and handshake failure paths.
- [x] DNS answer parser tests built from crafted packets, so the wire format is checked without a network.
- [x] CodeChecker static analysis: clangsa and clang-tidy over `src/`, no findings in project code. Third party Catch2 is excluded through `.codechecker.skip`.
