/*
 * Copyright (c) 2014 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include "cld-diag-parser.h"
#ifdef CONFIG_ANDROID_LOG
#include <android/log.h>

#define FWDEBUG_LOG_NAME        "ROME"
#define FWDEBUG_NAME            "ROME_DEBUG"
#define android_printf(...) \
       __android_log_print(ANDROID_LOG_INFO, FWDEBUG_LOG_NAME, __VA_ARGS__);
#endif

typedef struct diag_entry{
    uint32_t id;
    boolean isUsed;

    /* database - userspace */
    char *format;
    char *pack;

    /* runtime message - generated by target */
    char *msg;
    uint32_t msg_len;
}diag_entry;

typedef struct file_header {
    int32_t  file_version;
    int32_t  n_entries;
    int32_t  n_usedEntries;
    int32_t  hash;
}file_header;

static diag_entry *gdiag_db = NULL;
static file_header *gdiag_header = NULL;
static int32_t gisdiag_init = FALSE;
static int gdiag_sock_fd = 0, goptionflag = 0;
#ifdef CONFIG_ANDROID_LOG
#define debug_printf(...) do {     \
    if (goptionflag & DEBUG_FLAG)   \
       __android_log_print(ANDROID_LOG_INFO, FWDEBUG_NAME, __VA_ARGS__);    \
} while(0)
#endif

/*
 * macros to safely extract 8, 16, 32, or 64-bit values from byte buffer
 */
#define GET_8(v, msg, msg_len) do {     \
    if (msg_len < sizeof(uint8_t)) {    \
        goto msg_error;                 \
    }                                   \
    v = *msg;                           \
    msg += sizeof(uint8_t);             \
    msg_len -= sizeof(uint8_t);         \
} while (0)

#define _GET_LE16(a) ( \
        (((uint16_t)(a)[1]) << 8) | \
         ((uint16_t)(a)[0]))
#define GET_LE16(v, msg, msg_len) do {  \
    if (msg_len < sizeof(uint16_t)) {   \
        goto msg_error;                 \
    }                                   \
    v = _GET_LE16(msg);                 \
    msg += sizeof(uint16_t);            \
    msg_len -= sizeof(uint16_t);        \
} while (0)

#define _GET_LE32(a) ( \
        (((uint32_t)(a)[3]) << 24) | \
        (((uint32_t)(a)[2]) << 16) | \
        (((uint32_t)(a)[1]) << 8) | \
         ((uint32_t)(a)[0]))
#define GET_LE32(v, msg, msg_len) do {  \
    if (msg_len < sizeof(uint32_t)) {   \
        goto msg_error;                 \
    }                                   \
    v = _GET_LE32(msg);                 \
    msg += sizeof(uint32_t);            \
    msg_len -= sizeof(uint32_t);        \
} while (0)

#define _GET_LE64(a) ( \
        (((uint64_t)(a)[7]) << 56) | \
        (((uint64_t)(a)[6]) << 48) | \
        (((uint64_t)(a)[5]) << 40) | \
        (((uint64_t)(a)[4]) << 32) | \
        (((uint64_t)(a)[3]) << 24) | \
        (((uint64_t)(a)[2]) << 16) | \
        (((uint64_t)(a)[1]) << 8) | \
         ((uint64_t)(a)[0]))
#define GET_LE64(v, msg, msg_len) do {  \
    if (msg_len < sizeof(uint64_t)) {   \
        goto msg_error;                 \
    }                                   \
    v = _GET_LE64(msg);                 \
    msg += sizeof(uint64_t);            \
    msg_len -= sizeof(uint64_t);        \
} while (0)

/*
 * pack_printf derived from Rome FW's cmnos_vprintf
 *
 */

#define is_digit(c) ((c >= '0') && (c <= '9'))

static int _cvt(uint64_t val, char *buf, long radix, char *digits)
{
    char temp[80];
    char *cp = temp;
    int32_t length = 0;

    if (val == 0) {
        /* Special case */
        *cp++ = '0';
    } else {
        while (val) {
            *cp++ = digits[val % (int)radix];
            val /= (int)radix;
        }
    }
    while (cp != temp) {
        *buf++ = *--cp;
        length++;
    }
    *buf = '\0';
    return (length);
}

/* Return successive characters in a format string. */
char fmt_next_char(const char **fmtptr)
{
    char ch;

    ch = **fmtptr;

    if (ch != '\0') {
        (*fmtptr)++;
    }

    return ch;
}

