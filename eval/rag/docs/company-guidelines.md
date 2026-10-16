---
doc_id: CG
version: "3.2"
type: guideline
owner: engineering
classification: internal
scope: company
status: active
effective_from: 2026-01-01
---

# Company coding guidelines

## Error handling

Fallible operations return a typed result value; exceptions never cross a
module boundary. Public functions must document their failure modes in the
header. Error values carry a stable machine-readable code and a human-readable
message; callers branch on the code, never on the message text. When a
lower-level operation fails, wrap it with context instead of propagating a
bare error unchanged, so the final report points at the failing step rather
than the plumbing.

## Resource management

Acquire resources in constructors and release them in destructors; no manual
new and delete in application code. Prefer stack allocation and standard
containers over owning raw pointers. Every file descriptor, socket, lock and
temporary file must have a single obvious owner. Background threads are joined
in the destructor of their owner or via a scope guard.

## Naming and interfaces

Types use CamelCase, functions and variables use snake_case, constants use a
k-prefix. Interfaces are small and explicit: one responsibility per type, and
no boolean parameter that silently changes behaviour. Prefer composition over
inheritance; virtual functions need a documented contract, including thread
safety and ownership of returned references.

## Logging and secrets

Never log credentials, tokens or personal data, not even at debug level. Log
lines include the run identifier and the component name, and are structured
key-value records rather than free-form sentences. Secrets come from the
environment or the secret manager at runtime and are never written to disk,
committed or embedded in test fixtures.

## Testing expectations

Every behaviour change ships with a test that fails before the change and
passes after it. Tests are deterministic: no sleeps, no network, no reliance
on wall-clock ordering; inject a clock when time matters. Bug fixes start with
a regression test reproducing the reported defect.
