/*
 * MuMuDVB - Stream a DVB/ATSC transport stream.
 *
 * Parse the ATSC Master Guide Table (MGT) and open/forward EIT/ETT PIDs.
 *
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/** @file
 *  @brief ATSC PSIP MGT parsing for EIT/ETT pass-through
 *
 *  TVCT/CVCT already ride on the base PSIP PID (8187). Event Information
 *  Tables and Extended Text Tables are listed in the MGT and usually use
 *  other PIDs. This module discovers those PIDs and keeps demux filters
 *  open so clients receive program titles and extended text in the TS.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "atsc_psip.h"
#include "errors.h"
#include "log.h"

static char *log_module = "ATSC PSIP: ";

static int atsc_psip_table_type_wanted(uint16_t table_type)
{
	if (table_type == ATSC_TABLE_TYPE_CHANNEL_ETT)
		return 1;
	if (table_type >= ATSC_TABLE_TYPE_EIT_FIRST && table_type <= ATSC_TABLE_TYPE_EIT_LAST)
		return 1;
	if (table_type >= ATSC_TABLE_TYPE_ETT_FIRST && table_type <= ATSC_TABLE_TYPE_ETT_LAST)
		return 1;
	return 0;
}

static int atsc_psip_pid_already_listed(const atsc_psip_t *psip, int pid)
{
	int i;
	for (i = 0; i < psip->num_extra_pids; i++)
	{
		if (psip->extra_pids[i] == pid)
			return 1;
	}
	return 0;
}

void atsc_psip_init(atsc_psip_t *psip)
{
	memset(psip, 0, sizeof(*psip));
	psip->enabled = 1;
	psip->mgt_version = -1;
}

void atsc_psip_free(atsc_psip_t *psip)
{
	if (psip->mgt_packet)
	{
		free(psip->mgt_packet);
		psip->mgt_packet = NULL;
	}
}

int atsc_psip_read_configuration(atsc_psip_t *psip, char *substring)
{
	char delimiteurs[] = CONFIG_FILE_SEPARATOR;

	if (!strcmp(substring, "atsc_psip_eit"))
	{
		substring = strtok(NULL, delimiteurs);
		psip->enabled = atoi(substring);
		return 1;
	}
	return 0;
}

int atsc_psip_is_extra_pid(const atsc_psip_t *psip, int pid)
{
	if (!psip || !psip->enabled)
		return 0;
	return atsc_psip_pid_already_listed(psip, pid);
}

void atsc_psip_mark_asked(atsc_psip_t *psip, mumu_chan_p_t *chan_p)
{
	int i;
	if (!psip || !chan_p)
		return;

	/* Clear previous permanent flags for PIDs we manage, then re-apply. */
	for (i = 0; i < 8192; i++)
	{
		if (chan_p->permanent_pid[i] && !atsc_psip_pid_already_listed(psip, i))
			chan_p->permanent_pid[i] = 0;
	}

	if (!psip->enabled)
		return;

	for (i = 0; i < psip->num_extra_pids; i++)
	{
		int pid = psip->extra_pids[i];
		if (pid > 0 && pid < 8192)
		{
			chan_p->permanent_pid[pid] = 1;
			if (chan_p->asked_pid[pid] == PID_NOT_ASKED)
				chan_p->asked_pid[pid] = PID_ASKED;
		}
	}
}

