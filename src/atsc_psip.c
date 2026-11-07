/*
 * MuMuDVB - Stream a DVB/ATSC transport stream.
 *
 * Parse ATSC MGT (forward EIT/ETT PIDs) and decode EIT-0 / ETT text.
 *
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/** @file
 *  @brief ATSC PSIP MGT / EIT-0 / ETT handling
 *
 *  - Discover EIT/ETT PIDs from the MGT and keep demux filters open
 *  - Decode EIT-0 for the current event title per source_id
 *  - Decode event ETT-0 (and channel ETT) for extended text
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include "atsc_psip.h"
#include "errors.h"
#include "log.h"

#ifdef HAVE_LIBUCSI
#include <libucsi/atsc/types.h>
#endif

static char *log_module = "ATSC PSIP: ";

/* Unix UTC 1970-01-01 to GPS 1980-01-06, plus a typical leap-second offset.
 * Refined when STT is seen. */
#define GPS_UNIX_EPOCH_DIFF 315964800
#define GPS_LEAP_SECONDS_DEFAULT 18

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

static mumudvb_ts_packet_t *atsc_psip_alloc_packet(void)
{
	mumudvb_ts_packet_t *pkt = malloc(sizeof(*pkt));
	if (!pkt)
		return NULL;
	memset(pkt, 0, sizeof(*pkt));
	pthread_mutex_init(&pkt->packetmutex, NULL);
	return pkt;
}

void atsc_psip_init(atsc_psip_t *psip)
{
	memset(psip, 0, sizeof(*psip));
	psip->enabled = 1;
	psip->mgt_version = -1;
}

