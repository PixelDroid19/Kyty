# Contract gate (before guest semantics change)

Promotion from `observed` → `implementing` requires a written evidence packet.
If any row is empty and matters to the caller, status stays `blocked`.

## HLE export

| Field | Required |
|---|---|
| NID + library + module version | From registration table or import trace |
| Export name | Catalog or disassembly; never decode NID string blindly |
| SysV signature | rdi/rsi/rdx/rcx/r8/r9 + return width |
| Guest error codes | Observed checks on return or `*errno` |
| Output buffers | Sizes, alignment, partial writes, null rules |
| Side effects | Handles, global state, sync, GPU submission |
| Test | Failing unit or integration case with sanitized args |

## PM4 / GPU

| Field | Required |
|---|---|
| Packet type and dword layout | Bytes or builder return at encode time |
| Register meaning | Hardware doc + capture; unknown → structured EXIT |
| Encode vs execute | Same decoder for direct/indirect |
| Layout | Single descriptor→layout function for all consumers |
| Test | Sanitized packet fixture in GraphicsPackets/State tests |

## Host-only (loader, pthread, memory)

| Field | Required |
|---|---|
| Guest-visible contract | What the title's code expects |
| Producer | Who should have written bad state |
| Falsifier | What observation would disprove the fix |
| Test | Integration or unit test; gdb optional for first capture |

## Anti-patterns (reject at gate)

- "Probably wchar" / "likely TLS" without a log line or register dump.
- Null-check at reader without identifying the writer.
- Stub returning OK to reach the next missing import (unless documented
  bring-up discovery run, not a fix claim).
- Second implementation path "for safety."
