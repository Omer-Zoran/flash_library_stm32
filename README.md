# flash_lib — save bytes to internal flash (STM32)

A small library to keep data (configuration, calibration, IDs) in an STM32's
internal flash, so it survives power-off and reset.

- **3 functions:** `flash_init`, `flash_write`, `flash_read`
- **Generic:** stores any bytes — a struct, an array, anything
- **Safe area:** uses a region reserved in the linker script, so code can never
  grow into it
- **Detects empty / interrupted saves:** `flash_read` returns `FLASH_LIB_EMPTY`
  on a new chip or if power was lost in the middle of a save

## Files

| File | Purpose |
|---|---|
| `flash.h` | Public API and status codes |
| `flash.c` | Implementation (uses ST HAL flash driver) |

## Supported families

| Family | Erase unit | Write unit | Erase by | Compile-checked on |
|---|---|---|---|---|
| STM32G4 | 2 KB page | 8 B | page number + bank | G473 (dual bank), G431 (single bank) |
| STM32G0 | 2 KB page | 8 B | page number + bank | G071 (single bank), G0B1 (dual bank) |
| STM32L4 | 2 KB page | 8 B | page number + bank | L432 (single bank), L476 (dual bank) |
| STM32C0 | 2 KB page | 8 B | page number | C031 |
| STM32WB | 4 KB page | 8 B | page number | WB55 |
| STM32F0 | 1 or 2 KB page (see datasheet) | 8 B | page address | F072 |
| STM32F3 | 2 KB page | 8 B | page address | F303 |
| STM32H7 | **128 KB sector** (H74x/H75x/H72x/H73x), 8 KB sector (H7A3/H7B0) | 32 B (16 B on H7A3/H7B0) | sector number + bank | H743 (dual bank), H723 (single bank), H7A3 |

The family is detected at compile time: `flash.c` includes the HAL header it
finds in your include path. Any other family stops the build with
`#error "flash_lib: no supported STM32 HAL found in the include path"`.

"Compile-checked" means `flash.c` builds with `-Wall -Wextra` and no warnings
against that family's ST HAL. Test on your board before you rely on it.

Not supported:

- **STM32H750** (and other H7 with a single 128 KB sector): the program uses
  the only sector, so the build stops with an `#error`.
- F2/F4/F7 (sectors of different sizes) and U5/H5 (16-byte write unit) — not
  yet.

## Requirements

- The ST HAL of your chip in the include path (any STM32CubeMX project has
  it). `flash.c` finds the right `stm32xxxx_hal.h` by itself with
  `__has_include` (GCC 5+ / Clang; for other compilers, replace that block
  with a plain `#include` of your HAL header).
- The HAL FLASH driver enabled (`HAL_FLASH_MODULE_ENABLED` in
  `stm32xxxx_hal_conf.h`; CubeMX enables it by default).

## Setup

### 1. Reserve the flash area in the linker script

Edit your linker script (`STM32xxxx_FLASH.ld` in the project root). In the
`MEMORY` block, shrink `FLASH` and add `FLASH_DATA` at the end of flash.
Example for a 512 KB STM32G473:

```ld
MEMORY
{
RAM (xrw)        : ORIGIN = 0x20000000, LENGTH = 128K
FLASH (rx)       : ORIGIN = 0x8000000,  LENGTH = 496K   /* was 512K */
FLASH_DATA (r)   : ORIGIN = 0x0807C000, LENGTH = 16K    /* reserved for flash_lib */
}
```

Right after the `MEMORY` block, add the two symbols the library reads:

```ld
/* Reserved area for Drivers/flash_lib (used by flash.c) */
_flash_data_start = ORIGIN(FLASH_DATA);
_flash_data_end   = ORIGIN(FLASH_DATA) + LENGTH(FLASH_DATA);
```

Why:

- `FLASH` + `FLASH_DATA` = 496K + 16K = 512K, the whole chip, with no overlap.
- If the program ever grows past 496K, the linker fails with
  `region FLASH overflowed` instead of silently overwriting your data.