static int
pack_printf(
        void (*putc)(char **pbs, char *be, char c),
        char **pbuf_start,
        char *buf_end,
        const char *fmt,
        const char *pack,
        uint8_t *msg,
        uint32_t msg_len)
{
    char buf[sizeof(long long)*8];
    char c, sign, *cp=buf;
    int32_t left_prec, right_prec, zero_fill, pad, pad_on_right,
        i, islong, islonglong;
    long long val = 0;
    int32_t res = 0, length = 0;

    while ((c = fmt_next_char(&fmt)) != '\0') {
        if (c == '%') {
            c = fmt_next_char(&fmt);
            left_prec = right_prec = pad_on_right = islong = islonglong = 0;
            if (c == '-') {
                c = fmt_next_char(&fmt);
                pad_on_right++;
            }
            if (c == '0') {
                zero_fill = 1;
                c = fmt_next_char(&fmt);
            } else {
                zero_fill = 0;
            }
            while (is_digit(c)) {
                left_prec = (left_prec * 10) + (c - '0');
                c = fmt_next_char(&fmt);
            }
            if (c == '.') {
                c = fmt_next_char(&fmt);
                zero_fill++;
                while (is_digit(c)) {
                    right_prec = (right_prec * 10) + (c - '0');
                    c = fmt_next_char(&fmt);
                }
            } else {
                right_prec = left_prec;
            }
            sign = '\0';
            if (c == 'l') {
                /* 'long' qualifier */
                c = fmt_next_char(&fmt);
                islong = 1;
                if (c == 'l') {
                    /* long long qualifier */
                    c = fmt_next_char(&fmt);
                    islonglong = 1;
                }
            }
            /* Fetch value [numeric descriptors only] */
            switch (c) {
            case 'p':
                islong = 1;
            case 'd':
            case 'D':
            case 'x':
            case 'X':
            case 'u':
            case 'U':
            case 'b':
            case 'B':
                switch (fmt_next_char(&pack)) {
                case 'b':
                    GET_8(val, msg, msg_len);
                    break;
                case 'h':
                    GET_LE16(val, msg, msg_len);
                    break;
                case 'i':
                case 'I':
                    GET_LE32(val, msg, msg_len);
                    break;
                case 'q':
                    GET_LE64(val, msg, msg_len);
                    break;
                default:
                    c = 0;
                    break;
                }
                if ((c == 'd') || (c == 'D')) {
                    if (val < 0) {
                        sign = '-';
                        val = -val;
                    }
                } else {
                    /* Mask to unsigned, sized quantity */
                    if (!islonglong) {
                        if (islong) {
                            val &= ((long long)1 << (sizeof(long) * 8)) - 1;
                        } else{
                            val &= ((long long)1 << (sizeof(int) * 8)) - 1;
                        }
                    }
                }
                break;
            default:
                break;
            }
            /* Process output */
            switch (c) {
            case 'p':  /* Pointer */
                (*putc)(pbuf_start, buf_end,'0');
                (*putc)(pbuf_start, buf_end,'x');
                zero_fill = 1;
                left_prec = sizeof(unsigned long)*2;
            case 'd':
            case 'D':
            case 'u':
            case 'U':
            case 'x':
            case 'X':
                switch (c) {
                case 'd':
                case 'D':
                case 'u':
                case 'U':
                    length = _cvt(val, buf, 10, "0123456789");
                    break;
                case 'p':
                case 'x':
                    length = _cvt(val, buf, 16, "0123456789abcdef");
                    break;
                case 'X':
                    length = _cvt(val, buf, 16, "0123456789ABCDEF");
                    break;
                }
                cp = buf;
                break;
            case 's':
            case 'S':
                cp = NULL; /* TODO string literals not supported yet */
                if (cp == NULL)  {
                    cp = "<null>";
                }
                length = 0;
                while (cp[length] != '\0') length++;
                break;
            case 'c':
            case 'C':
                switch (fmt_next_char(&pack)) {
                case 'b':
                    GET_8(c, msg, msg_len);
                    break;
                case 'h':
                    GET_LE16(c, msg, msg_len);
                    break;
                case 'i':
                case 'I':
                    GET_LE32(c, msg, msg_len);
                    break;
                case 'q':
                    GET_LE64(c, msg, msg_len);
                    break;
                default:
                    c = 0;
                    break;
                }
                (*putc)(pbuf_start, buf_end,c);
                res++;
                continue;
            case 'b':
            case 'B':
                length = left_prec;
                if (left_prec == 0) {
                    if (islonglong)
                        length = sizeof(long long)*8;
                    else if (islong)
                        length = sizeof(long)*8;
                    else
                        length = sizeof(uint32_t)*8;
                }
                for (i = 0;  i < length-1;  i++) {
                    buf[i] = ((val & ((long long)1<<i)) ? '1' : '.');
                }
                cp = buf;
                break;
            case '%':
                (*putc)(pbuf_start, buf_end,'%');
                break;
            default:
                (*putc)(pbuf_start, buf_end,'%');
                (*putc)(pbuf_start, buf_end,c);
                res += 2;
            }
            pad = left_prec - length;
            if (sign != '\0') {
                pad--;
            }
            if (zero_fill) {
                c = '0';
                if (sign != '\0') {
                    (*putc)(pbuf_start, buf_end,sign);
                    res++;
                    sign = '\0';
                }
            } else {
                c = ' ';
            }
            if (!pad_on_right) {
                while (pad-- > 0) {
                    (*putc)(pbuf_start, buf_end,c);
                    res++;
                }
            }
            if (sign != '\0') {
                (*putc)(pbuf_start, buf_end,sign);
                res++;
            }
            while (length-- > 0) {
                c = *cp++;
                (*putc)(pbuf_start, buf_end,c);
                res++;
            }
            if (pad_on_right) {
                while (pad-- > 0) {
                    (*putc)(pbuf_start, buf_end,' ');
                    res++;
                }
            }
        } else {
            (*putc)(pbuf_start, buf_end,c);
            res++;
        }
    }
    (*putc)(pbuf_start, buf_end, '\0');
msg_error:
    return (res);
}

