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
- A permission gate refuses a tool call. The turn ends as a refusal and the reason is reported.
- A cancellation flag is observed. The turn ends as cancelled.

Every stop has a reason, and the reason is available to the caller through `stopReason()`: `end_turn`, `refusal`, `cancelled`, `max_tokens` or `max_turn_requests`.

## Cancellation

Cancellation is cooperative. The loop never blocks on a signal handler, and a signal handler never does more than set a flag.

- The loop polls the flag before and after every model call, so a turn that is waiting on the model stops as soon as the call returns.
- A permission gate is a callback the caller supplies. It receives the tool name, the arguments and the tool call id, and returns false with a reason when the tool must not run. The gate runs before the tool, so a refused tool never reaches the filesystem or the shell.
- A cancelled turn takes precedence over a refusal: when the flag is set, the reason is `cancelled` rather than `refusal`.
- The refusal path and the cancellation path are both observable through the same stop reason, so an ACP editor learns from one field why a turn ended.

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
