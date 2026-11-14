# SPEC.LOOP: Agent Loop Specification

## Purpose

This document specifies the agent loop for the C++ agent harness.

## Overview

The loop takes a task, builds context, calls the model, runs tools, and repeats until it stops.

## Loop Stages

### Context Assembly

The loop gathers the system prompt, the task, and the conversation history into one context.

### Model Call

The loop sends the context to the model. The model returns a response. The response may carry tool calls. The response may be a final answer.

### Tool Execution

The loop runs the requested tools. The loop captures results and appends them to the context.

### Iteration

The loop appends the model response and tool results, then returns to the model call.

## Stopping Conditions

- The model returns a final response without tool calls.
- The iteration cap is reached.
- A shutdown signal is observed before or after a model call. The loop stops and persists the session as interrupted.

## Session Persistence

When a session directory is configured, the loop's conversation is persisted after every turn, so an abrupt teardown loses at most the turn that was still running.

- The store writes one `<id>.json` per session through a temporary file and an atomic rename, so a process that dies mid-write leaves the previous complete record.
- A run begins with status `running`. A final answer stores `complete` with the answer. A model or loop failure stores `error` with the reason. A shutdown signal stores `interrupted`.
- The stored messages are the exact sequence the loop used, including the system message and every tool result, so a resume can continue the conversation without reconstructing it.
- If the process stops after the model emitted tool calls but before the results were recorded, resume finds the dangling calls and inserts a synthetic error result for each. Without that repair the API rejects the assistant message because its tool calls have no matching responses.
- Resume refreshes the stored system message from the current configuration and context fragments, and repairs and saves the conversation before the first model call. A session whose status is `complete` prints its stored answer without a model call.
- With no session directory the persistence is off, and the behaviour is the one-shot run.

## Context Management

- Default overflow strategy: truncate.
- Compression can be initiated before overflow. It is distinct from truncation.
- Compression timing is per model, because models differ in context performance.
- Compression instruction: the default prompt is "Summarize the conversation". A customized instruction may be better. The customized instruction comes from a markdown file referenced by configuration, combined with the binary and the config file. The user can configure more specific behavior in the markdown file.

## Error Handling

The loop handles model errors and tool errors.

- Model error: the loop stops and reports the failure.
- Tool error: the tool result carries the error text. The loop appends the result to the context and continues.
- Shutdown: a signal handler only sets a flag. The loop observes it between steps, saves the session as interrupted, and returns an error naming the interruption.

## Status

- Agent loop specification in progress. This file is the working output.
