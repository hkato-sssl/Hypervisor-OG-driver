/*
 * hvc_p128.c
 *
 * (C) 2020 Hidekazu Kato
 */

#include "asm/types.h"
#include "hvc_p128.h"

/* defines */

#define P128_CMD(c1, c0)        (((u64)(c1) << 24) | ((u64)(c0) << 16))
#define P128_CMD_NI             P128_CMD('N', 'I')
#define P128_CMD_GI             P128_CMD('G', 'I')
#define P128_CMD_GS             P128_CMD('G', 'S')
#define P128_CMD_WR             P128_CMD('W', 'R')
#define P128_CMD_RD             P128_CMD('R', 'D')

/* types */

/* prototypes */

int hvc_p128_read_u16(u64 ip0, u16 *p);
int hvc_p128_read_u32(u64 ip0, u32 *p);
int hvc_p128_send_asm(u64 ip0, const char *buff);
int hvc_p128_receive_asm(u64 ip0, char *buff);

/* variables */

/* functions */

static inline u64 hvc_command_id(u32 id, u64 command, u16 ifno)
{
    u64 d;

    d = ((u64)id << 32) | command | ifno;

    return d;
}

int hvc_p128_nr_interfaces(u32 id, u16 *nr_ifs)
{
        int ret;
        u64 ip0;

        ip0 = hvc_command_id(id, P128_CMD_NI, 0);
        ret = hvc_p128_read_u16(ip0, nr_ifs);

        return ret;
}

int hvc_p128_get_interrupt_no(u32 id, u16 ifno, u16 *no)
{
        int ret;
        u64 ip0;

        ip0 = hvc_command_id(id, P128_CMD_GI, ifno);
        ret = hvc_p128_read_u16(ip0, no);

        return ret;
}

int hvc_p128_get_status(u32 id, u16 ifno, u32 *status)
{
        int ret;
        u64 ip0;

        ip0 = hvc_command_id(id, P128_CMD_GS, ifno);
        ret = hvc_p128_read_u32(ip0, status);

        return ret;
}

int hvc_p128_send(u32 id, u16 ifno, const char *buff)
{
        int ret;
        u64 ip0;

        ip0 = hvc_command_id(id, P128_CMD_WR, ifno);
        ret = hvc_p128_send_asm(ip0, buff);

        return ret;
}

int hvc_p128_receive(u32 id, u16 ifno, char *buff)
{
        int ret;
        u64 ip0;

        ip0 = hvc_command_id(id, P128_CMD_RD, ifno);
        ret = hvc_p128_receive_asm(ip0, buff);

        return ret;
}

