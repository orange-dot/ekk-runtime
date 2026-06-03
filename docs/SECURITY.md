<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
<!-- Copyright (c) 2026 mamut-studio.com -->

# Security Notes

The repository-scoped Codex Security threat model for the current local scan is:

```text
/tmp/codex-security-scans/ekk-runtime/threat_model.md
```

Treat the runtime as an embedded coordination library, not an Internet service.
Integrity, availability, bounded execution, and physical safety are the primary
security properties.

Production deployments must provide the controls that this library cannot infer
from C structs alone:

- transport sender IDs are bound to hardware identity
- messages are authenticated where Byzantine behavior matters
- shared field memory prevents one module from publishing another module's slot
- module IDs are unique, non-zero, and below `EKK_MAX_MODULES`
- module ticks are single-threaded and receive monotonically increasing time
- application callbacks enforce safety policy before real actuation
