# MiniDB Documentation

This folder is the **contract** every team member codes against. If something
isn't defined here, it isn't part of the project.

## Documents

| File | Purpose | When to read it |
|---|---|---|
| [`architecture.md`](architecture.md) | High-level module diagram, who owns what, design goals. | Day 1, then as orientation. |
| [`interfaces.md`](interfaces.md) | **The** C++ public-interface definitions. This is the contract. | Before touching any code, and any time you add a public function. |
| [`dataflow.md`](dataflow.md) | Step-by-step request paths: `SELECT`, `INSERT`, transactions, recovery. | When implementing a new operator or debugging end-to-end. |
| [`catalog.md`](catalog.md) | How table metadata is stored and looked up. | Before implementing storage, index, or executor — all three need it. |
| [`benchmarks.md`](benchmarks.md) | What to measure, in which harness, and how to report. | M4 onwards. |

## How to use these docs

1. **First day:** read `architecture.md` top to bottom, then skim
   `interfaces.md` so you know the names of things.
2. **Before implementing a module:** re-read the section of `interfaces.md`
   that lists the public API of the modules you depend on. Don't guess.
3. **When you change a public function signature:** update `interfaces.md`
   and announce it in the team chat. Other modules will be using the old
   name.
4. **When you change a behavior that crosses modules** (e.g. "the buffer
   pool now needs to call back into the WAL on flush"), update both
   `interfaces.md` and the relevant `dataflow.md` step.

## What is *not* in this folder

- The project README (lives at the repo root).
- Per-module READMEs that list internal structure (live in each
  `include/<module>/README.md`).
- The C++ source code itself.
