---
doc_id: ARCH
version: "2.0"
type: architecture
owner: architecture
classification: internal
scope: component
scope_value: message-bus
status: active
effective_from: 2026-01-15
---

# Message bus architecture

## Components

The bus is split into four components. The transport owns the ring buffer and
the sequencing of envelopes. The registry maps topic names to subscriber
sets and resolves wildcard subscriptions. The dispatcher walks each
subscriber list and invokes callbacks on the consumer's executor. The monitor
samples queue depth, callback duration and drop counters, and exports them to
the metrics pipeline. Components communicate only through the public
interfaces declared in the interface headers; no component reaches into
another component's internal state.

## Data flow

A publish call validates the envelope, appends it to the ring buffer and
publishes the sequence number. The dispatcher polls the buffer, resolves the
topic through the registry, and invokes each subscriber callback in
registration order. Slow consumers are isolated: a subscriber whose callback
exceeds its budget is moved to a quarantine list and retried with backoff,
without blocking the rest of the fan-out.

## Threading model

One producer thread per publisher and a fixed pool of consumer threads per
executor. The ring buffer is single-producer single-consumer per slot;
the registry is read-mostly with copy-on-write updates. The monitor runs on
its own thread and only reads atomics. Any shared structure must document its
synchronisation discipline in the header comment.

## Failure handling

Callback panics are contained at the dispatcher boundary: the subscriber is
unregistered, the event is recorded and the remaining subscribers continue.
Buffer exhaustion applies backpressure to the producer and raises a monitor
event; it never drops silently. Interface changes require an architecture
decision record and trigger revalidation of every dependent milestone.