void atsc_psip_free(atsc_psip_t *psip)
{
	if (psip->mgt_packet) { free(psip->mgt_packet); psip->mgt_packet = NULL; }
	if (psip->eit0_packet) { free(psip->eit0_packet); psip->eit0_packet = NULL; }
	if (psip->ett0_packet) { free(psip->ett0_packet); psip->ett0_packet = NULL; }
	if (psip->channel_ett_packet) { free(psip->channel_ett_packet); psip->channel_ett_packet = NULL; }
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

static mumudvb_channel_t *atsc_psip_find_channel_by_source(mumu_chan_p_t *chan_p, int source_id)
{
	int i;
	if (source_id <= 0)
		return NULL;
	for (i = 0; i < chan_p->number_of_channels; i++)
	{
		if (chan_p->channels[i].atsc_source_id == source_id)
			return &chan_p->channels[i];
	}
	return NULL;
}

static uint32_t atsc_psip_now_gps(const atsc_psip_t *psip)
{
	if (psip->have_gps_time)
		return psip->gps_time;
	time_t now = time(NULL);
	if (now < GPS_UNIX_EPOCH_DIFF)
		return 0;
	return (uint32_t)(now - GPS_UNIX_EPOCH_DIFF + GPS_LEAP_SECONDS_DEFAULT);
}

/** Decode ATSC multiple_string_structure into UTF-8/Latin-1 text. */
static int atsc_decode_multiple_string(const unsigned char *buf, int len, char *out, int out_size)
{
	int number_strings;
	int pos = 0;
	int best_pos = -1;
	int best_len = 0;
	int best_is_eng = 0;
	int s;

	if (!buf || len < 1 || !out || out_size < 2)
		return -1;
	out[0] = '\0';

	number_strings = buf[pos++];
	if (number_strings < 1)
		return -1;

	for (s = 0; s < number_strings && pos + 4 <= len; s++)
	{
		char lang[4];
		int number_segments;
		int seg;
		int is_eng;
		unsigned char decoded[MAX_NAME_LEN];
		int decoded_len = 0;

		lang[0] = (char)buf[pos];
		lang[1] = (char)buf[pos + 1];
		lang[2] = (char)buf[pos + 2];
		lang[3] = '\0';
		pos += 3;
		number_segments = buf[pos++];
		is_eng = (lang[0] == 'e' && lang[1] == 'n' && lang[2] == 'g');

		for (seg = 0; seg < number_segments && pos + 3 <= len; seg++)
		{
			int compression_type = buf[pos++];
			int mode = buf[pos++];
			int number_bytes = buf[pos++];
			int k;

			if (pos + number_bytes > len)
				return -1;

#ifdef HAVE_LIBUCSI
			if (compression_type != 0x00)
			{
				uint8_t *dest8 = decoded + decoded_len;
				size_t destbufsize = sizeof(decoded) - (size_t)decoded_len - 1;
				size_t destbufpos = 0;
				int n = atsc_text_segment_decode(
						(struct atsc_text_string_segment *)(buf + pos - 3),
						&dest8, &destbufsize, &destbufpos);
				if (n > 0 && decoded_len + n < (int)sizeof(decoded))
					decoded_len += n;
				pos += number_bytes;
				continue;
			}
#endif
			if (compression_type == 0x00 && mode == 0x00)
			{
				/* Uncompressed ISO Latin-1 */
				for (k = 0; k < number_bytes && decoded_len < (int)sizeof(decoded) - 1; k++)
					decoded[decoded_len++] = buf[pos + k];
			}
			else if (compression_type == 0x00 && mode == 0x3F)
			{
				/* UTF-16BE */
				for (k = 0; k + 1 < number_bytes && decoded_len < (int)sizeof(decoded) - 1; k += 2)
				{
					unsigned int cp = (buf[pos + k] << 8) | buf[pos + k + 1];
					if (cp == 0)
						break;
					if (cp < 0x80)
						decoded[decoded_len++] = (unsigned char)cp;
					else if (cp < 0x800 && decoded_len + 2 < (int)sizeof(decoded))
					{
						decoded[decoded_len++] = (unsigned char)(0xC0 | (cp >> 6));
						decoded[decoded_len++] = (unsigned char)(0x80 | (cp & 0x3F));
					}
					else if (decoded_len + 3 < (int)sizeof(decoded))
					{
						decoded[decoded_len++] = (unsigned char)(0xE0 | (cp >> 12));
						decoded[decoded_len++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
						decoded[decoded_len++] = (unsigned char)(0x80 | (cp & 0x3F));
					}
				}
			}
			/* else: unsupported compression/mode — skip segment bytes */
			pos += number_bytes;
		}

		if (decoded_len <= 0)
			continue;
		decoded[decoded_len] = '\0';

		if (best_pos < 0 || (is_eng && !best_is_eng))
		{
			best_is_eng = is_eng;
			best_len = decoded_len;
			/* stash in out temporarily */
			if (best_len >= out_size)
				best_len = out_size - 1;
			memcpy(out, decoded, (size_t)best_len);
			out[best_len] = '\0';
			best_pos = 1;
		}
	}

	return best_pos > 0 ? 0 : -1;
}

static void atsc_psip_set_title(mumudvb_channel_t *chan, int event_id, const char *title)
{
	int changed = 0;
	if (!chan || !title)
		return;
	if (chan->atsc_event_id != event_id)
		changed = 1;
	if (strncmp(chan->atsc_program_title, title, MAX_NAME_LEN) != 0)
		changed = 1;
	chan->atsc_event_id = event_id;
	{
		size_t n = strnlen(title, sizeof(chan->atsc_program_title) - 1);
		memcpy(chan->atsc_program_title, title, n);
		chan->atsc_program_title[n] = '\0';
	}
	if (changed)
	{
		log_message(log_module, MSG_INFO,
				"Now playing on \"%s\" (SID %d): \"%s\"\n",
				chan->name[0] ? chan->name : chan->service_name,
				chan->service_id,
				chan->atsc_program_title);
	}
}

static void atsc_psip_set_description(mumudvb_channel_t *chan, const char *text)
{
	int changed = 0;
	if (!chan || !text || !text[0])
		return;
	if (strncmp(chan->atsc_program_description, text, MAX_NAME_LEN) != 0)
		changed = 1;
	{
		size_t n = strnlen(text, sizeof(chan->atsc_program_description) - 1);
		memcpy(chan->atsc_program_description, text, n);
		chan->atsc_program_description[n] = '\0';
	}
	if (changed)
	{
		log_message(log_module, MSG_INFO,
				"Description for \"%s\": \"%s\"\n",
				chan->name[0] ? chan->name : chan->service_name,
				chan->atsc_program_description);
	}
}

/** Parse STT (table_id 0xCD) for GPS system time. */
static void atsc_psip_parse_stt(atsc_psip_t *psip, unsigned char *buf, int len)
{
	uint32_t system_time;
	uint8_t gps_utc_offset;
	if (len < PSIP_HEADER_LEN + 5)
		return;
	if (buf[0] != ATSC_TABLE_ID_STT)
		return;
	system_time = ((uint32_t)buf[PSIP_HEADER_LEN] << 24) |
			((uint32_t)buf[PSIP_HEADER_LEN + 1] << 16) |
			((uint32_t)buf[PSIP_HEADER_LEN + 2] << 8) |
			(uint32_t)buf[PSIP_HEADER_LEN + 3];
	gps_utc_offset = buf[PSIP_HEADER_LEN + 4];
	psip->gps_time = system_time;
	psip->have_gps_time = 1;
	log_message(log_module, MSG_DEBUG,
			"STT GPS time %u (UTC offset %u)\n", system_time, gps_utc_offset);
	(void)gps_utc_offset;
}

/** Parse one EIT-0 section and update matching channel's current title. */
static void atsc_psip_parse_eit(atsc_psip_t *psip, unsigned char *buf, int len, mumu_chan_p_t *chan_p)
{
	psip_t *header;
	int section_length;
	int source_id;
	int num_events;
	int delta;
	int end_limit;
	uint32_t now_gps;
	mumudvb_channel_t *chan;
	int best_event_id = -1;
	char best_title[MAX_NAME_LEN];
	uint32_t best_start = 0;
	int j;

	best_title[0] = '\0';

	if (len < PSIP_HEADER_LEN + 1)
		return;
	header = (psip_t *)buf;
	if (header->table_id != ATSC_TABLE_ID_EIT)
		return;
	if (header->current_next_indicator == 0)
		return;

	section_length = HILO(header->section_length);
	if (section_length + 3 > len)
		return;
	end_limit = 3 + section_length - 4;

	source_id = (buf[3] << 8) | buf[4]; /* table_id_extension */
	chan = atsc_psip_find_channel_by_source(chan_p, source_id);
	if (!chan)
	{
		log_message(log_module, MSG_FLOOD, "EIT for unknown source_id 0x%04x\n", source_id);
		return;
	}

	delta = PSIP_HEADER_LEN;
	num_events = buf[delta++];
	now_gps = atsc_psip_now_gps(psip);

	for (j = 0; j < num_events && delta + 10 <= end_limit; j++)
	{
		int event_id;
		uint32_t start_time;
		uint32_t length_in_seconds;
		int title_length;
		int descriptors_length;
		char title[MAX_NAME_LEN];

		event_id = ((buf[delta] & 0x3f) << 8) | buf[delta + 1];
		delta += 2;
		start_time = ((uint32_t)buf[delta] << 24) | ((uint32_t)buf[delta + 1] << 16) |
				((uint32_t)buf[delta + 2] << 8) | (uint32_t)buf[delta + 3];
		delta += 4;
		/* reserved(2) + ETM_location(2) + length_in_seconds(20) */
		length_in_seconds = ((uint32_t)(buf[delta] & 0x0f) << 16) |
				((uint32_t)buf[delta + 1] << 8) | (uint32_t)buf[delta + 2];
		delta += 3;
		title_length = buf[delta++];
		if (delta + title_length > end_limit)
			return;

		title[0] = '\0';
		if (title_length > 0)
			atsc_decode_multiple_string(buf + delta, title_length, title, sizeof(title));
		delta += title_length;

		if (delta + 2 > end_limit)
			return;
		descriptors_length = ((buf[delta] & 0x0f) << 8) | buf[delta + 1];
		delta += 2;
		if (delta + descriptors_length > end_limit)
			return;
		delta += descriptors_length;

		/* Prefer event covering "now"; else latest start_time so far in EIT-0 */
		if (now_gps && start_time <= now_gps && (start_time + length_in_seconds) > now_gps)
		{
			best_event_id = event_id;
			best_start = start_time;
			memcpy(best_title, title, sizeof(best_title));
			break; /* current event found */
		}
		if ((!now_gps || start_time <= now_gps) && start_time >= best_start && title[0])
		{
			best_event_id = event_id;
			best_start = start_time;
			memcpy(best_title, title, sizeof(best_title));
		}
	}

	if (best_event_id >= 0 && best_title[0])
		atsc_psip_set_title(chan, best_event_id, best_title);
	else if (best_event_id >= 0)
		atsc_psip_set_title(chan, best_event_id, "(no title)");
}

/** Parse one ETT section; attach text to the matching channel/event. */
static void atsc_psip_parse_ett(atsc_psip_t *psip, unsigned char *buf, int len, mumu_chan_p_t *chan_p)
{
	int section_length;
	int end_limit;
	uint32_t etm_id;
	int source_id;
	int event_id;
	int is_event_etm;
	mumudvb_channel_t *chan;
	char text[MAX_NAME_LEN];
	int msg_off;

	(void)psip;
	if (len < PSIP_HEADER_LEN + 4 + 1)
		return;
	if (buf[0] != ATSC_TABLE_ID_ETT)
		return;

	section_length = ((buf[1] & 0x0f) << 8) | buf[2];
	if (section_length + 3 > len)
		return;
	end_limit = 3 + section_length - 4;

	/* After protocol_version at offset 8: ETM_id (4 bytes) then text */
	msg_off = PSIP_HEADER_LEN;
	if (msg_off + 4 > end_limit)
		return;
	etm_id = ((uint32_t)buf[msg_off] << 24) | ((uint32_t)buf[msg_off + 1] << 16) |
			((uint32_t)buf[msg_off + 2] << 8) | (uint32_t)buf[msg_off + 3];
	msg_off += 4;

	source_id = (int)((etm_id >> 16) & 0xffff);
	is_event_etm = ((etm_id & 0x3) == 0x2);
	event_id = is_event_etm ? (int)((etm_id >> 2) & 0x3fff) : -1;

	chan = atsc_psip_find_channel_by_source(chan_p, source_id);
	if (!chan)
		return;

	text[0] = '\0';
	if (msg_off < end_limit)
		atsc_decode_multiple_string(buf + msg_off, end_limit - msg_off, text, sizeof(text));
	if (!text[0])
		return;

	if (is_event_etm)
	{
		/* Only attach if this ETM matches the current event (or none set yet) */
		if (chan->atsc_event_id < 0 || chan->atsc_event_id == event_id)
			atsc_psip_set_description(chan, text);
	}
	else
	{
		log_message(log_module, MSG_DETAIL,
				"Channel ETT for \"%s\": \"%s\"\n",
				chan->name[0] ? chan->name : chan->service_name, text);
	}
}

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
	int new_eit0 = 0, new_ett0 = 0, new_channel_ett = 0;

	if (len < PSIP_HEADER_LEN + 2 + 4)
		return 0;

	header = (psip_t *)buf;
	if (header->table_id != ATSC_TABLE_ID_MGT)
		return 0;
	if (header->current_next_indicator == 0)
		return 0;

	version = header->version_number;
	section_length = HILO(header->section_length);
	if (section_length + 3 > len)
		return 0;

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
		int end_limit = 3 + section_length - 4;

		if (delta + 11 > end_limit)
			break;

		table_type = (buf[delta] << 8) | buf[delta + 1];
		delta += 2;
		table_type_pid = ((buf[delta] & 0x1f) << 8) | buf[delta + 1];
		delta += 2;
		delta += 1; /* version */
		delta += 4; /* number_bytes */
		desc_len = ((buf[delta] & 0x0f) << 8) | buf[delta + 1];
		delta += 2;
		if (delta + desc_len > end_limit)
			break;
		delta += desc_len;

		if (!atsc_psip_table_type_wanted(table_type))
			continue;
		if (table_type_pid == 0 || table_type_pid == PSIP_PID || table_type_pid >= 8191)
			continue;

		if (table_type == ATSC_TABLE_TYPE_EIT_FIRST)
			new_eit0 = table_type_pid;
		else if (table_type == ATSC_TABLE_TYPE_ETT_FIRST)
			new_ett0 = table_type_pid;
		else if (table_type == ATSC_TABLE_TYPE_CHANNEL_ETT)
			new_channel_ett = table_type_pid;

		if (new_count < MAX_ATSC_PSIP_EXTRA_PIDS)
		{
			int j, listed = 0;
			for (j = 0; j < new_count; j++)
			{
				if (new_pids[j] == (int)table_type_pid) { listed = 1; break; }
			}
			if (!listed)
			{
				new_pids[new_count++] = table_type_pid;
				log_message(log_module, MSG_DETAIL,
						"MGT: table_type 0x%04x -> PID %d (EIT/ETT)\n",
						table_type, table_type_pid);
			}
		}
	}

	if (new_count != psip->num_extra_pids)
		changed = 1;
	else
	{
		for (i = 0; i < new_count; i++)
		{
			if (!atsc_psip_pid_already_listed(psip, new_pids[i]))
			{ changed = 1; break; }
		}
	}

	psip->mgt_version = version;
	psip->eit0_pid = new_eit0;
	psip->ett0_pid = new_ett0;
	psip->channel_ett_pid = new_channel_ett;

	if (changed)
	{
		memcpy(psip->extra_pids, new_pids, sizeof(int) * new_count);
		psip->num_extra_pids = new_count;

		pthread_mutex_lock(&chan_p->lock);
		atsc_psip_mark_asked(psip, chan_p);
		pthread_mutex_unlock(&chan_p->lock);

		log_message(log_module, MSG_INFO,
				"Forwarding %d ATSC PSIP EIT/ETT PID(s) (EIT-0=%d ETT-0=%d channel-ETT=%d)\n",
				psip->num_extra_pids, psip->eit0_pid, psip->ett0_pid, psip->channel_ett_pid);
		update_chan_filters(chan_p, card_base_path, tuner, fds);
	}
	return 1;
}