static void
format_pack( const char *pack,  char *buf, uint32_t buflen)
{
    char c;
    uint32_t num = 0, index = 0, i = 0;
    boolean isfound = 0;
    memset(buf, 0 , buflen);
    while ((c = fmt_next_char(&pack)) != '\0') {
        if (index >= buflen -1)
            break;
        while (is_digit(c)) {
            num = (i++ * 10) + (c - '0');
            c = fmt_next_char(&pack);
            isfound = TRUE;
        }
        if (isfound) {
            while (num--) {
                buf[index++] = c;
                if (index >= buflen -1)
                    break;
            }
            num = 0;
            i = 0;
        }
        else
            buf[index++] = c;
    }
    buf[index] = '\0';
}

static int
diag_printf(const char *buf,  uint16_t vdevid,  uint16_t level,
            uint32_t optionflag, uint32_t timestamp, FILE *log_out)
{
    char pbuf[512];
    if (vdevid < DBGLOG_MAX_VDEVID)
        snprintf(pbuf, 512, "FWMSG: [%u] vap-%u %s", timestamp, vdevid, buf);
    else
        snprintf(pbuf, 512, "FWMSG: [%u] %s", timestamp, buf);

    if (optionflag & QXDM_FLAG) {
       switch(level) {
       case DBGLOG_VERBOSE:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_LOW, "%s", pbuf);
       break;
       case DBGLOG_INFO:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_MED , "%s", pbuf);
       break;
       case DBGLOG_INFO_LVL_1:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_MED , "%s", pbuf);
       break;
       case DBGLOG_INFO_LVL_2:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_MED , "%s", pbuf);
       break;
       case DBGLOG_WARN:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_HIGH, "%s", pbuf);
       break;
       case DBGLOG_ERR:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_HIGH, "%s", pbuf);
       break;
       case DBGLOG_LVL_MAX:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_FATAL, "%s", pbuf);
       break;
       default:
           MSG_SPRINTF_1(MSG_SSID_WLAN, MSG_LEGACY_FATAL, "%s", pbuf);
       break;
       }
    } else if (optionflag & CONSOLE_FLAG) {
        android_printf("%s\n", pbuf);
    }
    else if (optionflag & LOGFILE_FLAG) {
        if (log_out)
            return fprintf(log_out, "%s\n", pbuf);
    }
    return 0;
}

/*
 * database initialization
 */
static void
diag_create_db(uint32_t n_entries)
{
    gdiag_header = calloc(1, sizeof(*gdiag_header));
    if (!gdiag_header)
       return;
    gdiag_header->n_entries = n_entries;
    gdiag_db = calloc(gdiag_header->n_entries, sizeof(*gdiag_db));
    if (!gdiag_db)
       return;
    /* hash */
    gdiag_header->hash = (gdiag_header->n_entries % 2 == 0) ?  \
                               gdiag_header->n_entries / 2 :   \
                              (gdiag_header->n_entries + 1) / 2;
}

/*
 * database free
 */
