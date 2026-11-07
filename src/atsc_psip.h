/*
 * MuMuDVB - Stream a DVB/ATSC transport stream.
 *
 * ATSC PSIP helpers: parse MGT, forward EIT/ETT PIDs, decode EIT-0/ETT text,
 * and optionally sort guide sections per channel source_id.
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

/** Cap stored EIT/ETT sections used for per-channel guide sorting. */
#define MAX_ATSC_GUIDE_SECTIONS 128

/** ATSC table_id values (A/65). */
#define ATSC_TABLE_ID_MGT 0xC7
#define ATSC_TABLE_ID_EIT 0xCB
#define ATSC_TABLE_ID_ETT 0xCC
#define ATSC_TABLE_ID_STT 0xCD

/** MGT table_type values we forward (A/65 Table 6.3). */
#define ATSC_TABLE_TYPE_CHANNEL_ETT 0x0004
#define ATSC_TABLE_TYPE_EIT_FIRST    0x0100
#define ATSC_TABLE_TYPE_EIT_LAST     0x017F
#define ATSC_TABLE_TYPE_ETT_FIRST    0x0200
#define ATSC_TABLE_TYPE_ETT_LAST     0x027F

/**@brief One stored ATSC EIT/ETT section for per-channel guide emit */
typedef struct atsc_guide_section_t {
	int in_use;
	int source_id;
	int pid;
	uint8_t table_id;
	uint8_t version;
	uint8_t section_number;
	uint8_t last_section_number;
	int len;
	unsigned char data[MAX_TS_SIZE];
} atsc_guide_section_t;

/**@brief Runtime state for ATSC PSIP EIT/ETT pass-through and parsing */
typedef struct atsc_psip_t {
	/** Enable MGT parsing, EIT/ETT PID forwarding and EIT-0/ETT decode (default 1). */
	int enabled;
	/** When set (typically via sort_eit=1), only emit EIT/ETT sections matching
	 * each channel's atsc_source_id instead of forwarding the raw mux guide. */
	int sort_guide;
	/** Last seen MGT version_number, -1 if none yet. */
	int mgt_version;
	/** GPS seconds from STT when available; 0 if unknown. */
	uint32_t gps_time;
	int have_gps_time;
	/** PID for EIT-0 (MGT table_type 0x0100), 0 if unknown. */
	int eit0_pid;
	/** PID for event ETT-0 (MGT table_type 0x0200), 0 if unknown. */
	int ett0_pid;
	/** PID for channel ETT (MGT table_type 0x0004), 0 if unknown. */
	int channel_ett_pid;
	/** Section assembler for base PSIP PID (MGT/STT). */
	mumudvb_ts_packet_t *mgt_packet;
	/** Per-extra-PID section assemblers (parallel to extra_pids). */
	mumudvb_ts_packet_t *extra_assemblers[MAX_ATSC_PSIP_EXTRA_PIDS];
	/** Extra PIDs (EIT/ETT) to ask and forward on ATSC. */
	int extra_pids[MAX_ATSC_PSIP_EXTRA_PIDS];
	int num_extra_pids;
	/** Stored EIT/ETT sections for sort_guide emit (heap). */
	atsc_guide_section_t *guide_store;
} atsc_psip_t;

void atsc_psip_init(atsc_psip_t *psip);
void atsc_psip_free(atsc_psip_t *psip);
int atsc_psip_read_configuration(atsc_psip_t *psip, char *substring);
int atsc_psip_is_extra_pid(const atsc_psip_t *psip, int pid);
void atsc_psip_mark_asked(atsc_psip_t *psip, mumu_chan_p_t *chan_p);
/** Feed a TS packet; pid must be the packet's PID. */
int atsc_psip_new_packet(atsc_psip_t *psip, int pid, unsigned char *ts_packet,
		mumu_chan_p_t *chan_p, fds_t *fds, char *card_base_path, int tuner);
/** Emit stored EIT/ETT sections for this channel's source_id on the given PID. */
void atsc_psip_emit_guide_for_channel(atsc_psip_t *psip, mumudvb_channel_t *channel,
		int pid, unsigned char *ts_packet,
		struct unicast_parameters_t *unicast_vars, void *scam_vars_v);

#endif
