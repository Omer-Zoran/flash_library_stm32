#include "flash.h"

/* Include the HAL of the target family. A project's include path holds only
 * its own family's HAL, so exactly one of these is found. */
#if !defined(__has_include)
  #error "flash_lib: compiler lacks __has_include; include your stm32xxxx_hal.h here by hand"
#elif __has_include("stm32g4xx_hal.h")
  #include "stm32g4xx_hal.h"
#elif __has_include("stm32g0xx_hal.h")
  #include "stm32g0xx_hal.h"
#elif __has_include("stm32l4xx_hal.h")
  #include "stm32l4xx_hal.h"
#elif __has_include("stm32c0xx_hal.h")
  #include "stm32c0xx_hal.h"
#elif __has_include("stm32wbxx_hal.h")
  #include "stm32wbxx_hal.h"
#elif __has_include("stm32f0xx_hal.h")
  #include "stm32f0xx_hal.h"
#elif __has_include("stm32f3xx_hal.h")
  #include "stm32f3xx_hal.h"
#elif __has_include("stm32h7xx_hal.h")
  #include "stm32h7xx_hal.h"
#else
  #error "flash_lib: no supported STM32 HAL found in the include path (see README)"
#endif

#include <stddef.h>
#include <string.h>

/* ---- Per-family settings ------------------------------------------------- */
#if defined(STM32G0) || defined(STM32G4) || defined(STM32L4)
  #define FLASH_LIB_ERASE_BY_PAGE_INDEX       /* erase: page number (+ bank) */
  #define FLASH_LIB_HAS_BANKS_FIELD           /* FLASH_EraseInitTypeDef.Banks exists */
#elif defined(STM32C0) || defined(STM32WB)
  #define FLASH_LIB_ERASE_BY_PAGE_INDEX       /* erase: page number, no Banks field */
#elif defined(STM32F0) || defined(STM32F3)
  #define FLASH_LIB_ERASE_BY_PAGE_ADDRESS     /* erase: page start address */
#elif defined(STM32H7)
  #define FLASH_LIB_ERASE_BY_SECTOR           /* erase: sector number + bank */
  #define FLASH_LIB_HAS_BANKS_FIELD
  #if (FLASH_SECTOR_TOTAL == 1U)
    #error "flash_lib: this H7 has a single flash sector (e.g. H750), no room for data"
  #endif
#else
  #error "flash_lib: this STM32 family is not supported (see README)"
#endif

/* Smallest erase (page or sector) and smallest write (bytes per program call) */
#if defined(FLASH_LIB_ERASE_BY_SECTOR)
  #define FLASH_LIB_ERASE_SIZE  FLASH_SECTOR_SIZE                        /* H7: 128K or 8K */
  #define FLASH_LIB_WRITE_UNIT  (FLASH_NB_32BITWORD_IN_FLASHWORD * 4U)   /* H7: 32 or 16 B */
#else
  #define FLASH_LIB_ERASE_SIZE  FLASH_PAGE_SIZE
  #define FLASH_LIB_WRITE_UNIT  8U                                       /* double-word */
#endif

/* Clear leftover error flags; a stale flag makes the next erase/program fail */
#if defined(STM32H7) && defined(DUAL_BANK)
  #define FLASH_LIB_CLEAR_ERRORS()  do { __HAL_FLASH_CLEAR_FLAG_BANK1(FLASH_FLAG_ALL_ERRORS_BANK1); \
                                         __HAL_FLASH_CLEAR_FLAG_BANK2(FLASH_FLAG_ALL_ERRORS_BANK2); } while (0)
#elif defined(STM32H7)
  #define FLASH_LIB_CLEAR_ERRORS()  __HAL_FLASH_CLEAR_FLAG_BANK1(FLASH_FLAG_ALL_ERRORS_BANK1)
#elif defined(FLASH_FLAG_ALL_ERRORS)          /* G4, L4, C0, WB */
  #define FLASH_LIB_CLEAR_ERRORS()  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS)
#elif defined(FLASH_SR_ERRORS)                /* G0: HAL macro clears one flag at a time */
  #define FLASH_LIB_CLEAR_ERRORS()  WRITE_REG(FLASH->SR, FLASH_SR_ERRORS)
#else                                         /* F0, F3 */
  #define FLASH_LIB_CLEAR_ERRORS()  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR)
#endif

/* Defined in the linker script (.ld) */
extern uint8_t _flash_data_start[];
extern uint8_t _flash_data_end[];

/* Layout in FLASH_DATA:  [ MAGIC (one write unit) ][ user data ... ]
 * MAGIC is written LAST, so it marks a completed save. Missing MAGIC means
 * a new chip or a save cut short by power loss -> read returns EMPTY. */
#define FLASH_LIB_MAGIC     0x464C4942UL      /* "FLIB" */
#define FLASH_LIB_HDR_SIZE  FLASH_LIB_WRITE_UNIT

typedef struct
{
  FLASH_EraseInitTypeDef erase_init;
  uint32_t flash_data_start;
  uint32_t flash_data_end;
  uint32_t size;
} flash_lib_context_t;

static flash_lib_context_t context = { 0 };


static void set_erase_location(uint32_t addr);
static HAL_StatusTypeDef program_unit(uint32_t addr, const uint32_t *unit);