static void atsc_psip_feed_assembler(mumudvb_ts_packet_t **pkt_slot,
		unsigned char *ts_packet,
		void (*handler)(atsc_psip_t *, unsigned char *, int, mumu_chan_p_t *),
		atsc_psip_t *psip, mumu_chan_p_t *chan_p)
{
	if (!*pkt_slot)
	{
		*pkt_slot = atsc_psip_alloc_packet();
		if (!*pkt_slot)
		{
			log_message(log_module, MSG_ERROR, "malloc failed for PSIP section buffer\n");
			return;
		}
	}
	while (get_ts_packet(ts_packet, *pkt_slot))
	{
		ts_packet = NULL;
		handler(psip, (*pkt_slot)->data_full, (*pkt_slot)->len_full, chan_p);
	}
}

static void atsc_psip_handle_base_section(atsc_psip_t *psip, unsigned char *buf, int len,
		mumu_chan_p_t *chan_p, fds_t *fds, char *card_base_path, int tuner)
{
	if (len < 1)
		return;
	if (buf[0] == ATSC_TABLE_ID_MGT)
		atsc_psip_parse_mgt(psip, buf, len, chan_p, fds, card_base_path, tuner);
	else if (buf[0] == ATSC_TABLE_ID_STT)
		atsc_psip_parse_stt(psip, buf, len);
}

