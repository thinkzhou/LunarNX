## REVIEW-01

- Source doc: `/Users/zhouyang/Downloads/LunarNX_PS_30Mbps_High_Bitrate_Fix_Spec.md`
- Review agent: same-model sub-agent
- Scope checked: PMULL startup/fallback, callback/decode isolation, bounded queue and recovery, diagnostics, Xbox isolation, builds/BSS, simulator and hardware evidence
- Evidence checked: commits through `1eba84d`, diff against `origin/main`, focused tests, Docker build artifacts, CSV claims, and Ryubing app log
- Claim/evidence alignment: mismatches found
- Limited validation honestly reported: yes
- Result: gaps_found
- Gaps:
  - Bounded queue/recovery validation was source-contract-only and did not execute the admission boundaries or stale generation/epoch/reset gating.
  - PMULL TABLE fallback did not emit the explicit warning required by the spec.
- Follow-up issues added: FOLLOWUP-01, FOLLOWUP-02, REVIEW-02
- Assumptions: executable pure-policy tests are the smallest deterministic seam for timing and lifecycle predicates; full decoder concurrency remains covered by build/simulator/hardware validation rather than a fake decoder integration.
- Decision debt: real Switch/PS5 30 Mbps soak and network-disturbance matrix remain outside available automation.
- Human-required blockers: none

## REVIEW-02

- Source doc: `/Users/zhouyang/Downloads/LunarNX_PS_30Mbps_High_Bitrate_Fix_Spec.md`
- Review agent: same-model sub-agent
- Scope checked: REVIEW-01 follow-ups plus original Xbox isolation and bounded oversize behavior
- Evidence checked: commits through `9e7b05b`, production policy integration, executable policy test, focused regressions, Switch build/BSS evidence
- Claim/evidence alignment: mismatches found
- Limited validation honestly reported: yes
- Result: gaps_found
- Gaps:
  - Non-bounded Xbox capacity overflow was accidentally bypassed by the policy refactor.
  - `RejectOversize` could still enqueue a random-access AU after recovery.
- Follow-up issues added: FOLLOWUP-03, FOLLOWUP-04, REVIEW-03
- Assumptions: none
- Decision debt: real hardware soak/disturbance validation remains unavailable.
- Human-required blockers: none
