#include "flash.h"

#include "stm32g473xx.h"
#include "stm32g4xx_hal.h"
#include "stm32g4xx_hal_def.h"
#include "stm32g4xx_hal_flash.h"
#include "stm32g4xx_hal_flash_ex.h"
#include <stddef.h>
#include <string.h>

/* Defined in STM32G473XX_FLASH.ld */
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


static uint32_t getBank(uint32_t Addr);
static uint32_t getPage(uint32_t addr);
flash_lib_status_t flash_init(uint32_t size)
{
  context.size = 0U;          /* invalid until success */
  context.flash_data_start = (uint32_t)_flash_data_start;
  context.flash_data_end   = (uint32_t)_flash_data_end;

  if(size == 0 || (size + FLASH_LIB_HDR_SIZE) > (context.flash_data_end - context.flash_data_start))
  {
    return FLASH_LIB_ERR_PARAM;
  }

  context.size = size;
  context.erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
  context.erase_init.Banks = getBank(context.flash_data_start);
  context.erase_init.Page = getPage(context.flash_data_start);
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
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

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

flash_lib_status_t flash_read(void *buffer, size_t buffer_length) 
{
   
  if (context.size == 0U)                  { return FLASH_LIB_ERR_NOT_INIT; }
  if ((buffer == NULL) || (buffer_length == 0U) ||
      (buffer_length > context.size))      { return FLASH_LIB_ERR_PARAM; }

  if (*(const uint32_t *)context.flash_data_start != FLASH_LIB_MAGIC)
  {
    return FLASH_LIB_EMPTY;            /* new chip, or save was cut short */
  }

  memcpy(buffer, (const void *)(context.flash_data_start + FLASH_LIB_HDR_SIZE), buffer_length);

  return FLASH_LIB_OK;
}

static uint32_t getBank(uint32_t addr)
{
  uint32_t bank = 0;

  if (READ_BIT(SYSCFG->MEMRMP, SYSCFG_MEMRMP_FB_MODE) == 0)
  {
  	/* No Bank swap */
    if (addr < (FLASH_BASE + FLASH_BANK_SIZE))
    {
      bank = FLASH_BANK_1;
    }
    else
    {
      bank = FLASH_BANK_2;
    }
  }
  else
  {
  	/* Bank swap */
    if (addr < (FLASH_BASE + FLASH_BANK_SIZE))
    {
      bank = FLASH_BANK_2;
    }
    else
    {
      bank = FLASH_BANK_1;
    }
  }

  return bank;
}

static uint32_t getPage(uint32_t addr)
{
 return (addr - (FLASH_BASE + FLASH_BANK_SIZE)) / FLASH_PAGE_SIZE;
}