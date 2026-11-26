#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    FLASH_LIB_OK = 0,        /* operation done                                       */
    FLASH_LIB_EMPTY,         /* read: nothing saved yet, or last save was cut short  */
    FLASH_LIB_ERR_PARAM,     /* NULL, length 0, length > init size; init: size 0
                                or too big for FLASH_DATA                            */
    FLASH_LIB_ERR_NOT_INIT,  /* flash_init() not called, or it failed               */
    FLASH_LIB_ERR_LAYOUT,    /* FLASH_DATA does not start on a page/sector boundary */
    FLASH_LIB_ERR_HAL        /* flash erase or program failed                        */
} flash_lib_status_t;


flash_lib_status_t flash_init(uint32_t size);
flash_lib_status_t flash_write(const void *data, size_t data_len);
flash_lib_status_t flash_read(void *buffer, size_t buffer_len);

#endif /* FLASH_H */