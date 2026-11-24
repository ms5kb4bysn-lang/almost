# The audio coprocessor, texture layout and DirectStorage imports: how the retail executable calls them (RT-5)

`gta6rt --dry-run` leaves 30 of `game_scarlett_master.exe`'s imports unimplemented
(the section on the retail executable in `docs/runtime_imports.md`, #6715). Eight are `xmem.dll` and `d3d12_xs.dll` and are
in [`xmem_exe_calls.md`](xmem_exe_calls.md). The other 22 are the audio coprocessor (`AcpHal.dll`, `xapu.dll`,
`chad.dll`: 19), the texture layout library (`xg_xs.dll`: 2) and DirectStorage (`XCRDAPI.dll`: 1). No public
documentation of the audio functions exists, so this page records, from the file alone, how the executable calls them:
how many call sites each has, the constants those sites pass, and how each site uses the result. Nothing was run, and no
code or data of the executable is quoted. Measured at `main` `1cc6e3c1` with `tools/import_call_sites.py`; the argument
registers before each call were read by hand.

```powershell
python -I tools/import_call_sites.py game_scarlett_master.exe --dll AcpHal.dll --dll xapu.dll --dll chad.dll `
    --dll xg_xs.dll --dll XCRDAPI.dll
```

The counts are direct calls and tail jumps to each import's thunk (22 imports, 36 sites; the tool's own caveats apply).
Every one of the 22 has at least one site, so none is dead code the linker kept.

## `AcpHal.dll` (9 imports, 16 sites)

| Import | Sites | What the sites show |
|---|---:|---|
| `AcpHalCreate` | 1 | first argument is the address of a global that is read right after the call (an out parameter); second is 0. The result is tested for a negative value and a negative one jumps to a cleanup path |
| `AcpHalAllocateShapeContexts` | 1 | first argument is the address of another global, read afterwards; the result is tested for a negative value like the one above |
| `AcpHalReleaseShapeContexts` | 1 | called with a handle read from the first global; result not used |
| `ApuCreateHeap` | 1 | second argument `0x226e00`; the result is tested for a negative value |
| `ApuAlloc` | 1 | second argument `0`, third a computed value, fourth `0x800`; the result is not used at the site |
| `ApuFree` | 1 tail jump | no call, only a tail jump from a wrapper |
| `ApuIsVirtualAddressValid` | 4 | second argument `0x40`, `0x80`, `0x800`, `0x800`; the result byte is tested |
| `ApuMapVirtualAddress` | 4 | the same pointer and the same four constants as `ApuIsVirtualAddressValid`, one after the other; the 32-bit result is stored in a per-entry record |
| `ApuMapApuAddress` | 2 | the argument is a 32-bit value read from those records; the result is tested for null, so it is a pointer |

The four `ApuIsVirtualAddressValid`/`ApuMapVirtualAddress` pairs and the two `ApuMapApuAddress` calls are consistent with
a title that checks that a CPU buffer can be seen by the coprocessor, takes its coprocessor-side 32-bit address, and later
turns such an address back into a pointer. That is an inference from the shapes; the meaning of the sizes is unmeasured.

## `xapu.dll` (5 imports, 8 sites) and `chad.dll` (5 imports, 5 sites)

| Import | Sites | What the sites show |
|---|---:|---|
| `XApuConnect` | 1 | no argument loaded in the lines before the call was read |
| `XApuDisconnect` | 1 | first argument a handle read from a global, second `0x20`; the global is set to 0 right after |
| `XApuEnqueueCommand` | 4 | the same global handle, a pointer to a stack-built command, and 0 |
| `XApuDequeueResult` | 1 | the same global handle and a second argument; the result is tested for a negative value |
| `XApuGetResultCount` | 1 | no argument read |
| `XDspConnectWithMaximumStreamLimit` | 1 | fourth argument `0x80`; the result is compared with 0 and turned into a boolean that is true for 0 |
| `XDspActivate` | 1 | the result is tested |
| `XDspSubmitCommand` | 1 | two arguments, both taken from registers; not read further |
| `XDspDeactivate`, `XDspDisconnect` | 1 tail jump each | no call |

`XApuEnqueueCommand` and `XApuDequeueResult` share one handle global, which `XApuDisconnect` clears: a command queue
with a result queue, connected once and torn down once. How `chad.dll`'s handle relates to it is not read.

## `xg_xs.dll` and `XCRDAPI.dll`

| Import | Sites | What the sites show |
|---|---:|---|
| `XGCreateTextureComputer` | 4 | argument shapes differ between sites; not read further |
| `XGSuggestSwizzleMode` | 2 | not read further |
| `DStorageGetFactory` | 1 | the first argument is the address of a constant (an interface id) and the second the address of a global; the global is loaded afterwards and tested for null |

`DStorageGetFactory` has the shape of the public DirectStorage function `DStorageGetFactory(REFIID, void**)`. #4348 is an
open pull request for it.

## What the executable already does about a failure

Four of the calls above have a branch for a negative result: `AcpHalCreate`, `AcpHalAllocateShapeContexts`,
`ApuCreateHeap` and `XApuDequeueResult`. An unimplemented import under `LEONIDA_UNIMPLEMENTED=log` returns 0, so a caller
that tests for a negative result sees success with its outputs unset; under `fail` only imports `classify_import`
knows to return an `HRESULT` get `E_NOTIMPL`, and none of these four is on that list (it names only functions the public
documentation describes). So today neither mode makes the executable take its own failure path for the audio hardware.
Whether the title survives that path, or needs the coprocessor, is **unmeasured**: it takes a run to find out, and the
first run reported in #6603 stops earlier, in `D3D12ConfigureVirtualMemoryX`.

## What this does not show

Nothing here says that the boot reaches these calls, in what order, or what the commands and results carry. A site is code
that can call; the first trace (`--trace`, `unimplemented_call` events) is the signal for which of them run.

## Argument orders decoded for the boot shim (feat/acphal-shim, on c0c5eb2a0)

Reading the registers set immediately before each call (not only the counts above) settles six shapes that the table
alone leaves open. `runtime/shims/acphal.cpp` implements them; its file header has the per-site evidence.

- `AcpHalAllocateShapeContexts` takes **one pointer to a descriptor**, not an out pointer. At `+0x43aec4b` rcx is
  the descriptor at RVA `0x9ae98d0`; the title writes the qword `0x15e` at descriptor `+0x10`, reads it back after the
  call, and reads the array pointer the call must write from descriptor `+0x38`. It then memsets `count * 0x40` bytes
  at `+0x43aec94` (the return address `+0x43aec99` is the crash this shim fixes). The field meanings are not measured.
- `ApuCreateHeap` has **no out argument**. rcx is `0x1c7` or `0xff` from `+0x25a8750`, shifted left 20
  (`0x1c700000` / `0xff00000`), and rdx is `0x226e00`. Writing a heap object through rcx would write to a 476 MiB
  address; the heap is internal state.
- `ApuAlloc`'s first argument is an **out pointer**, not a heap handle. The wrapper at `+0x43a6110` is called with rcx
  pointing at a zeroed stack qword, and the caller reads that qword into r15 after the call at `+0x43aed18` and memsets
  through it. A second caller at `+0x1e74620` passes the address of a global. The second argument is 0, the third the
  size, the fourth `0x800`.
- `ApuMapVirtualAddress` takes **the pointer alone**: the call at `+0x7879c` (and the other three) reloads only rcx;
  the `0x40`/`0x80`/`0x800` in edx is the leftover size of the preceding `ApuIsVirtualAddressValid`.
- `XApuConnect` takes **three pointers**: input at RVA `0x9ae9890` (24 bytes: process=1, streams=0x100,
  commands_per_stream=2, memory=0x104, frames_per_output=0, options=1), output at `0x9ae98a8` (`{void* memory;
  HANDLE result_event}`) and the out handle at `0x9ae8960`, which every other xapu call reads. The memory field is
  4 KiB pages there (`0x104000` bytes). The process command built at `+0x4f20b7e` is 0x40 bytes after a **24-byte**
  packed id (four dwords and a qword, not 20), and the result read at `+0x4eaa8a3` is 0x50 bytes with `kind` at +0,
  `stream` at +4 and `output_length` at +0x34. An empty dequeue returns `0x8AC81001` (PENDING_RESULTS), a full
  per-stream queue `0x8AC81000`; the shim queues one result per command and returns silence for process commands.
