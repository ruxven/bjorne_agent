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

## Context Management

- Default overflow strategy: truncate.
- Compression can be initiated before overflow. It is distinct from truncation.
- Compression timing is per model, because models differ in context performance.
- Compression instruction: the default prompt is "Summarize the conversation". A customized instruction may be better. The customized instruction comes from a markdown file referenced by configuration, combined with the binary and the config file. The user can configure more specific behavior in the markdown file.

## Error Handling

The loop handles model errors and tool errors.

- Model error: the loop stops and reports the failure.
- Tool error: the tool result carries the error text. The loop appends the result to the context and continues.

## Status

- Agent loop specification in progress. This file is the working output.