flash_lib_status_t flash_init(uint32_t size)
{
  context.size = 0U;          /* invalid until success */
  context.flash_data_start = (uint32_t)_flash_data_start;
  context.flash_data_end   = (uint32_t)_flash_data_end;

  /* Erase works on whole pages/sectors: a misaligned area would erase code before it */
  if ((context.flash_data_start % FLASH_LIB_ERASE_SIZE) != 0U)
  {
    return FLASH_LIB_ERR_LAYOUT;
  }

  if(size == 0 || (size + FLASH_LIB_HDR_SIZE) > (context.flash_data_end - context.flash_data_start))
  {
    return FLASH_LIB_ERR_PARAM;
  }

  uint32_t nb_units = (size + FLASH_LIB_HDR_SIZE + FLASH_LIB_ERASE_SIZE - 1U) / FLASH_LIB_ERASE_SIZE;

  context.size = size;
  set_erase_location(context.flash_data_start);
#if defined(FLASH_LIB_ERASE_BY_SECTOR)
  context.erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
  context.erase_init.NbSectors = nb_units;
#else
  context.erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
  context.erase_init.NbPages   = nb_units;
#endif

  return FLASH_LIB_OK;
}

flash_lib_status_t flash_write(const void *data, size_t data_len)
{

  const uint8_t *src = (const uint8_t *)data;
  uint32_t page_error = 0U;
  HAL_StatusTypeDef st;

  if (context.size == 0U)                  { return FLASH_LIB_ERR_NOT_INIT; }
  if ((data == NULL) || (data_len == 0U) ||
      (data_len > context.size))        { return FLASH_LIB_ERR_PARAM; }

  HAL_FLASH_Unlock(); // enable writing to the flash
  FLASH_LIB_CLEAR_ERRORS();

  st = HAL_FLASHEx_Erase(&context.erase_init, &page_error);

  for(uint32_t i = 0; (st == HAL_OK) && (i < data_len); i += FLASH_LIB_WRITE_UNIT)
  {
    uint32_t unit[FLASH_LIB_WRITE_UNIT / 4U];      /* uint32_t: H7 HAL reads it as words */
    uint32_t bytes_to_write = (data_len - i) >= FLASH_LIB_WRITE_UNIT ? FLASH_LIB_WRITE_UNIT : (data_len - i);
    memset(unit, 0xFF, sizeof(unit));               /* pad the last unit like erased flash */
    memcpy(unit, src + i, bytes_to_write);
    st = program_unit(context.flash_data_start + FLASH_LIB_HDR_SIZE + i, unit);
  }

  /*write MAGIC only after all data is in flash */
  if (st == HAL_OK)
  {
    uint32_t magic[FLASH_LIB_WRITE_UNIT / 4U] = { FLASH_LIB_MAGIC };
    st = program_unit(context.flash_data_start, magic);
  }

  HAL_FLASH_Lock(); //lock the option writing to the flash

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
  /* H7: the D-cache may still hold the old flash content -> drop it */
  SCB_InvalidateDCache_by_Addr((void *)context.flash_data_start, (int32_t)(FLASH_LIB_HDR_SIZE + context.size));
#endif

  return st == HAL_OK ? FLASH_LIB_OK : FLASH_LIB_ERR_HAL;
}

flash_lib_status_t flash_read(void *buffer, size_t buffer_len)
{

  if (context.size == 0U)
  {
    return FLASH_LIB_ERR_NOT_INIT;
  }

  if ((buffer == NULL) || (buffer_len == 0U) || (buffer_len > context.size))
  {
    return FLASH_LIB_ERR_PARAM;
  }

  if (*(const uint32_t *)context.flash_data_start != FLASH_LIB_MAGIC)
  {
    return FLASH_LIB_EMPTY;            /* new chip, or save was cut short */
  }

  memcpy(buffer, (const void *)(context.flash_data_start + FLASH_LIB_HDR_SIZE), buffer_len);

  return FLASH_LIB_OK;
}

/* Program one write unit (8 bytes, or one H7 flash word) at addr */
static HAL_StatusTypeDef program_unit(uint32_t addr, const uint32_t *unit)
{
#if defined(STM32H7)
  return HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr, (uint32_t)unit);
#else
  uint64_t dw;
  memcpy(&dw, unit, sizeof(dw));
  return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dw);
#endif
}

/* Fill the bank + first page/sector of the erase from the address, so they always agree */
static void set_erase_location(uint32_t addr)
{
#if defined(FLASH_LIB_ERASE_BY_PAGE_ADDRESS)
  context.erase_init.PageAddress = addr;
#else
  uint32_t offset = addr - FLASH_BASE;          /* offset inside its bank */
  #if defined(FLASH_LIB_HAS_BANKS_FIELD)
  context.erase_init.Banks = FLASH_BANK_1;
  #endif
  #if defined(FLASH_BANK_2)                     /* dual-bank chip */
  if (addr >= (FLASH_BASE + FLASH_BANK_SIZE))
  {
    context.erase_init.Banks = FLASH_BANK_2;
    offset -= FLASH_BANK_SIZE;
  }
  #endif

  #if defined(FLASH_LIB_ERASE_BY_SECTOR)
  context.erase_init.Sector = offset / FLASH_LIB_ERASE_SIZE;
    #if defined(FLASH_CR_PSIZE)                 /* H74x/H75x/H72x; H7A3/B0 have no PSIZE */
  context.erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;   /* 2.7-3.6 V supply */
    #endif
  #else
  context.erase_init.Page = offset / FLASH_LIB_ERASE_SIZE;
  #endif
#endif
}
