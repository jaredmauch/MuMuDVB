/*
 * MuMuDVB - Stream a DVB/ATSC transport stream.
 *
 * ATSC PSIP helpers: parse MGT and forward EIT/ETT PIDs.
 *
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _ATSC_PSIP_H
#define _ATSC_PSIP_H

#include "mumudvb.h"
#include "ts.h"
#include "dvb.h"

/** Maximum secondary PSIP PIDs discovered from MGT (EIT/ETT). */
#define MAX_ATSC_PSIP_EXTRA_PIDS 256

/** ATSC MGT table_id (A/65). */
#define ATSC_TABLE_ID_MGT 0xC7

/** MGT table_type values we forward (A/65 Table 6.3). */
#define ATSC_TABLE_TYPE_CHANNEL_ETT 0x0004
#define ATSC_TABLE_TYPE_EIT_FIRST    0x0100
#define ATSC_TABLE_TYPE_EIT_LAST     0x017F
#define ATSC_TABLE_TYPE_ETT_FIRST    0x0200
#define ATSC_TABLE_TYPE_ETT_LAST     0x027F

/**@brief Runtime state for ATSC PSIP EIT/ETT pass-through */
typedef struct atsc_psip_t {
	/** Enable MGT parsing and EIT/ETT PID forwarding (default 1). */
	int enabled;
	/** Last seen MGT version_number, -1 if none yet. */
	int mgt_version;
	/** Section assembler for MGT on the base PSIP PID. */
	mumudvb_ts_packet_t *mgt_packet;
	/** Extra PIDs (EIT/ETT) to ask and always forward on ATSC. */
	int extra_pids[MAX_ATSC_PSIP_EXTRA_PIDS];
	int num_extra_pids;
} atsc_psip_t;

void atsc_psip_init(atsc_psip_t *psip);
void atsc_psip_free(atsc_psip_t *psip);
int atsc_psip_read_configuration(atsc_psip_t *psip, char *substring);
int atsc_psip_is_extra_pid(const atsc_psip_t *psip, int pid);
void atsc_psip_mark_asked(atsc_psip_t *psip, mumu_chan_p_t *chan_p);
int atsc_psip_new_packet(atsc_psip_t *psip, unsigned char *ts_packet,
		mumu_chan_p_t *chan_p, fds_t *fds, char *card_base_path, int tuner);

#endif
