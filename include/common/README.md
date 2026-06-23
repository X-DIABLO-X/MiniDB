# `common/` — shared types and error codes

No business logic lives here. Just the type aliases and the `Status` enum
that *every* module's public functions return. If you need a new ID type or
a new status code, add it here **and** to `docs/interfaces.md`.

## Files

| File | Contents |
|---|---|
| `include/common/types.h`   | `PageId`, `TransactionId`, `RecordId`, `FrameId`, `LSN`, `Key`. |
| `include/common/status.h`  | `enum class Status` and a tiny `toString(Status)` helper. |
| `include/common/record_id.h` | `makeRid`, `ridPage`, `ridSlot` helpers. |

## Public API (the **whole** point of this module)

```cpp
// types.h
namespace minidb {
    using PageId        = uint32_t;
    using TransactionId = uint64_t;
    using RecordId      = uint64_t;
    using FrameId       = int32_t;
    using LSN           = uint64_t;
    using Key           = std::string;

    constexpr PageId       INVALID_PAGE_ID  = 0;
    constexpr FrameId      INVALID_FRAME_ID = -1;
    constexpr TransactionId INVALID_TXN_ID  = 0;
    constexpr RecordId     INVALID_RID      = 0;
    constexpr LSN          INVALID_LSN      = 0;
}

// status.h
namespace minidb {
    enum class Status : int {
        OK, UNIMPLEMENTED, NOT_FOUND, DUPLICATE_KEY, FULL,
        IO_ERROR, INVALID_ARGUMENT, TYPE_MISMATCH,
        DEADLOCK, ABORTED, TXN_CONFLICT
    };
    const char* toString(Status s);
}

// record_id.h
namespace minidb {
    RecordId  makeRid(PageId pageId, uint16_t slotIdx);
    PageId    ridPage(RecordId rid);
    uint16_t  ridSlot (RecordId rid);
}
```

## How other modules use this folder

- Every module's header includes `<common/types.h>` and `<common/status.h>`
  as needed.
- Every public function returns `Status`. Callers must check the return
  value (a quick `if (s != Status::OK) return s;` is the canonical pattern).
- `Key` is the generic B+ tree key type. The index module currently
  treats it as a length-prefixed byte string; ints and floats are
  serialised into a `Key` by the index wrapper.

## Conventions

- `Status` codes are stable. **Do not renumber** — they're persisted in the
  WAL.
- `RecordId = (pageId << 32) | slotIdx`. The 32 high bits are `PageId`, the
  16 low bits are `slotIdx` (the next 16 bits are reserved for MVCC
  version). Never construct a `RecordId` by hand; always use `makeRid`.
- No module other than `storage` should know what a `PageId` indexes into.
  The rest of the system only knows "this is a page id".

## What is *not* in this folder

- No logging macros (use `std::cerr` in the stubs; we'll add a real logger
  at M3).
- No memory allocator hooks.
- No thread-pool primitives (the executor will use `std::thread` directly
  for the benchmark comparison).