static void
diag_free_db()
{
    int32_t count = 0;
    if (gdiag_db && gdiag_header) {
        for (count = 0; count < gdiag_header->n_entries; count++) {
            if (gdiag_db[count].isUsed){
                if (gdiag_db[count].format)
                    free(gdiag_db[count].format);
                if (gdiag_db[count].pack)
                    free(gdiag_db[count].pack);
            }
        }
    }
    if (gdiag_db)
        free(gdiag_db);
    gdiag_db = NULL;
    if (gdiag_header)
        free(gdiag_header);
    gdiag_header = NULL;
    gisdiag_init = FALSE;
}

/*
 * insert into database
 */
static int32_t
diag_insert_db(char *format, char *pack, int32_t id)
{
    /* Double Hashing  */
    int32_t i = id % gdiag_header->n_entries;
    int32_t j = gdiag_header->hash - (id % gdiag_header->hash);
    if (gdiag_header->n_entries == gdiag_header->n_usedEntries) {
        debug_printf("db is full");
        return 0;
    }
    /* search */
    while (gdiag_db[i].isUsed) {
        i = (i + j)%gdiag_header->n_entries;
    }

    gdiag_db[i].id = id;
    gdiag_db[i].format = format;
    gdiag_db[i].pack = pack;
    gdiag_db[i].isUsed = TRUE;
    gdiag_header->n_usedEntries++;
    return 1;
}

/*
 * parser looks up entry at runtime based on 'id' extracted from FW
 * message
 */
static diag_entry*
diag_find_by_id( uint32_t id)
{
    boolean isfound = FALSE;
    int32_t count = 0;
    int32_t i = id % gdiag_header->n_entries;
    int32_t j = gdiag_header->hash - (id % gdiag_header->hash);
    if (gdiag_header->n_usedEntries == 0) {
        return NULL;
    }
    while (gdiag_db[i].isUsed != 0 && count <= gdiag_header->n_entries) {
            if (gdiag_db[i].id == id) {
                isfound = TRUE;
                break;
            }
            i = (i + j) % gdiag_header->n_entries;
            count++;
    }
    if (!isfound) {
        debug_printf("Not found in data base\n");
        return NULL;
    }
    return &gdiag_db[i];
}

/* user  supply their own function to build string in temporary
 * buffer
 */
static void dbg_write_char(char **pbuf_start, char *buf_end, char c)
{
    if ( *pbuf_start < buf_end) {
        *(*pbuf_start) = c;
        ++(*pbuf_start);
    }
}

static uint32_t
get_numberofentries()
{
    FILE* fd;
    char line[1024];
    int32_t n_entries = 0, i = 0;
    boolean  isfound = FALSE;
    if ((fd = fopen(DB_FILE_PATH, "r")) == NULL) {
        diag_printf("[Error] : While opening the file\n",
                      0, 4, goptionflag, 0, NULL);
        return 0;
    }
    while ( fgets (line, sizeof(line), fd) != NULL ) {
        n_entries++;
    }
    /* Decrement 1 for version and the last line /r/n */
    n_entries-= 2;

    /* check if n_entries is prime number else change to prime number */
    while (1) {
        for (i = 2; i<n_entries; i++) {
            if ( n_entries % i == 0 ) {
           /* n_entries is divisible, break for loop */
                isfound = TRUE;
                break;
            }
        }
        if (!isfound && n_entries > 2)
            break;
        isfound = FALSE;
       /* Increment n_entries and check is it prime number */
        n_entries++;
    }
    fclose(fd);
    debug_printf( "Number of entries is %d\n", n_entries);
    return n_entries;
}

