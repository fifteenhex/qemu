#ifndef MD_EVERDRIVE_H
#define MD_EVERDRIVE_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "chardev/char-fe.h"
#include "qemu/fifo8.h"

#define TYPE_MD_EVERDRIVE "md-everdrive"
OBJECT_DECLARE_SIMPLE_TYPE(MDEverdriveState, MD_EVERDRIVE)

/*
 * Everdrive SSF2 mapper
 */
#define MD_EVERDRIVE_REG_SIZE   0x30
#define MD_EVERDRIVE_NUM_BANKS  8
#define MD_EVERDRIVE_SLOT_SIZE  (512 * 1024)

#define MD_EVERDRIVE_RX_FIFO    4096    /* host -> guest response queue */
#define MD_EVERDRIVE_ARG_MAX    1024    /* per-command argument buffer  */
#define MD_EVERDRIVE_MAX_OPEN   1       /* one file at a time           */

struct MDEverdriveState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    CharFrontend chr;

    /* USB mailbox command parser */
    uint8_t  fifo_state;
    uint8_t  hdr_pos;
    uint8_t  cmd;
    uint8_t  len_pos;
    uint32_t data_len;
    uint32_t data_pos;

    /* Disk-command argument collection */
    uint32_t arg_need;
    uint32_t arg_pos;
    uint8_t  arg_buf[MD_EVERDRIVE_ARG_MAX];

    int64_t  timer_base;    /* virtual-clock ns at counter zero */

    /* Host -> guest RX FIFO */
    Fifo8    rx_fifo;

    /* Disk / file backend */
    char    *disk_dir;          /* exported host directory (property) */
    char    *dir_path;          /* currently-loaded relative dir      */
    void    *dir;               /* open DIR* for listing              */
    int      file_fd;           /* currently open file, or -1         */
    uint16_t status;            /* last command status (read via STATUS) */

    uint8_t bank[MD_EVERDRIVE_NUM_BANKS];
};

#endif /* MD_EVERDRIVE_H */
