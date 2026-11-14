# SPEC.CONFIG: C++ Agent Harness Configuration Specification

## Purpose

This document specifies the configuration file for the C++ agent harness. The file is JSON. The parser is hand rolled. Keys use snake_case.

## Scope

The configuration file defines the agent behavior and the model list used by the agent loop.

## Overview

The configuration file has three top level sections: version, agent, and models.

## Config File Location

A path given on the command line is used as it is. Without one, the run reads the BJORNE_AGENT_CONFIG environment variable. When that variable holds a path, the run uses it as the config path and skips the search. A variable that holds only whitespace counts as unset. A variable that names a file that is not there stops the run and names both the variable and the path, because the caller asked for that file.

Without the environment variable, the run searches these locations in order and takes the first that exists:

- ./.bjorne_agent/bjorne_agent.json, the project local file.
- $XDG_CONFIG_HOME/bjorne_agent/bjorne_agent.json, the user level file. When XDG_CONFIG_HOME is unset or relative, the path is ~/.config/bjorne_agent/bjorne_agent.json.

The project local file wins, so a repository can carry its own settings without touching the user configuration. When no candidate exists, the run stops and names every path it checked.

The environment variable is the path a container uses. The config is mounted at a path the search does not know about, so the variable names it.

## Top Level

- version: integer. Starts at one. Marks the schema revision.
- agent: object. Holds agent behavior fields.
- models: array. Holds model entries.

## Agent

- system_prompt: string. Instruction content sent to the model.
- workspace: string. Workspace root for tool operations.
- skills_path: string, optional. Directory holding `skills/<name>/SKILL.md` entries. A relative path resolves against the directory holding the config file. Empty disables skill discovery.
- session_dir: string, optional. Directory for persisted sessions. A relative path resolves against the directory holding the config file. Empty disables persistence and keeps the one-shot behaviour.
- interactive: boolean, optional. Defaults to false. Reserved for the interaction phase.

## Models

Each entry in the models array:

- provider: string. Provider identifier, for example openai-compatible.
- endpoint: string. Base URL, for example https://api.example.com/v1. The scheme selects the transport: https uses TLS, http uses plain TCP for a local model such as llama.cpp. The host may carry an explicit port, for example http://127.0.0.1:11434/v1. Without a port, https uses 443 and http uses 80. IPv6 hosts use brackets, for example http://[::1]:8080/v1. The path is a prefix: the request goes to the endpoint path plus /chat/completions, so the example above posts to /v1/chat/completions.
- model: string. Model identifier.
- context_window: integer. Token count for the model context.
- max_tokens: integer. Token cap per completion.
- temperature: number. Sampling parameter.
- api_key_env: string. Environment variable name. The key stays in the environment, not in the file.
- priority: integer. Lower means higher preference. Lower values are tried first.
- compression_prompt_path: string, optional. Path to a markdown file holding the compression instruction. When set, the file content is the compression instruction. When absent, the default is "Summarize the conversation". A relative path is resolved against the directory holding the config file, not the working directory.
- ignore_self_signed: boolean, optional. Defaults to false. When false, the TLS peer certificate is verified against the system trust store, and the certificate hostname is checked. When true, peer verification is turned off entirely: the chain is not checked and the hostname is not checked either. Use it only against a host with a self-signed certificate, for example a private gateway.
- hostname: string, optional. The name sent in the TLS SNI extension and matched against the peer certificate. When absent, the host from the endpoint is used. Set it when the endpoint host is an address or an internal name but the certificate carries a different name.

Priority semantics: priority one is the first choice, priority two is the second, priority three is the third, priority thirty is the thirtieth.

## TLS

Certificate verification is on by default. The chain is checked against the system trust store, and the expected hostname is the `hostname` key when present, otherwise the endpoint host. A rejected certificate ends the run with the verification reason, for example "Certificate verification failed: self-signed certificate".

Turning `ignore_self_signed` on skips both the chain check and the hostname check. The API key travels over this connection, so a build that trusts arbitrary certificates can hand the key to a wrong peer.

## Context Management

- Default overflow strategy: truncate.
- Compression can be initiated before overflow. It is distinct from truncation.
- Compression timing is per model, because models differ in context performance.
- Compression instruction: the default prompt is "Summarize the conversation". A customized instruction may be better. The customized instruction comes from a markdown file referenced by configuration, combined with the binary and the config file. The user can configure more specific behavior in the markdown file.

## Decisions

- Config format: JSON, hand rolled parser, snake_case.
- Overflow default: truncate. Compression available before overflow.
- Key source: environment variable named by api_key_env.
- Model selection: array with priority ordering, lower value first.
- Compression instruction source: markdown file referenced by configuration, combined with the binary and the config file. The default compression prompt is "Summarize the conversation". A user-configured markdown file can override.
- Certificate verification: on by default. `ignore_self_signed` turns it off, and `hostname` sets the name used for SNI and for the certificate hostname check.

## Open Questions

- None. The mechanism for customized compression instructions is resolved. See Decisions.

## Status

- Configuration specification. The compression instruction mechanism is resolved. This file is the working output.