static uint32_t
parse_dbfile()
{
    FILE* fd;
    uint32_t n_entries = 0;
    uint32_t id = 0;
    char line[1024], *p = NULL, *pack = NULL, *format = NULL;
    char pbuf[128], *q = NULL;
    char *save;
    n_entries = get_numberofentries( );
    diag_create_db(n_entries );
    n_entries = 0;
     /*Open the data.msc file*/
    if ((fd = fopen(DB_FILE_PATH , "r")) == NULL) {
        diag_printf("[Error] : While opening the file\n",
                   0, 4, goptionflag, 0, NULL);
        return 0;
    }
    memset(line, 0 , sizeof(line));
    while ( fgets (line, sizeof(line), fd) != NULL ) {
        n_entries++;
        if (n_entries == 1) {
             /* Parse for the version */
            p = strstr(line, "VERSION:");
            if (p) {
                p += strlen("VERSION:");
                gdiag_header->file_version = atoi(p);
            }
            else
                return 0;
        }
        else {
            p = strtok_r(line, ",", &save);
            if (p)
                id = atoi(p);
            else
                continue;

            p = strtok_r(NULL, ",", &save);
            if (p)
                pack = strdup(p);
            else
                continue;

            p = strtok_r(NULL, "\r", &save);
            if (p) {
                format = strdup(p);
                if (format) {
                    /* Check for CR */
                    p = strstr(format, "\r");
                    if (p)
                       *p = '\0';
                    else {
                       p = strstr(format, "\n");
                       if (p)
                         *p = '\0';
                    }
                }
            }
            else {
                /* Else CASE for pack specifier is 0  */
                if (pack) {
                    /* Check for CR */
                    p = strstr(pack, "\r");
                    if (p)
                       *p = '\0';
                    else {
                       p = strstr(pack, "\n");
                       if (p)
                         *p = '\0';
                    }
                    format = pack;
                    pack = NULL;
                }
            }
            /* Go through the pack specifier, to find pack with number */
            if (pack) {
                q = pack;
                format_pack(pack, pbuf, sizeof(pbuf));
                pack = strdup(pbuf);
                free(q);
            }
            if (!diag_insert_db(format, pack, id))
                return 0;
        }
        memset(line, 0 , sizeof(line));
    }
    fclose(fd);
    return n_entries;
}

int
cnssdiag_register_kernel_logging(int sock_fd, struct nlmsghdr *nlh)
{
    tAniNlHdr *wnl;
    tAniNlAppRegReq *regReq;
    int regMsgLen = 0;

    if (!nlh)
       return -1;
    /* Only the msg header is being carried */
    nlh->nlmsg_len =  aniNlAlign(sizeof(tAniNlHdr));
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_type = WLAN_NL_MSG_CNSS_HOST_MSG;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq++;
    wnl = (tAniNlHdr *)nlh;
    wnl->radio = 0;
    wnl->wmsg.length = sizeof(tAniHdr);
    wnl->wmsg.type = ANI_NL_MSG_LOG_REG_TYPE;
    if (sendto(sock_fd, (char*)wnl, nlh->nlmsg_len,0,NULL, 0) < 0) {
        return -1;
    }

    regMsgLen = aniNlLen(sizeof(tAniNlAppRegReq));
    nlh->nlmsg_len = aniNlAlign(sizeof(tAniNlHdr)) + regMsgLen;
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_type = WLAN_NL_MSG_CNSS_HOST_EVENT_LOG;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq++;
    wnl = (tAniNlHdr *)nlh;
    wnl->radio = 0;
    wnl->wmsg.length = regMsgLen;
    wnl->wmsg.type = htons(ANI_NL_MSG_LOG_REG_TYPE);
    regReq = (tAniNlAppRegReq *)(wnl + 1);
    regReq->pid = getpid();
    if (sendto(sock_fd, (char*)wnl, nlh->nlmsg_len,0,NULL, 0) < 0) {
        return -1;
    }
    return 0;
}

