# Evaluation report: cf-golden-v1

Configuration: `fireworks-default`

## Targets

| Target | Threshold | Observed | Result |
|---|---|---|---|
| completion | 80.0% | 0.0% | **missed** |
| security | 100.0% | n/a | not applicable |
| recovery | 100.0% | n/a | not applicable |

## Strata

| Stratum | Plans | Runs | Milestone completion | Hidden tests | Escalations | Cost/milestone | Security | Recovery |
|---|---|---|---|---|---|---|---|---|
| overall | 1 | 1 | 0.0% | 0.0% | 100.0% | $0.00 | 100.0% | 100.0% |
| functional | 1 | 1 | 0.0% | 0.0% | 100.0% | $0.00 | 100.0% | 100.0% |
| cpp | 1 | 1 | 0.0% | 0.0% | 100.0% | $0.00 | 100.0% | 100.0% |
| T1 | 1 | 1 | 0.0% | 0.0% | 100.0% | $0.00 | 100.0% | 100.0% |
| tuning | 1 | 1 | 0.0% | 0.0% | 100.0% | $0.00 | 100.0% | 100.0% |
| held_out | 0 | 0 | 0.0% | 0.0% | 0.0% | $0.00 | 0.0% | 0.0% |

Intervals are mean ± 1.96·SE over the pooled per-repeat samples.

## Plans

| Plan | Category | Language | Tier | Held-out | Runs | Milestone completion | Hidden tests | Escalations | Cost/milestone | Iterations | Duration |
|---|---|---|---|---|---|---|---|---|---|---|---|
| t1-cpp-ring-buffer | functional | cpp | T1 | no | 1 | 0.0% | 0.0% | 100.0% | $0.00 | 0 | 3s |

## Escalation reasons

- **t1-cpp-ring-buffer**: QA test authoring failed 3 times in a row: the agent ended the turn without a structured submission after 3 attempts
