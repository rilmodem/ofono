/****************************************************************************
**
** This file is part of the Qt Extended Opensource Package.
**
** Copyright (C) 2009 Trolltech ASA.
**
** Contact: Qt Extended Information (info@qtextended.org)
**
** This file may be used under the terms of the GNU General Public License
** version 2.0 as published by the Free Software Foundation and appearing
** in the file LICENSE.GPL included in the packaging of this file.
**
** Please review the following information to ensure GNU General Public
** Licensing requirements will be met:
**     http://www.fsf.org/licensing/licenses/info/GPLv2.html.
**
**
****************************************************************************/

#ifndef GSM0710_P_H
#define GSM0710_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt Extended API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#ifdef __cplusplus
extern "C" {
#endif

#define GSM0710_BUFFER_SIZE             4096
#define GSM0710_DEFAULT_FRAME_SIZE      31
#define GSM0710_MAX_CHANNELS            63

#define GSM0710_MODE_BASIC              0
#define GSM0710_MODE_ADVANCED           1

/* Frame types and subtypes */
#define GSM0710_OPEN_CHANNEL            0x3F
#define GSM0710_CLOSE_CHANNEL           0x53
#define GSM0710_DATA                    0xEF
#define GSM0710_DATA_ALT                0x03
#define GSM0710_STATUS_SET              0xE3
#define GSM0710_STATUS_ACK              0xE1
#define GSM0710_TERMINATE_BYTE1         0xC3
#define GSM0710_TERMINATE_BYTE2         0x01

/* Status flags */
#define GSM0710_FC                      0x02
#define GSM0710_DTR                     0x04
#define GSM0710_DSR                     0x04
#define GSM0710_RTS                     0x08
#define GSM0710_CTS                     0x08
#define GSM0710_DCD                     0x80

struct gsm0710_context
{
    /* GSM 07.10 implementation details */
    int     mode;
    int     frame_size;
    int     port_speed;
    int     server;
    char    buffer[GSM0710_BUFFER_SIZE];
    int     buffer_used;
    unsigned long used_channels[(GSM0710_MAX_CHANNELS + 31) / 32];
    const char *reinit_detect;
    int     reinit_detect_len;

    /* Hooks to other levels */
    void   *user_data;
    int     fd;
    int     (*at_command)(struct gsm0710_context *ctx, const char *cmd);
    int     (*read)(struct gsm0710_context *ctx, void *data, int len);
    int     (*write)(struct gsm0710_context *ctx, const void *data, int len);
    void    (*deliver_data)(struct gsm0710_context *ctx, int channel,
                            const void *data, int len);
    void    (*deliver_status)(struct gsm0710_context *ctx,
                              int channel, int status);
    void    (*debug_message)(struct gsm0710_context *ctx, const char *msg);
    void    (*open_channel)(struct gsm0710_context *ctx, int channel);
    void    (*close_channel)(struct gsm0710_context *ctx, int channel);
    void    (*terminate)(struct gsm0710_context *ctx);
    int     (*packet_filter)(struct gsm0710_context *ctx, int channel,
                             int type, const char *data, int len);
};

void gsm0710_initialize(struct gsm0710_context *ctx);
void gsm0710_set_reinit_detect(struct gsm0710_context *ctx, const char *str);
int gsm0710_startup(struct gsm0710_context *ctx, int send_cmux);
void gsm0710_shutdown(struct gsm0710_context *ctx);
int gsm0710_open_channel(struct gsm0710_context *ctx, int channel);
void gsm0710_close_channel(struct gsm0710_context *ctx, int channel);
int gsm0710_is_channel_open(struct gsm0710_context *ctx, int channel);
void gsm0710_ready_read(struct gsm0710_context *ctx);
void gsm0710_write_frame(struct gsm0710_context *ctx, int channel, int type,
                         const char *data, int len);
void gsm0710_write_data(struct gsm0710_context *ctx, int channel,
                        const void *data, int len);
void gsm0710_set_status(struct gsm0710_context *ctx, int channel, int status);
int gsm0710_compute_crc(const char *data, int len);

#ifdef __cplusplus
};
#endif

#endif
