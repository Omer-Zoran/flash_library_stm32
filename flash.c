#include "flash.h"
#include "main.h"      /* CubeMX main.h pulls in the HAL of the target family */
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
#else
  #error "flash_lib: this STM32 family is not supported (see README)"
#endif

/* Clear leftover error flags; a stale flag makes the next erase/program fail */
#if defined(FLASH_FLAG_ALL_ERRORS)            /* G4, L4, C0, WB */
  #define FLASH_LIB_CLEAR_ERRORS()  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS)
#elif defined(FLASH_SR_ERRORS)                /* G0: HAL macro clears one flag at a time */
  #define FLASH_LIB_CLEAR_ERRORS()  WRITE_REG(FLASH->SR, FLASH_SR_ERRORS)
#else                                         /* F0, F3 */
  #define FLASH_LIB_CLEAR_ERRORS()  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR)
#endif

/* Defined in the linker script (.ld) */
extern uint8_t _flash_data_start[];
extern uint8_t _flash_data_end[];

/* Layout in FLASH_DATA:  [ MAGIC (8 bytes) ][ user data ... ]
 * MAGIC is written LAST, so it marks a completed save. Missing MAGIC means
 * a new chip or a save cut short by power loss -> read returns EMPTY. */
#define FLASH_LIB_MAGIC     0x464C4942UL      /* "FLIB" */
#define FLASH_LIB_HDR_SIZE  8U                /* one double-word */

typedef struct
{
  FLASH_EraseInitTypeDef erase_init;
  uint32_t flash_data_start;
  uint32_t flash_data_end;
  uint32_t size;
} flash_lib_context_t;

static flash_lib_context_t context = { 0 };


static void set_erase_location(uint32_t addr);

flash_lib_status_t flash_init(uint32_t size)
{
  context.size = 0U;          /* invalid until success */
  context.flash_data_start = (uint32_t)_flash_data_start;
  context.flash_data_end   = (uint32_t)_flash_data_end;

  /* Erase works on whole pages: a misaligned area would erase code before it */
  if ((context.flash_data_start % FLASH_PAGE_SIZE) != 0U)
  {
    return FLASH_LIB_ERR_LAYOUT;
  }

  if(size == 0 || (size + FLASH_LIB_HDR_SIZE) > (context.flash_data_end - context.flash_data_start))
  {
    return FLASH_LIB_ERR_PARAM;
  }

  context.size = size;
  context.erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
  set_erase_location(context.flash_data_start);
  context.erase_init.NbPages = (size + FLASH_LIB_HDR_SIZE + FLASH_PAGE_SIZE - 1U) / FLASH_PAGE_SIZE;

  return FLASH_LIB_OK;
}

flash_lib_status_t flash_write(const void *data, size_t data_length)
{

  const uint8_t *src = (const uint8_t *)data;
  uint32_t page_error = 0U;
  HAL_StatusTypeDef st = HAL_OK;

  if (context.size == 0U)                  { return FLASH_LIB_ERR_NOT_INIT; }
  if ((data == NULL) || (data_length == 0U) ||
      (data_length > context.size))        { return FLASH_LIB_ERR_PARAM; }

  HAL_FLASH_Unlock(); // enable writing to the flash
  FLASH_LIB_CLEAR_ERRORS();

  if (HAL_FLASHEx_Erase(&context.erase_init, &page_error) != HAL_OK)
  {
    HAL_FLASH_Lock();
    return FLASH_LIB_ERR_HAL;
  }

  for(uint32_t i = 0; (st == HAL_OK) && (i < data_length); i += 8)
  {
    uint64_t data_to_write = 0xFFFFFFFFFFFFFFFFULL;
    uint32_t address = context.flash_data_start + FLASH_LIB_HDR_SIZE + i;
    uint32_t bytes_to_write = (data_length - i) >= 8 ? 8 : (data_length - i);
    memcpy(&data_to_write, src + i, bytes_to_write);
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, data_to_write);
  }

  /* Commit: write MAGIC only after all data is in flash */
  if (st == HAL_OK)
  {
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                           context.flash_data_start, (uint64_t)FLASH_LIB_MAGIC);
  }

  HAL_FLASH_Lock(); //lock the option writing to the flash
  return st == HAL_OK ? FLASH_LIB_OK : FLASH_LIB_ERR_HAL;
}

flash_lib_status_t flash_read(void *buffer, size_t bytes_n)
{

  if (context.size == 0U)
  { 
    return FLASH_LIB_ERR_NOT_INIT; 
  }

  if ((buffer == NULL) || (buffer_length == 0U) || (buffer_length > context.size))
  {
    return FLASH_LIB_ERR_PARAM; 
  }

  if (*(const uint32_t *)context.flash_data_start != FLASH_LIB_MAGIC)
  {
    return FLASH_LIB_EMPTY;            /* new chip, or save was cut short */
  }

  size_t flash_data_size = context.flash_data_end - context.flash_data_start;
  size_t bytes_to_read = (bytes_n > flash_data_size) ? flash_data_size : bytes_n
  memcpy(buffer, (const void *)(context.flash_data_start + FLASH_LIB_HDR_SIZE), bytes_to_read);

  return FLASH_LIB_OK;
}

/* Fill the bank + first page of the erase from the address, so they always agree */
static void set_erase_location(uint32_t addr)
{
#if defined(FLASH_LIB_ERASE_BY_PAGE_ADDRESS)
  context.erase_init.PageAddress = addr;
#elif defined(FLASH_BANK_2)                     /* dual-bank chip */
  if (addr >= (FLASH_BASE + FLASH_BANK_SIZE))
  {
    context.erase_init.Banks = FLASH_BANK_2;
    context.erase_init.Page  = (addr - (FLASH_BASE + FLASH_BANK_SIZE)) / FLASH_PAGE_SIZE;
  }
  else
  {
    context.erase_init.Banks = FLASH_BANK_1;
    context.erase_init.Page  = (addr - FLASH_BASE) / FLASH_PAGE_SIZE;
  }
#else                                           /* single-bank chip */
  #if defined(FLASH_LIB_HAS_BANKS_FIELD)
  context.erase_init.Banks = FLASH_BANK_1;
  #endif
  context.erase_init.Page  = (addr - FLASH_BASE) / FLASH_PAGE_SIZE;
#endif
}


