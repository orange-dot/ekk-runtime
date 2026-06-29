<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 mamut-studio.com -->

# Electrokombinacija EK3 to ekk-runtime Mapping

Status: prototype integration map. `ekk-runtime` is treated here as a
host-buildable candidate for ROJ_COORD / SWARM CORE behavior, not as production
JEZGRO firmware, not as a safety-certified controller, and not as proof of a
deployed CAN-FD wire protocol.

## Relationship

`workspace/systems/elektrokombinacija-hw-sim` owns the EK3 hardware design
intent: LLC converter studies, KiCad capture, normalized SPICE experiments, and
copied source material. That repository states the charger-module vocabulary and
the bus-depot scaling targets.

`workspace/systems/ekk-runtime` owns the coordination-runtime prototype: modules,
task scheduling, k-neighbor topology, decaying shared fields, heartbeat/failure
detection, and threshold consensus. It is the right place to make the ROJ/SWARM
coordination idea executable on the host before any embedded adapter exists.

## Design-Intent Constants

The current integration harness uses these values from the EK3 source material:

| EK3 value | Runtime use |
| --- | --- |
| 3.3 kW continuous module power | Normalization denominator for load/power fields |
| 84 modules per rack | Full-rack host simulation size |
| 277 kW nominal rack class | Derived from 84 x 3.3 kW |
| 650 V DC input / 50-500 V DC output | Documented electrical context only |
| STM32G474 / CAN-FD | HAL-adapter target, not implemented by this harness |

These values are copied into the host harness intentionally. The harness does
not parse Markdown or schematic data at runtime.

## Concept Mapping

| EK3 / ROJ concept | `ekk-runtime` surface | Notes |
| --- | --- | --- |
| Charger module | `ekk_module_t` | One runtime module per EK3 power module |
| Module identifier | `ekk_module_id_t` | Host harness uses module IDs 1..N |
| Rack / local swarm | `ekk_topology_t` | k-neighbor topology with `EKK_K_NEIGHBORS=7` |
| ROJ heartbeat | `EKK_MSG_HEARTBEAT` / `ekk_heartbeat_t` | Runtime heartbeat is not yet a CAN-FD frame schema |
| Module status field | `EKK_MSG_FIELD` / `ekk_field_t` | Decaying shared state across neighbors |
| Power-limit negotiation | `EKK_PROPOSAL_POWER_LIMIT` | Exposed by `ekk_module_propose_power_limit` |
| Vote / agreement | `EKK_MSG_VOTE` / `ekk_consensus_t` | Threshold consensus primitive |
| Fault / alert frame | Future HAL adapter, likely `EKK_MSG_USER_BASE` | No production fault vocabulary is claimed here |
| CAN-FD connector | `ekk_hal_t` boundary | The adapter is a future hardware-facing layer |

## Field Mapping

The first executable mapping keeps the fields deliberately small:

| Runtime field | EK3 interpretation |
| --- | --- |
| `EKK_FIELD_LOAD` | Output load normalized by 3300 W |
| `EKK_FIELD_THERMAL` | Normalized thermal headroom or measured temperature band |
| `EKK_FIELD_POWER` | Delivered power normalized by 3300 W |
| `EKK_FIELD_SLACK` | Reserved for service-deadline or scheduling slack |
| `EKK_FIELD_CUSTOM_0` | Reserved for EK3 fault-class tags |
| `EKK_FIELD_CUSTOM_1` | Reserved for EK3 energy/session tags |

The host harness currently updates load, thermal, and power. Fault classes and
energy/session accounting need a CAN-FD adapter vocabulary before they should be
treated as stable.

## Cadence Boundary

The EK3 material mentions multiple cadences, including fast status updates and
slower ROJ heartbeat behavior. `ekk-runtime` defaults are currently a 10 ms
heartbeat period and 100 ms field decay. The host harness models the runtime
tick at 10 ms and direct-discovery topology setup; it does not claim a final
wire cadence for CAN-FD.

## Executable Harness

`examples/ek3_rack.c` provides the first buildable integration point:

```sh
cmake --build build --target ekx_ek3_rack
./build/ekx_ek3_rack --modules 7
./build/ekx_ek3_rack --modules 84
```

The default runtime build keeps `EKK_MAX_MODULES=64`. The EK3 rack harness links
against a separate `ekx_core_ek3` target compiled with `EKK_MAX_MODULES=96`, so
the normal public defaults are unchanged while the 84-module rack case can run.

Expected success lines:

```text
EK3_RACK_OK modules=7
EK3_RACK_OK modules=84
```

## Next Integration Step

The next useful slice is a small HAL adapter contract:

- define EK3 CAN-FD frame IDs and payload layouts;
- map EK3 status frames into `ekk_field_t` updates;
- map EKK proposals/votes into ROJ coordination frames;
- keep safety interlocks outside the host harness until there is hardware
  evidence and a certification boundary.