- On a dual-bank chip, the end of flash is **Bank 2**. Code runs from Bank 1,
  so erasing this area does not stall the CPU — interrupts keep running during
  a save. On a single-bank chip the CPU pauses during the erase.

For another chip or size:

- `FLASH_DATA` `ORIGIN` and `LENGTH` must be **multiples of the erase unit**
  (page, or sector on H7 — table above). `flash_init` returns
  `FLASH_LIB_ERR_LAYOUT` if the start is not aligned, because erasing would
  also wipe the code before it.
- `FLASH_DATA` end = end of flash = `0x08000000 + flash size`.
  Shrink `FLASH` by the same amount you give `FLASH_DATA`.
- Example, 64 KB STM32G071, 4 KB of data: `FLASH LENGTH = 60K`,
  `FLASH_DATA ORIGIN = 0x0800F000, LENGTH = 4K`.
- **STM32WB:** the top of flash holds the wireless (BLE) stack of the second
  core. Put `FLASH_DATA` **below** the stack start (option byte `SFSA` in
  STM32CubeProgrammer), not at the very end of flash.
- **STM32H7 (H74x/H75x/H72x/H73x):** the smallest erase is a 128 KB sector, so
  `FLASH_DATA` must be at least **128K**, even for a few bytes. Example for a
  2 MB STM32H743 (last sector of Bank 2):

  ```ld
  FLASH (rx)       : ORIGIN = 0x08000000, LENGTH = 1920K   /* was 2048K */
  FLASH_DATA (r)   : ORIGIN = 0x081E0000, LENGTH = 128K
  ```

  On H7A3/H7B0 sectors are 8 KB, so `FLASH_DATA` can be 8K.
- **STM32H7 dual-core (H745/H747/H755/H757):** the Cortex-M4 program usually
  lives in Bank 2. Make sure `FLASH_DATA` does not overlap it.

> **CubeMX warning:** regenerating code from the `.ioc` may overwrite the
> linker script. After every regenerate, check that the changes above are still
> there. If the symbols are gone, the build fails with
> `undefined reference to _flash_data_start` — re-apply this step.

### 2. Add the library to the build

Copy `flash.c` and `flash.h` into your project (here: `Drivers/flash_lib/`).
In the root `CMakeLists.txt`:

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/flash_lib/flash.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/flash_lib
)
```

### 3. Include it

```c
#include "flash.h"
```

## API

```c
flash_lib_status_t flash_init(uint32_t size);
flash_lib_status_t flash_write(const void *data, size_t data_length);
flash_lib_status_t flash_read(void *buffer, size_t bytes_n);
```

| Function | What it does |
|---|---|
| `flash_init(size)` | Call once at boot. `size` = the maximum number of bytes you will store. Max = `FLASH_DATA` length − one write unit (8 B; 32 or 16 B on H7). |
| `flash_write(data, len)` | Erases the area and saves `len` bytes. `len` must be ≤ `size`. |
| `flash_read(buf, len)` | Copies `len` bytes from flash into `buf`. Returns `FLASH_LIB_EMPTY` if nothing was saved yet. |

### Status codes

| Code | Meaning |
|---|---|
| `FLASH_LIB_OK` | Done |
| `FLASH_LIB_EMPTY` | `read`: nothing saved yet (new chip), or the last save was cut short by power loss |
| `FLASH_LIB_ERR_PARAM` | `NULL` pointer, length 0, or length > `size` from `init`; in `init`: `size` is 0 or does not fit in `FLASH_DATA` |
| `FLASH_LIB_ERR_NOT_INIT` | `flash_init()` was not called, or it failed |
| `FLASH_LIB_ERR_LAYOUT` | `init`: `FLASH_DATA` does not start on a page (or H7 sector) boundary — fix the linker script |
| `FLASH_LIB_ERR_HAL` | Flash erase or program failed |

## Example

Save a configuration struct and read it back:

```c
#include "flash.h"

typedef struct
{
    char     name[16];
    char     last_name[16];
    uint32_t sn;
} configs_t;

