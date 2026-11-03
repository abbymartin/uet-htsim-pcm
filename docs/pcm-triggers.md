# PCM Trigger State Machine

How a PCM trigger decides when to call the handler, as implemented in
`htsim/sim/pcm/pcm_trigger.{h,cpp}` and `pcm_signal_state.{h,cpp}`. It follows
UE spec v1.1 §3.6.18.5. Where it differs from the spec, see
[pcm-plan.md](pcm-plan.md) §7.

Each signal entry in an algorithm's YAML file can have one trigger. A trigger
watches that entry's accumulator and is configured by:

| YAML field | Meaning |
|---|---|
| `op`, `op_val` | Comparison `accumulator (op) op_val`; `wc` is always true |
| `trigger_type` | `raw` compares the value; `delta` / `magnitude` (from the PCM paper) compare the change since the last true comparison |
| `mode_refval: C T` | Fire after `C` true comparisons, or once `T` × 128 ns have passed since it last fired |
| `arm_mode` | `once` or `continuous` (the spec's `disarmed` is rejected; see below) |
| `action` | `invoke` calls the handler; `record` only sets the trigger's bit in the mask |
| `invoke_deadline` | For `invoke`: how long the call may be delayed after firing (0 = call now) |

## Arm states

```mermaid
stateDiagram-v2
    direction LR
    [*] --> Armed

    Armed --> Armed: evaluated, did not fire
    Armed --> Armed: fired, continuous<br/>(count and timer reset)
    Armed --> Disarmed: fired, once

    Disarmed --> Disarmed: evaluated (never fires)
```

Every trigger starts armed. The spec lets a handler change, re-arm or disarm
triggers at runtime with `uet_modify_trigger()`, but that isn't implemented
here (plan §7 item 20): a trigger's configuration comes only from the YAML file.
So a `once` trigger fires a single time and then stays disarmed, and
`arm_mode: disarmed` is rejected when loading, since such a trigger could never
fire.

## What one evaluation does

A trigger is evaluated when an event (a received or sent packet, or a timer
event) updated its own signal (plan §7 item 2).

```mermaid
flowchart TD
    E([Trigger's signal was updated]) --> A{Armed?}
    A -- no --> N([Not fired])
    A -- yes --> Z{"mode_refval is 0 0?"}
    Z -- yes --> F
    Z -- no --> CC{"C > 0?"}

    CC -- yes --> CMP{"Comparison true?<br/>raw: value op op_val<br/>delta: value − baseline op op_val<br/>magnitude: |value − baseline| op op_val"}
    CMP -- yes --> INC["count += 1<br/>delta / magnitude: baseline = value"]
    INC --> CR{"count ≥ C?"}
    CMP -- no --> TT
    CR -- no --> TT
    CR -- yes --> F
    CC -- no --> TT

    TT{"T > 0 and<br/>now − last fire ≥ T?"}
    TT -- yes --> F
    TT -- no --> N

    F([Fired]) --> M{arm_mode}
    M -- once --> D["Disarm"]
    M -- continuous --> R["count = 0<br/>last fire = now"]
```

Notes:
- `mode_refval: 0 0` fires on every evaluation without comparing: the spec's
  "no evaluation is performed; trigger fires immediately if armed".
- The time condition doesn't use the comparison: it's "elapsed time since the
  trigger last fired". Before the first fire, time counts from when the trigger
  was created.
- The time condition is only checked when the trigger is evaluated. For a
  trigger on a `uet_time_*` signal, the runtime schedules timer events using
  `PcmTrigger::timeDue()`; a trigger on an ACK or NACK signal is checked at the
  next ACK or NACK.

## One event, all triggers

`PcmSignalState::applyEvent()` handles one event for a CCC:

```mermaid
flowchart TD
    EV([Event: list of signal updates]) --> U["Apply every update to every<br/>accumulator of that signal"]
    U --> L["For each updated entry with a trigger:<br/>evaluate it"]
    L --> Q{Fired?}
    Q -- no --> NX[Next entry]
    Q -- yes --> B["Set the entry's bit in the mask"]
    B --> ACT{action}
    ACT -- record --> NX
    ACT -- invoke --> DL{"invoke_deadline = 0?"}
    DL -- yes --> NOW["Call the handler now"]
    DL -- no --> BY["Call the handler by<br/>min(now + invoke_deadline)"]
    NOW --> NX
    BY --> NX
```

All of an event's updates are applied before any trigger is checked, so updates
from one packet take effect together (§3.6.18.4.1). A trigger is evaluated at
most once per event, even if the event updated its signal twice.