static int32_t
sendcnss_cmd(int sock_fd, int32_t cmd)
{
    struct dbglog_slot slot;
    struct sockaddr_nl src_addr, dest_addr;
    struct nlmsghdr *nlh = NULL;
    struct msghdr msg;
    struct iovec iov;
    int32_t ret;

    memset(&slot, 0 , sizeof(struct dbglog_slot));
    slot.diag_type = cmd;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.nl_family = AF_NETLINK;
    dest_addr.nl_pid = 0; /* For Linux Kernel */
    dest_addr.nl_groups = 0; /* unicast */

    nlh = malloc(NLMSG_SPACE(sizeof(struct dbglog_slot)));
    if (nlh == NULL) {
        fprintf(stderr, "Cannot allocate memory \n");
        close(sock_fd);
        return -1;
    }
    memset(nlh, 0, NLMSG_SPACE(sizeof(struct dbglog_slot)));
    nlh->nlmsg_len = NLMSG_SPACE(sizeof(struct dbglog_slot));
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_type = WLAN_NL_MSG_CNSS_DIAG;
    nlh->nlmsg_flags = NLM_F_REQUEST;

    memcpy(NLMSG_DATA(nlh), &slot, sizeof(struct dbglog_slot));

    iov.iov_base = (void *)nlh;
    iov.iov_len = nlh->nlmsg_len;
    msg.msg_name = (void *)&dest_addr;
    msg.msg_namelen = sizeof(dest_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ret  = sendmsg(sock_fd, &msg, 0);
    free(nlh);
    return ret;
}


void
diag_initialize(boolean isDriverLoaded, int sock_fd, uint32_t optionflag)
{
    if (isDriverLoaded) {
        if (!gisdiag_init) {
            uint32_t ret;
            goptionflag = optionflag;
            diag_free_db();
            ret = parse_dbfile();
            if (ret > 1)
                gisdiag_init = TRUE;
            gdiag_sock_fd = sock_fd;
        }
    } else {
        gdiag_sock_fd = 0;
        gisdiag_init = FALSE;
    }
}

void
process_diaghost_msg(uint8_t *datap, uint16_t len)
{
    uint8_t  *payload;
    event_report_t *pEvent_report =(event_report_t *)datap ;
    if (!pEvent_report)
        return;
    debug_printf("\n  %s diag_type = %d event_id =%d\n",
                 __func__, pEvent_report->diag_type,
                 pEvent_report->event_id);
    if (pEvent_report->diag_type == DIAG_TYPE_EVENTS) {
        payload = datap + sizeof(event_report_t);
        wlan_bringup_t *pwlan_bringup_status = (wlan_bringup_t *)payload ;
        event_report_payload(pEvent_report->event_id,
                                pEvent_report->length, payload);
    }
}

uint32_t
process_diagfw_msg(uint8_t *datap, uint16_t len, uint32_t optionflag,
      FILE *log_out, int32_t *record, int32_t max_records, int32_t version,
      int sock_fd)
{
    uint32_t count = 0, index = 0;
    uint32_t timestamp = 0;
    uint32_t diagid = 0, id = 0;
    uint32_t moduleid = 0, res = 0;
    uint32_t num_buf = 0, payloadlen = 0;
    uint16_t vdevid = 0, vdevlevel = 0;
    uint16_t numargs = 0;
    uint32_t *buffer;
    uint32_t header1 = 0, header2 = 0;
    int32_t  lrecord = 0;
    char *payload;
    char buf[BUF_SIZ], payload_buf[BUF_SIZ];
    char *start = buf;
    int32_t hashInd = 0, i =0, j =0;
    diag_entry *entry = NULL;
    int ret = 0, total_dump_len = 0;
    uint8_t *debugp = datap;
    char dump_buffer[BUF_SIZ];

    if (optionflag & DEBUG_FLAG) {
        memset(dump_buffer, 0, sizeof(dump_buffer));
        debug_printf("process_diagfw_msg hex dump start len %d", len);
        for (i = 0; i < len; i++) {
             ret = snprintf(dump_buffer + j, BUF_SIZ - j, "0x%x ", debugp[i]);
             j += ret;
             if (!(i % 16) && (i!=0)) {
                total_dump_len += 16;
                debug_printf("%s", dump_buffer);
                memset(dump_buffer, 0, sizeof(dump_buffer));
                j = 0;
             }
        }
        if (total_dump_len != len)
           debug_printf("%s", dump_buffer);
        debug_printf("process_diagfw_msg hex dump end");
    }

    if (!gisdiag_init) {
       /* If cnss_diag is started if WIFI already ON,
        * then turn on event not received hence
        * before throwing out error initialize again
        */
        diag_initialize(1, sock_fd, optionflag);
        if (!gisdiag_init) {
            diag_printf("**ERROR** Diag not Initialized",
                          0, 4, optionflag, 0, NULL);
            return -1;
        }

    }
    buffer = (uint32_t *)datap  ;
    buffer ++; /* increment 1 to skip dropped */
    num_buf = len - 4;
    debug_printf("\n --%s-- %d\n", __FUNCTION__, optionflag);

    while (num_buf  > count) {

        header1 = *(buffer + index);
        header2 = *(buffer + 1 + index);
        payload = (char *)(buffer + 2 + index);
        diagid  = DIAG_GET_TYPE(header1);
        timestamp = DIAG_GET_TIME_STAMP(header1);
        payloadlen = 0;
        debug_printf("\n diagid = %d  timestamp = %d"
                    " header1 = %x heade2 = %x\n",
                     diagid,  timestamp, header1, header2);
        switch (diagid) {
        case WLAN_DIAG_TYPE_EVENT:
        {
            id = DIAG_GET_ID(header2);
            payloadlen = DIAG_GET_PAYLEN16(header2);
            debug_printf("DIAG_TYPE_FW_EVENT: id = %d"
                         " payloadlen = %d \n", id, payloadlen);
            if (optionflag & QXDM_FLAG) {
                if (payloadlen)
                    event_report_payload(id, payloadlen, payload);
                else
                    event_report(id);
             }
        }
        break;
        case WLAN_DIAG_TYPE_LOG:
        {
            id = DIAG_GET_ID(header2);
            payloadlen = DIAG_GET_PAYLEN16(header2);
            debug_printf("DIAG_TYPE_FW_LOG: id = %d"
                         " payloadlen = %d \n", id,  payloadlen);
            if (optionflag & QXDM_FLAG) {
                /* Allocate a log buffer */
                uint8_t *logbuff = (uint8_t*) log_alloc(id,
                                    sizeof(log_hdr_type)+payloadlen);
                if ( logbuff != NULL ) {
                    /* Copy the log data */
                    memcpy(logbuff + sizeof(log_hdr_type), payload,
                              payloadlen);
                    /* Commit the log buffer */
                    log_commit(logbuff);
                }
                else
                    debug_printf("log_alloc failed for len = %d ", payloadlen);
            }
        }
        break;
        case WLAN_DIAG_TYPE_MSG:
        {
            id = DIAG_GET_ID(header2);
            payloadlen = DIAG_GET_PAYLEN(header2);
            vdevid = DIAG_GET_VDEVID(header2);
            vdevlevel = DIAG_GET_VDEVLEVEL(header2);
            memset(buf, 0, BUF_SIZ);
            memset(payload_buf, 0, BUF_SIZ);
            debug_printf(" DIAG_TYPE_FW_DEBUG_MSG: "
                   " vdevid %d vdevlevel %d payloadlen = %d id = %d\n",
                                  vdevid, vdevlevel, payloadlen, id);
            if (gdiag_header->file_version != version) {
                snprintf(buf, BUF_SIZ, "**ERROR**"
                " Data.msc Version %d doesn't match"
                " with Firmware version %d id = %d",
                gdiag_header->file_version, version, id);
                diag_printf(buf, 0, 4, optionflag, 0, NULL);
                break;
            }
            entry = diag_find_by_id(id);
            if (entry) {
                if (entry->format && entry->pack) {
                    debug_printf("entry->format = %s pack = %s\n",
                                    entry->format, entry->pack);
                }
                if ((payloadlen > 0) && entry->pack) {
                    if (payloadlen < BUF_SIZ)
                        memcpy(payload_buf, payload, payloadlen);
                    else
                        memcpy(payload_buf, payload, BUF_SIZ);
                    /* Sending with BUF_SIZ to pack_printf
                     * because some times payloadlen received
                     * doesnt match with the pack specifier, in
                     * that case just print the zero
                     */
                    entry->msg_len = BUF_SIZ;
                    entry->msg = payload_buf;
                    start = buf;
                    pack_printf(
                             dbg_write_char,
                             &start,
                             start + sizeof(buf),
                             entry->format,
                             entry->pack,
                             (uint8_t*)entry->msg,
                             entry->msg_len
                              );
                }
                else if (entry->format)
                    strlcpy(buf, entry->format, strlen(entry->format));

                debug_printf("\n buf = %s \n", buf);
                if (optionflag & LOGFILE_FLAG)  {
                    lrecord = *record;
                    lrecord++;
                    if (!((optionflag & SILENT_FLAG) == SILENT_FLAG))
                        printf("%d: %s\n", lrecord, buf);

                    res = diag_printf(
                         buf, vdevid, vdevlevel, optionflag, timestamp, log_out
                              );
                    //fseek(log_out, lrecord * res, SEEK_SET);
                    if (lrecord == max_records) {
                        lrecord = 0;
                        fseek(log_out, lrecord * res, SEEK_SET);
                    }
                    *record = lrecord;
                }
                if (optionflag & (CONSOLE_FLAG | QXDM_FLAG))
                    diag_printf(
                         buf, vdevid, vdevlevel, optionflag, timestamp, NULL
                              );
            }
            else {
                switch (id) {
                case DIAG_WLAN_MODULE_STA_PWRSAVE:
                case DIAG_WLAN_MODULE_WAL:
                case DIAG_NAN_MODULE_ID:
                case DIAG_WLAN_MODULE_IBSS_PWRSAVE:
                    if (!diag_msg_handler(id, payload, vdevid, timestamp)) {
                        snprintf(buf, BUF_SIZ,
                            "****WARNING****, undefined moduleid = %d no t"
                            " found", moduleid);
                        diag_printf(buf, 0, 4, optionflag, timestamp, NULL);
                    }
                break;
                default:
                    snprintf(buf, BUF_SIZ,
                             "****WARNING****, FWMSG ID %d not found", id);
                    diag_printf(buf, 0, 4, optionflag, timestamp, NULL);
                    printf( "NOT found id = %d\n", id);
                }
            }
        }
        break;
        default:
            diag_printf(" ****WARNING**** WRONG DIAG ID", 0,
                      4, optionflag, timestamp, NULL);
        return 0;
        }
        count  += payloadlen + 8;
        index = count >> 2;
        debug_printf("Loope end:id = %d  payloadlen = %d count = %d index = %d\n",
                    id,  payloadlen,  count, index);
    }

    return (0);
}

/*
WLAN trigger command from QXDM

1) SSR
   send_data 75 41 7 0 1 0 253 1 25
2) log level
   send_data 75 41 7 0 2 0 253 1 25

75 - DIAG_SUBSYS_CMD_F
41 - DIAG_SUBSYS_WLAN
0007 - CNSS_WLAN_DIAG
1 -  CMD type
FC00 - VS Command OpCode

*/

PACK(void *) cnss_wlan_handle(PACK(void *)req_pkt, uint16_t pkt_len)
{
    PACK(void *)rsp = NULL;
    uint8_t *pkt_ptr = (uint8_t *)req_pkt + 4;
    uint16_t p_len, p_opcode;
    int32_t ret = 0;

   /* Allocate the same length as the request
   */
    rsp = diagpkt_subsys_alloc( DIAG_SUBSYS_WLAN, CNSS_WLAN_DIAG, pkt_len);
    if (rsp  != NULL)
    {
        p_len = *(pkt_ptr+3); /* VS Command packet length */
        p_opcode = (*(pkt_ptr+2) << 8) | *(pkt_ptr+1);
        debug_printf(
            "%s : p_len: %d, pkt_len -8: %d, p_opcode:%.04x  cmd = %d\n",
              __func__, p_len, pkt_len -8, p_opcode, *pkt_ptr
                 );
        if (p_len !=(pkt_len - 8) || ( p_opcode != 0xFD00))
            return rsp;
        memcpy(rsp, req_pkt, pkt_len);
        if (*pkt_ptr == CNSS_WLAN_SSR_TYPE) {
            if ((ret = system(RESTART_LEVEL))){
                if (ret <  0) {
                    return rsp;
                }
            }
            if (gdiag_sock_fd > 0)
                sendcnss_cmd(gdiag_sock_fd, DIAG_TYPE_CRASH_INJECT);
        }
    }
    else
      debug_printf("%s:Allocate response buffer error", __func__ );
    return rsp;
}

void process_cnss_host_message(tAniNlHdr *wnl, int32_t optionflag,
      FILE *log_out, int32_t *record, int32_t max_records)
{
    char *wlanLog = (char *)&wnl->wmsg.length + sizeof(wnl->wmsg.length);
    char *charCache = NULL ;

    /* Assuming every kmsg is terminated by a '\n' character,split the
     * wlanLog buffer received from the driver and log individual messages
     */
    while((charCache = strchr(wlanLog, '\n'))!= NULL) {
        *charCache = '\0';
        if (optionflag & QXDM_FLAG) {
            WLAN_LOG_TO_DIAG(MSG_SSID_WLAN_RESERVED_10, MSG_LEGACY_MED,
                             wlanLog);
        }
        else if (optionflag & LOGFILE_FLAG) {
            int32_t  lrecord = 0;
            uint32_t res = 0;
            lrecord = *record;
            lrecord++;
            if (!((optionflag & SILENT_FLAG) == SILENT_FLAG))
                printf("%d: %s\n", lrecord, wlanLog);
            res = fprintf(log_out, "%s\n", wlanLog);
            if (lrecord == max_records) {
                lrecord = 0;
                fseek(log_out, lrecord * res, SEEK_SET);
            }
            *record = lrecord;
        }
        else if (optionflag & CONSOLE_FLAG) {
            android_printf("%s\n", wlanLog);
        }
        wlanLog = charCache++;
    }
}

void process_cnss_host_diag_events_log(char *pData, int32_t optionflag)
{
    uint32_t diag_type = 0;

    if (optionflag & QXDM_FLAG) {
        if (pData) {
            diag_type = *(uint32_t*) pData;
            pData += sizeof(uint32_t);
        }
        if (diag_type == DIAG_TYPE_LOGS) {
            log_hdr_type *pHdr = (log_hdr_type*)pData;
            if (log_status(pHdr->code))
            {
                log_set_timestamp(pHdr);
                log_submit(pHdr);
            }
        }
        else if (diag_type == DIAG_TYPE_EVENTS) {
            uint16_t event_id;
            uint16_t length;
            event_id = *(uint16_t*)pData;
            pData += sizeof(uint16_t);
            length = *(uint16_t*)pData;
            pData += sizeof(uint16_t);
            event_report_payload(event_id,length,pData);
        }
    }
}