int atsc_psip_new_packet(atsc_psip_t *psip, int pid, unsigned char *ts_packet,
		mumu_chan_p_t *chan_p, fds_t *fds, char *card_base_path, int tuner)
{
	if (!psip || !psip->enabled || !ts_packet)
		return 0;

	if (pid == PSIP_PID)
	{
		if (!psip->mgt_packet)
		{
			psip->mgt_packet = atsc_psip_alloc_packet();
			if (!psip->mgt_packet)
				return -1;
		}
		while (get_ts_packet(ts_packet, psip->mgt_packet))
		{
			ts_packet = NULL;
			atsc_psip_handle_base_section(psip, psip->mgt_packet->data_full,
					psip->mgt_packet->len_full, chan_p, fds, card_base_path, tuner);
		}
		return 0;
	}

	if (psip->eit0_pid && pid == psip->eit0_pid)
	{
		atsc_psip_feed_assembler(&psip->eit0_packet, ts_packet,
				atsc_psip_parse_eit, psip, chan_p);
		return 0;
	}

	if (psip->ett0_pid && pid == psip->ett0_pid)
	{
		atsc_psip_feed_assembler(&psip->ett0_packet, ts_packet,
				atsc_psip_parse_ett, psip, chan_p);
		return 0;
	}

	if (psip->channel_ett_pid && pid == psip->channel_ett_pid)
	{
		atsc_psip_feed_assembler(&psip->channel_ett_packet, ts_packet,
				atsc_psip_parse_ett, psip, chan_p);
		return 0;
	}

	return 0;
}
