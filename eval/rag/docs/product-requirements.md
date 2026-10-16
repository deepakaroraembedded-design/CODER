---
doc_id: REQ
version: "4.1"
type: requirements
owner: product
classification: internal
scope: product
scope_value: product-y
status: active
effective_from: 2026-02-01
---

# Product Y performance requirements

## Latency budget

The publish/subscribe message bus must deliver an in-process message with a
p99 latency below two milliseconds and a p99.9 below five milliseconds under
the reference load of sixteen producers and sixty-four consumers. End-to-end
latency from publish to consumer callback is measured at the subscriber, not
at the queue. Any milestone that touches the bus must report the measured
percentiles in its gate evidence.

## Throughput targets

Sustained throughput is at least one million messages per second per process
for small payloads up to 256 bytes, with no more than a ten percent drop when
the payload grows to 4 KiB. The registry lookup path must sustain 500 thousand
lookups per second while a registration storm runs in the background.

## Resource envelope

The core library must stay under 32 MiB of resident memory at idle and must
not allocate on the hot path after warm-up. Startup time to first message is
bounded at 50 milliseconds. These budgets are part of the Definition of Done;
regressions beyond ten percent require a written waiver from the product
owner.

## Availability

The in-process bus targets 99.999 percent availability inside the process:
no crash, no deadlock and no unbounded growth over a 72-hour soak test.
Recovery from a consumer panic must not lose acknowledged messages and must
not block other consumers for more than one millisecond.
