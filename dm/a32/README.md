# AArch32 ahead-of-time recompiler (work in progress)

Goal: run 32-bit Switch titles (Monster Hunter Generations Ultimate: `main.npdm` has the 64-bit flag clear, all code is ARM mode, no Thumb) the
way nxnative runs 64-bit ones, without an emulated CPU. Apple silicon cannot execute AArch32, so the game's ARM code is translated to AArch64
ahead of time and linked with a host that plays Horizon's kernel and services.

## State (October 2026)

What runs, on MHGU 1.0.0, from the real NSP:

- The four modules (rtld, main, subsdk0, sdk: 6.6 million translated instruction slots) load into a 4 GB guest address space, relocate, and link
  (ELF32 REL relocations, 171,017 relative ones in `main`; imports resolved against the other modules; a few SDK functions taken over by name).
- The SDK starts as rtld would start it: `_init_libc0`, `nnosInitialize`, `nninitInitializeSdkModule`, `nninitStartup`, `_init_libc2`, each module's
  `DT_INIT` (all of the game's, subsdk0's and sdk's global constructors), then `nnMain`.
- The game's own code runs in `nnMain`: it creates worker threads, initialises the time and applet services, mounts its RomFS (the SDK's RomFS
  parser reads the real RomFS tables through an `IStorage` served by the host), and calls `nv::InitializeGraphics`, which is where it stops (below).
- Services the host answers over HIPC/CMIF (`rt/a32ipc.c`, `rt/a32svcs.c`, `rt/a32fs.c`): `sm:`, `lm`, `appletOE` (with the message queue
  `ICommonStateGetter` needs), `fsp-srv` (RomFS storage), and a generic service that succeeds with zeroed output for any other (each command is logged once).
- Kernel (`rt/a32svc.c`): the 32-bit svc ABI read off the SDK's own svc stubs; threads (pthreads running translated code), address arbitration,
  process-wide keys, events, `WaitSynchronization`, `GetInfo`, memory queries and maps, the system counter.

Where it stops: `nv::InitializeGraphics` (in the sdk module) opens the `nvdrv` service and transfer memory. This SDK ships the NVN driver itself, so
graphics is the GPU's ioctl interface (nvmap / nvhost-gpu / as-gpu, Maxwell pushbuffers and shaders). That is the next large piece and is not started.

## Design

