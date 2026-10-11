# GPU buffer retirement

GpuMemory keeps a guest range's buffer objects alive after their last use so
later draws can reuse them. Two passes retire them.

## Frame pass

`GpuMemory::FrameDone` runs every 30 presented frames once 120 frames have
been presented. It retires textures, storage images and buffers that no use has
touched for 120 presented frames and whose recorded submissions have completed.
Storage images written by a dispatch, writable buffers with pending GPU writes
and buffers bound to depth metadata stay alive.

## Buffer pressure pass

Presented frames are not a reliable clock. A phase can record thousands of
submissions between two presents, for example a loading screen that binds a new
read-only storage view per draw: each view slides a few records along a ring
and declares the rest of the ring, so every draw creates a large buffer that
crosses all earlier ones. The frame pass never runs during that phase, so the
buffers accumulate until the host runs out of memory.

Creation therefore counts the bytes of storage, vertex and index buffers created
since the last retirement pass. Once that count reaches
`kGpuMemoryBufferPressureRetireBytes` (256 MiB), the creating call retires every
buffer that:

- owns no device content (read-only, or its GPU writes were written back);
- is not bound to depth metadata;
- has recorded at least one submission, and every recorded submission has
  completed.

A buffer used by the command buffer being recorded carries that open
submission, so the pass never retires a buffer a pending draw still reads. The
new buffer carries the same open submission. Destruction goes through the
deferred deletion queue like any other retirement. A retired buffer holds only
guest bytes or a copy of a linked peer, so a later use re-creates it from guest
memory. Either pass resets the byte count.

## Verification

```bash
cmake --build <owned-build> --parallel 2 --target kyty_unit_test
<owned-build>/kyty_unit_test
```

The run must end with a `[  PASSED  ]` line and no `[  FAILED  ]` line; an
unknown flag prints the help text and exits with status 0 without running tests.

For a bounded runtime check, run a title through a phase that creates many
buffers between presents, sample the run's cgroup memory and confirm that live
memory stays flat instead of growing with every draw.
