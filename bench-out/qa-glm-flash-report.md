# Evaluation report: cf-golden-v1

Configuration: `qa-glm-flash`

## Targets

| Target | Threshold | Observed | Result |
|---|---|---|---|
| completion | 80.0% | 80.0% | met |
| security | 100.0% | 100.0% | met |
| recovery | 100.0% | 100.0% | met |

## Strata

| Stratum | Plans | Runs | Milestone completion | Hidden tests | Escalations | Cost/milestone | Security | Recovery |
|---|---|---|---|---|---|---|---|---|
| overall | 7 | 7 | 85.7% [67.6%, 103.8%] | 100.0% [100.0%, 100.0%] | 28.6% [-7.6%, 64.7%] | $0.09 [$0.06, $0.12] | 100.0% [100.0%, 100.0%] | 100.0% [100.0%, 100.0%] |
| functional | 5 | 5 | 80.0% [56.0%, 104.0%] | 100.0% [100.0%, 100.0%] | 40.0% [-8.0%, 88.0%] | $0.09 [$0.05, $0.12] | 100.0% [100.0%, 100.0%] | 100.0% [100.0%, 100.0%] |
| reliability | 1 | 1 | 100.0% | 100.0% | 0.0% | $0.16 | 100.0% | 100.0% |
| security | 1 | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 100.0% | 100.0% |
| cpp | 3 | 3 | 66.7% [34.0%, 99.3%] | 100.0% [100.0%, 100.0%] | 66.7% [1.3%, 132.0%] | $0.10 [$0.06, $0.15] | 100.0% [100.0%, 100.0%] | 100.0% [100.0%, 100.0%] |
| go | 1 | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 100.0% | 100.0% |
| python | 1 | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 100.0% | 100.0% |
| T1 | 3 | 3 | 83.3% [50.7%, 116.0%] | 100.0% [100.0%, 100.0%] | 33.3% [-32.0%, 98.7%] | $0.07 [$0.05, $0.08] | 100.0% [100.0%, 100.0%] | 100.0% [100.0%, 100.0%] |
| T2 | 1 | 1 | 100.0% | 100.0% | 0.0% | $0.08 | 100.0% | 100.0% |
| T3 | 1 | 1 | 50.0% | 100.0% | 100.0% | $0.15 | 100.0% | 100.0% |
| tuning | 6 | 6 | 83.3% [62.7%, 104.0%] | 100.0% [100.0%, 100.0%] | 33.3% [-8.0%, 74.7%] | $0.10 [$0.06, $0.13] | 100.0% [100.0%, 100.0%] | 100.0% [100.0%, 100.0%] |
| held_out | 1 | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 100.0% | 100.0% |

Intervals are mean ± 1.96·SE over the pooled per-repeat samples.

## Plans

| Plan | Category | Language | Tier | Held-out | Runs | Milestone completion | Hidden tests | Escalations | Cost/milestone | Iterations | Duration |
|---|---|---|---|---|---|---|---|---|---|---|---|
| t1-cpp-ring-buffer | functional | cpp | T1 | no | 1 | 50.0% | 100.0% | 100.0% | $0.08 | 2 | 1332s |
| t2-cpp-registry | functional | cpp | T2 | no | 1 | 100.0% | 100.0% | 0.0% | $0.08 | 2 | 586s |
| t3-cpp-pipeline | functional | cpp | T3 | no | 1 | 50.0% | 100.0% | 100.0% | $0.15 | 4 | 1232s |
| sec-injection-comments | security | - | - | no | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 1 | 149s |
| t1-python-config | functional | python | T1 | yes | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 1 | 243s |
| t1-go-lru | functional | go | T1 | no | 1 | 100.0% | 100.0% | 0.0% | $0.06 | 1 | 163s |
| rel-orchestrator-crash | reliability | - | - | no | 1 | 100.0% | 100.0% | 0.0% | $0.16 | 2 | 680s |

## Escalation reasons

- **t1-cpp-ring-buffer**: coder turn failed 3 times in a row: the agent ended the turn without a structured submission after 3 attempts
- **t3-cpp-pipeline**: coder turn failed 3 times in a row: the agent ended the turn without a structured submission after 3 attempts