/** @brief Parse a complete MGT section and refresh the extra PID list. */
static int atsc_psip_parse_mgt(atsc_psip_t *psip, unsigned char *buf, int len,
		mumu_chan_p_t *chan_p, fds_t *fds, char *card_base_path, int tuner)
{
	psip_t *header;
	int section_length;
	int tables_defined;
	int delta;
	int i;
	int new_pids[MAX_ATSC_PSIP_EXTRA_PIDS];
	int new_count = 0;
	int changed = 0;
	int version;

	if (len < PSIP_HEADER_LEN + 2 + 4) /* header + tables_defined + CRC */
		return 0;

	header = (psip_t *)buf;
	if (header->table_id != ATSC_TABLE_ID_MGT)
		return 0;
	if (header->current_next_indicator == 0)
		return 0;

	version = header->version_number;
	section_length = HILO(header->section_length);
	/* section_length counts from after that field through CRC; total section = 3 + section_length */
	if (section_length + 3 > len)
	{
		log_message(log_module, MSG_DETAIL, "MGT section_length %d larger than buffer %d, skip\n",
				section_length, len);
		return 0;
	}

	/* Same MGT version and we already have PIDs: nothing to do */
	if (version == psip->mgt_version && psip->num_extra_pids > 0)
		return 0;

	delta = PSIP_HEADER_LEN;
	tables_defined = (buf[delta] << 8) | buf[delta + 1];
	delta += 2;

	log_message(log_module, MSG_INFO,
			"MGT version %d, tables_defined=%d (was version %d, %d extra PIDs)\n",
			version, tables_defined, psip->mgt_version, psip->num_extra_pids);

	for (i = 0; i < tables_defined; i++)
	{
		uint16_t table_type;
		uint16_t table_type_pid;
		uint16_t desc_len;
		int end_limit = 3 + section_length - 4; /* exclude CRC */

		if (delta + 11 > end_limit)
		{
			log_message(log_module, MSG_WARN, "MGT truncated while reading table %d\n", i);
			break;
		}

		table_type = (buf[delta] << 8) | buf[delta + 1];
		delta += 2;
		table_type_pid = ((buf[delta] & 0x1f) << 8) | buf[delta + 1];
		delta += 2;
		/* skip version byte */
		delta += 1;
		/* skip number_bytes */
		delta += 4;
		desc_len = ((buf[delta] & 0x0f) << 8) | buf[delta + 1];
		delta += 2;

		if (delta + desc_len > end_limit)
		{
			log_message(log_module, MSG_WARN, "MGT descriptors overrun for table_type 0x%04x\n",
					table_type);
			break;
		}
		delta += desc_len;

		if (!atsc_psip_table_type_wanted(table_type))
			continue;
		if (table_type_pid == 0 || table_type_pid == PSIP_PID || table_type_pid >= 8191)
			continue;

		if (new_count < MAX_ATSC_PSIP_EXTRA_PIDS)
		{
			int j;
			int listed = 0;
			for (j = 0; j < new_count; j++)
			{
				if (new_pids[j] == (int)table_type_pid)
				{
					listed = 1;
					break;
				}
			}
			if (!listed)
			{
				new_pids[new_count++] = table_type_pid;
				log_message(log_module, MSG_DETAIL,
						"MGT: table_type 0x%04x -> PID %d (EIT/ETT)\n",
						table_type, table_type_pid);
			}
		}
		else
		{
			log_message(log_module, MSG_WARN,
					"MGT: more than %d EIT/ETT PIDs, truncating list\n",
					MAX_ATSC_PSIP_EXTRA_PIDS);
			break;
		}
	}

	if (new_count != psip->num_extra_pids)
		changed = 1;
	else
	{
		for (i = 0; i < new_count; i++)
		{
			if (!atsc_psip_pid_already_listed(psip, new_pids[i]))
			{
				changed = 1;
				break;
			}
		}
	}

	psip->mgt_version = version;
	if (changed)
	{
		memcpy(psip->extra_pids, new_pids, sizeof(int) * new_count);
		psip->num_extra_pids = new_count;

		pthread_mutex_lock(&chan_p->lock);
		atsc_psip_mark_asked(psip, chan_p);
		pthread_mutex_unlock(&chan_p->lock);

		log_message(log_module, MSG_INFO,
				"Forwarding %d ATSC PSIP EIT/ETT PID(s) discovered from MGT\n",
				psip->num_extra_pids);
		update_chan_filters(chan_p, card_base_path, tuner, fds);
	}
	else if (psip->num_extra_pids == 0 && new_count == 0)
	{
		log_message(log_module, MSG_DETAIL, "MGT contained no EIT/ETT PIDs to forward\n");
	}

	return 1;
}

int atsc_psip_new_packet(atsc_psip_t *psip, unsigned char *ts_packet,
		mumu_chan_p_t *chan_p, fds_t *fds, char *card_base_path, int tuner)
{
	if (!psip || !psip->enabled || !ts_packet)
		return 0;

	if (!psip->mgt_packet)
	{
		psip->mgt_packet = malloc(sizeof(mumudvb_ts_packet_t));
		if (!psip->mgt_packet)
		{
			log_message(log_module, MSG_ERROR, "malloc failed for MGT packet: %s\n",
					strerror(errno));
			return -1;
		}
		memset(psip->mgt_packet, 0, sizeof(mumudvb_ts_packet_t));
		pthread_mutex_init(&psip->mgt_packet->packetmutex, NULL);
	}

	while (get_ts_packet(ts_packet, psip->mgt_packet))
	{
		ts_packet = NULL; /* pop remaining full sections without feeding more TS */
		atsc_psip_parse_mgt(psip, psip->mgt_packet->data_full, psip->mgt_packet->len_full,
				chan_p, fds, card_base_path, tuner);
	}
	return 0;
}