- r0-r14 live in x0-x14 (the w views; the upper halves stay zero). pc is never a register: reads are constants, writes are a dispatch.
- Guest memory is `[x28, w, uxtw]` (a 4 GB window), so 32-bit address wrap-around is free. The whole window is mapped read-write up front (the host's
  16 KB pages are coarser than the guest's 4 KB ones) and lazily committed.
- AArch32 NZCV is AArch64 NZCV (carry = not borrow in both): add/sub/adc/sbc/cmp/cmn with S are one instruction; the logical instructions with S rebuild N, Z, C
  (shifter carry-out) and keep V. Conditional execution is `b.<inverse condition>` over the body.
- VFP/NEON: q0-q15 = v0-v15 with the same overlay AArch32 has (d(2n), d(2n+1) = halves of v(n); s(4n+k) = lanes). A result is computed in v16 and inserted into
  its lane (scalar A64 operations would zero the neighbours). FPSCR flags live in x24; rounding, flush-to-zero and default NaN are the FPCR bits (same positions).
  NEON arithmetic runs with the "standard FPSCR value" (round to nearest, flush-to-zero, default NaN).
- Every aligned word of a module's text is a 16-byte *slot* of four A64 instructions, so a guest address maps to its code by arithmetic:
  `slots + ((addr - text_start) >> 2) * 16`. A word needing more than four instructions leaves `b` to out-of-line code that ends with a branch to the next slot;
  a word the lifter does not cover is a trap naming its address. Indirect jumps, returns and `pop {pc}` hand a guest address in **w21** to `_a32_dispatch`.
- Stub addresses (`HOOK_BASE` 0xf0000000 and up) are host calls: imports the host implements, and the return into the host (`a32_enter` / `a32_call`).

Two traps found the hard way (both are checked now):

1. The linker inserts branch islands wherever a call cannot reach its target. The libc stubs sat more than 128 MB from `__text`, and the islands landed *inside* the
   slot section, displacing every slot after them. The slots live in their own segment (`__A32`), the runtime entry points are reached through per-chunk stubs
   (`adrp/add/br`), and the host verifies at startup that every chunk is exactly where the dispatcher computes it.
2. Registers `x16`/`x17` are used by those islands, so the guest address passed to a runtime stub travels in `w21`.

## Files

- `lift.py`, `vfp.py`, `neon.py`, `lift_base.py`: one ARM instruction word -> AArch64 assembly lines. Anything not covered raises `Unsupported`, never a guess.
- `translate.py`: whole-module translation into assembly chunks (`--only`, `--trace module:lo:hi`, `--outline-all`, `--outline-range` for debugging), `layout.tsv`, `modtab.c`.
- `build.sh` (translate + assemble + link), `relink.sh` (runtime only), `iter.sh` (one development round).
- `rt/a32rt.S` dispatch, hooks, svc and trace gates, entering guest code; `rt/a32host.c` loader and linker; `rt/a32svc.c` kernel; `rt/a32ipc.c` HIPC/CMIF;
  `rt/a32svcs.c` service tables; `rt/a32fs.c` fsp-srv and the RomFS storage; `rt/a32force.c` SDK functions taken over by name (nn::time, the profanity filter, ...).
- `mkromfs.py`: builds the RomFS binary tables (header, directory/file hash and meta tables) of an extracted RomFS directory; file data is read from the extracted files.
- `difftest.py` + `harness.c`: differential test against Unicorn (QEMU's AArch32 CPU): random encodings per instruction class and random register, flag, vector and memory
  states run in Unicorn and, lifted and assembled, natively on the Mac; r0-r14, NZCV, FPSCR, d0-d31, the next pc and a CRC of the test memory must match.
  `python3 dm/a32/difftest.py [tests-per-class] [seed] [class ...]`; `A32_CORPUS=<file of u32 words> ... corpus` tests real instruction words taken from a game.
- `census.py`, `coverage.py`, `plt.py`: feasibility census, lifter coverage of a whole text segment, which symbol a PLT stub binds to.

## Verification

About 9,000 random cases across 38 instruction classes pass against Unicorn (one known mismatch: a floating-point exception flag on an out-of-range `vcvtr`), and 6,000 real
instruction words sampled from MHGU's code pass. VSEL cannot be tested (Unicorn's CPU model lacks it). The lifter covers 98-99% of the words in the text segments; the rest is data in the
text or forms not used so far (exception-return forms, `ldrt`/`strt`, unpredictable encodings).

## Run it

```
python3 -m venv .venv && .venv/bin/pip install lz4 capstone fonttools unicorn
dm/a32/build.sh <ExeFS dir> <out dir>                         # translate, assemble, link  (~40 s)
python3 dm/a32/mkromfs.py <extracted RomFS dir> <out dir>/romfs
A32_ROMFS_DIR=<extracted RomFS dir> <out dir>/a32host <out dir>   # A32_SVC=1|2 traces svcs, A32_IPC=1 the IPC messages, A32_BIND=<substr> import binding
```

## Next

1. Graphics: `nvdrv` (nvmap, nvhost-gpu, nvhost-as-gpu, nvhost-ctrl) or the NVN API above it; the Maxwell command stream and the game's shaders need translating to Metal.
2. Audio (`audout`/`audren`), input (`hid`), save data (`fsp-srv` save-data filesystems), the remaining services, and movies.
3. Thread scheduling closer to Horizon's (priorities, core affinity), and a kernel `svc` set beyond what startup uses.
4. Packaging (the 130 MB translated image, the RomFS and the Mac app wrapper), and an iOS build, which cannot use a JIT but can embed the same ahead-of-time code.
