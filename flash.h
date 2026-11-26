#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <stddef.h>

typedef enum
{
    FLASH_LIB_OK = 0,        /* valid data present / operation done            */
    FLASH_LIB_EMPTY,         /* read: nothing saved yet, or last save was cut short */
    FLASH_LIB_ERR_PARAM,     /* NULL buf, len 0, or offset+len > size          */
    FLASH_LIB_ERR_NOT_INIT,  /* flash_lib_init() not called / failed           */
    FLASH_LIB_ERR_NO_SPACE,  /* requested size does not fit FLASH_DATA         */
    FLASH_LIB_ERR_LAYOUT,    /* not dual-bank, or FLASH_DATA not in Bank 2     */
    FLASH_LIB_ERR_HAL        /* erase/program/verify failed                    */
} flash_lib_status_t;


flash_lib_status_t flash_init(uint32_t size);
flash_lib_status_t flash_write(const void *data, size_t data_length);
flash_lib_status_t flash_read(void *buffer, size_t buffer_length);

#endif /* FLASH_H */