configs_t myConfig =
{
    .name      = "John",
    .last_name = "Doe",
    .sn        = 1234567
};

configs_t myConfigRead = {0};

void setup(void)
{
    volatile flash_lib_status_t status;   /* volatile: visible in debugger */

    status = flash_init(sizeof(myConfig));
    status = flash_write(&myConfig, sizeof(myConfig));
    status = flash_read(&myConfigRead, sizeof(myConfigRead));
    /* expect: status == FLASH_LIB_OK, myConfigRead == myConfig */
}
```

Typical real use — read first, write defaults only if nothing is saved:

```c
static const configs_t default_config = { "John", "Doe", 1234567 };
static configs_t config;

void setup(void)
{
    flash_init(sizeof(config));

    if (flash_read(&config, sizeof(config)) != FLASH_LIB_OK)
    {
        config = default_config;                  /* first boot */
        flash_write(&config, sizeof(config));
    }
}

/* later, when a setting changes: */
config.sn = 7654321;
flash_write(&config, sizeof(config));
```

## How it works

```
FLASH_DATA (_flash_data_start)
┌──────────────┬───────────────────────────────┐
│ MAGIC        │ your data (size bytes)        │
└──────────────┴───────────────────────────────┘
  one write unit (8 B; 32 or 16 B on H7), written LAST
```

1. `flash_write` erases the needed pages (or sectors on H7).
2. Writes your data, one write unit at a time.
3. Writes `MAGIC` last. Only then is the save "complete".

If power is lost during steps 1–2, `MAGIC` is missing, and the next
`flash_read` returns `FLASH_LIB_EMPTY` instead of half-written data.

## Notes and limits

- **Do not save often.** Each page (or sector) survives about 10,000 erases. Save when a
  setting changes, not in a loop and not on every boot.
- **Blocking.** `flash_write` takes tens of milliseconds (one page erase is
  about 20–40 ms, depending on the chip). Do not call it from an interrupt — it
  waits on `HAL_GetTick()` and would hang.
- **STM32H7: erasing a 128 KB sector takes about 1–2 seconds.** If you use a
  watchdog (IWDG), make its timeout longer than that, or the board resets in
  the middle of a save.
- **STM32H7 supply voltage.** The erase uses `FLASH_VOLTAGE_RANGE_3`
  (2.7–3.6 V supply). For a board below 2.7 V, change it in
  `set_erase_location()` in `flash.c`. (H7A3/H7B0 do not use this setting.)
- **STM32H7 data cache.** After a save, `flash_write` invalidates the D-cache
  for the data area, so `flash_read` returns the new data, not an old cached
  copy.
- **Changing the struct layout** (adding/removing fields) makes old saved data
  wrong. Read it as empty or handle versions yourself.
- **Dual-bank chips: keep the factory bank mode.** On dual-bank chips
  (e.g. G47x/G48x, G0B1, L47x) the page and bank math assume dual-bank mode,
  the factory default. If the bank-mode option bit is changed (`DBANK` on G4,
  `DUAL_BANK` on G0, `DUALBANK` on L4), the page or bank layout changes and the
  library erases the wrong area. Check in STM32CubeProgrammer → Option Bytes.
  (H7 dual-bank chips have no such option.)
- **Survives firmware updates.** Flashing new firmware with ST-LINK normally
  erases only the pages (sectors) it writes, so `FLASH_DATA` is kept. A **full chip
  erase** in STM32CubeProgrammer wipes it.
- **Known limit — ECC lock-up** (G0, G4, L4, C0, WB, H7; F0/F3 have no ECC).
  Flash stores an error-check code (ECC) with every write unit. If power is lost in the exact moment `MAGIC` itself is
  being written, that unit is half-written and reading it triggers an NMI (on
  H7: a BusFault); the default CubeMX handler loops forever. (A cut during the
  *data* is safe: `MAGIC` is missing, so `flash_read` never reads the broken
  data.) Very rare, but not yet handled by this library.
