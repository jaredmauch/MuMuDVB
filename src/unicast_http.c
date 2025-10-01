/*
 * MuMuDVB - Stream a DVB transport stream.
 *
 * (C) 2009-2013 Brice DUBOST
 *
 * The latest version can be found at http://mumudvb.net
 *
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/** @file
 * @brief File for HTTP unicast
 * @author Brice DUBOST
 * @date 2009-2013
 */

//in order to use asprintf (extension gnu)
#define _GNU_SOURCE
#define _CRT_SECURE_NO_WARNINGS

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#else
#define write(sock, buf, size) send(sock, buf, size, 0)
#define close(sock) closesocket(sock)
#endif
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#ifndef _WIN32
#include <poll.h>
#include <unistd.h>
#include <strings.h>
#include <sys/time.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>

#include "unicast_http.h"
#include "unicast_queue.h"
#include "mumudvb.h"
#include "errors.h"
#include "log.h"
#include "dvb.h"
#include "tune.h"
#include "autoconf.h"
#include "rewrite.h"
#include "network.h"
#include "unified_storage_adapter.h"
#ifdef ENABLE_CAM_SUPPORT
#include "cam.h"
#endif
#ifdef ENABLE_SCAM_SUPPORT
#include "scam_capmt.h"
#include "scam_common.h"
#include "scam_getcw.h"
#include "scam_decsa.h"
#endif

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

static char *log_module="Unicast : ";

// Global unified system reference (will be set by main system)
extern unified_channel_system_t *global_unified_system;

/**
 * @brief Get unified enhanced channel data for HTTP operations (preserves frequency/card info)
 * @param channels Output enhanced channel array
 * @param number_of_channels Output number of channels
 * @return 0 on success, -1 on error
 */
static int get_unified_enhanced_channel_data(enhanced_channel_t **channels, int *number_of_channels)
{
    if (!channels || !number_of_channels) {
        return -1;
    }
    
    // Try to get enhanced channels from unified storage v2
    if (global_unified_system && global_unified_system->unified_storage_v2 &&
        get_all_channels_adapter(channels, number_of_channels) == 0 &&
        *number_of_channels > 0) {
        
        log_message(log_module, MSG_DEBUG, "Using %d enhanced channels from unified storage v2 for validation", *number_of_channels);
        return 0;
    }
    
    // Fallback to regular channels - this will be set by the caller
    return -1; // Indicate we need to use regular channels
}


/**
 * @brief Find enhanced channel by number in the unified channel data
 * @param channel_number 1-based channel number
 * @param channels Enhanced channel array
 * @param number_of_channels Number of channels
 * @return Pointer to enhanced channel if found and ready, NULL otherwise
 */
static enhanced_channel_t *find_enhanced_channel_by_number(int channel_number, 
                                                          enhanced_channel_t *channels, 
                                                          int number_of_channels)
{
    if (!channels || channel_number <= 0 || channel_number > number_of_channels) {
        return NULL;
    }
    
    // Convert to 0-based index
    int channel_index = channel_number - 1;
    
    // Check if channel is ready
    if (channels[channel_index].base_channel.channel_ready >= READY) {
        return &channels[channel_index];
    }
    
    return NULL;
}






// Unicast file descriptor types
#define UNICAST_MASTER 1
#define UNICAST_CLIENT 2
#define UNICAST_LISTEN_CHANNEL 3

// Helper function to determine file descriptor type
static int get_fd_type(unicast_parameters_t *unicast_vars, int fd) {
	// Check if it's the master socket
	if (fd == unicast_vars->socketIn) {
		return UNICAST_MASTER;
	}
	
	// Check if it's a client socket by looking it up in the client list
	unicast_client_t *client = unicast_vars->clients;
	while (client != NULL) {
		if (client->Socket == fd) {
			return UNICAST_CLIENT;
		}
		client = client->next;
	}
	
	// Check if it's a channel listening socket by looking it up in channels
	// We need to pass channels array to this function, but for now assume it's a channel socket
	return UNICAST_LISTEN_CHANNEL;
}


//from unicast_client.c
unicast_client_t *unicast_add_client(unicast_parameters_t *unicast_vars, int Socket, const char *client_ip);
int channel_add_unicast_client(unicast_client_t *client,mumudvb_channel_t *channel);

unicast_client_t *unicast_accept_connection(unicast_parameters_t *unicast_vars, int socketIn);
void unicast_close_connection(unicast_parameters_t *unicast_vars, int Socket);

int
unicast_send_streamed_channels_list (int number_of_channels, mumudvb_channel_t *channels, int Socket, char *host);
int
unicast_send_index_page  (int Socket);
int
unicast_send_play_list_unicast (int number_of_channels, mumudvb_channel_t *channels, int Socket, int unicast_portOut, int perport, unicast_parameters_t *unicast_vars);
int
unicast_send_play_list_multicast (int number_of_channels, mumudvb_channel_t* channels, int Socket, int vlc, unicast_parameters_t *unicast_vars);
int
unicast_send_streamed_channels_list_js (int number_of_channels, mumudvb_channel_t *channels, void* cam_p_v, int Socket);
int
unicast_send_signal_power_js (int Socket, strength_parameters_t *strengthparams);
int
unicast_send_channel_traffic_js (int number_of_channels, mumudvb_channel_t *channels, int Socket);
int
unicast_send_json_state (int number_of_channels, mumudvb_channel_t* channels, int Socket, strength_parameters_t* strengthparams, auto_p_t* auto_p, void* cam_p_v, void* scam_vars_v);
int
unicast_send_prometheus (int number_of_channels, mumudvb_channel_t* channels, int Socket, strength_parameters_t* strengthparams);
int
unicast_send_xml_state (int number_of_channels, mumudvb_channel_t* channels, int Socket, strength_parameters_t* strengthparams, auto_p_t* auto_p, void* cam_p_v, void* scam_vars_v);
int
unicast_send_cam_menu (int Socket, void *cam_p);
int
unicast_send_cam_action (int Socket, char *Key, void *cam_p);
int
unicast_send_EIT (eit_packet_t *eit_packets, int Socket);


int unicast_handle_message(unicast_parameters_t* unicast_vars,
		unicast_client_t* client,
		strength_parameters_t* strengthparams,
		auto_p_t* auto_p,
		void* cam_p,
		void* scam_vars,
		eit_packet_t *eit_packets);

#define REPLY_HEADER 0
#define REPLY_BODY 1
#define REPLY_SIZE_STEP 4096


/** Initialize unicast variables*/
void init_unicast_v(unicast_parameters_t *unicast_vars)
{
	memset(unicast_vars,0,sizeof(unicast_parameters_t));
	 *unicast_vars=(unicast_parameters_t){
				.unicast=0,
				.ipOut="0.0.0.0",
				.portOut=4242,
				.portOut_str=NULL,
				.consecutive_errors_timeout=UNICAST_CONSECUTIVE_ERROR_TIMEOUT,
				.max_clients=-1,
				.queue_max_size=UNICAST_DEFAULT_QUEUE_MAX,
				.socket_sendbuf_size=0,
				.flush_on_eagain=0,
				.pfdsnum=0,
				.playlist_ignore_dead=0,
				.playlist_ignore_scrambled_ratio=0,
				.hls=0,
				.hls_rotate_time=10,
				.hls_rotate_count=2,
				.hls_rotate_iframe=0,
				.hls_storage_dir=NULL,
				.hls_playlist_name=NULL,
				.tcp_keepalive=1,
				.tcp_keepalive_idle=30,
				.tcp_keepalive_interval=5,
				.tcp_keepalive_count=3,
				.tcp_window_scaling=1,
				.tcp_selective_acks=1,
				.scan_results_refresh_delay=300,
	 };
	 unicast_vars->pfds=NULL;
	 //+1 for closing the pfd list, see man poll
	 unicast_vars->pfds=malloc(sizeof(struct pollfd));
	 if (unicast_vars->pfds==NULL)
	 {
		 log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		 set_interrupted(ERROR_MEMORY<<8);
		 return;
	 }
	 unicast_vars->pfds[0].fd = 0;
	 unicast_vars->pfds[0].events = POLLIN | POLLPRI;
	 unicast_vars->pfds[0].revents = 0;

	 // No need to initialize fd_info array - we determine types dynamically

	 unicast_vars->hls_storage_dir = malloc(MAX_NAME_LEN);
	 if (unicast_vars->hls_storage_dir==NULL)
	 {
		 log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		 set_interrupted(ERROR_MEMORY<<8);
		 return;
	 }
	 unicast_vars->hls_playlist_name = malloc(MAX_NAME_LEN);
	 if (unicast_vars->hls_playlist_name==NULL)
	 {
		 log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		 set_interrupted(ERROR_MEMORY<<8);
		 return;
	 }
	 // initialize with defaults
	 sprintf(unicast_vars->hls_storage_dir, "/tmp");
	 sprintf(unicast_vars->hls_playlist_name, "playlist.m3u8");
}



/** @brief Read a line of the configuration file to check if there is a unicast parameter
 *
 * @param unicast_vars the unicast parameters
 * @param substring The currrent line
 */
int read_unicast_configuration(unicast_parameters_t *unicast_vars, mumudvb_channel_t *c_chan, char *substring)
{

	char delimiteurs[] = CONFIG_FILE_SEPARATOR;

	if (!strcmp (substring, "ip_http"))
	{
		substring = strtok (NULL, delimiteurs);
		if (strlen(substring) > INET6_ADDRSTRLEN) {
			log_message( log_module,  MSG_ERROR, "Configuration error: IP address '%s' is too long (max %d characters). Please use a valid IPv4 or IPv6 address.\n", substring, INET6_ADDRSTRLEN-1);
			exit(ERROR_CONF);
		}
		sscanf (substring, "%s\n", unicast_vars->ipOut);
		if(unicast_vars->ipOut[0]!='\0')
		{
			if(unicast_vars->unicast==0)
			{
				log_message( log_module,  MSG_WARN,"You should use the option \"unicast=1\" before to activate unicast instead of ip_http\n");
				unicast_vars->unicast=1;
			}
		}
	}
	else if (!strcmp (substring, "unicast"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->unicast = atoi (substring);
	}
	else if (!strcmp (substring, "unicast_consecutive_errors_timeout"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->consecutive_errors_timeout = atoi (substring);
		if(unicast_vars->consecutive_errors_timeout<=0)
			log_message( log_module,  MSG_WARN,
					"Warning : You have deactivated the unicast timeout for disconnecting clients, this can lead to an accumulation of zombie clients, this is unadvised, prefer a long timeout\n");
	}
	else if (!strcmp (substring, "unicast_max_clients"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->max_clients = atoi (substring);
	}
	else if (!strcmp (substring, "unicast_queue_size"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->queue_max_size = atoi (substring);
	}
	else if (!strcmp (substring, "port_http"))
	{
		substring = strtok (NULL, "=");

		// next we replace all the spaces as too many spaces are messing with the further parsing
		// like: "     port_http            = 2000 + %card"
		int len = strlen(substring)+1;
		substring = mumu_string_replace(substring, &len, 1, " ", "");

		// if the string is empty after the replacement, we need to tokenize one more time to get the actual setting
		if (strlen(substring) == 0) {
			substring = strtok(NULL, "=");
		}

		if((strchr(substring,'*')!=NULL)||(strchr(substring,'+')!=NULL)||(strchr(substring,'%')!=NULL))
		{
			unicast_vars->portOut_str=malloc(sizeof(char)*(strlen(substring)+1));
			strcpy(unicast_vars->portOut_str,substring);
		}
		else
			unicast_vars->portOut = atoi (substring);
	}
	else if (!strcmp (substring, "unicast_port"))
	{
		if ( c_chan == NULL )
		{
			log_message( log_module,  MSG_ERROR,
					"unicast_port : You have to start a channel first (using new_channel)\n");
			exit(ERROR_CONF);
		}
		substring = strtok (NULL, delimiteurs);
		c_chan->unicast_port = atoi (substring);
		MU_F(c_chan->unicast_port) = F_USER;
	}
	else if (!strcmp (substring, "socket_sendbuf_size"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->socket_sendbuf_size = atoi (substring);
	}
	else if (!strcmp (substring, "flush_on_eagain"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->flush_on_eagain = atoi (substring);
		if(unicast_vars->flush_on_eagain)
			log_message( log_module,  MSG_INFO, "The unicast data WILL be dropped on eagain errors\n");
	}
	else if (!strcmp (substring, "playlist_ignore_dead"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->playlist_ignore_dead = atoi (substring);
	}
	else if (!strcmp (substring, "playlist_ignore_scrambled_ratio"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->playlist_ignore_scrambled_ratio = atoi (substring);
		if (unicast_vars->playlist_ignore_scrambled_ratio > 100) {
                        log_message( log_module,  MSG_WARN,"Scrambled ignore ratio \"%d\" is over 100 percent, forcing to 100!\n", unicast_vars->playlist_ignore_scrambled_ratio);
                        unicast_vars->playlist_ignore_scrambled_ratio = 100;
                }

	}
	else if (!strcmp (substring, "hls"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->hls = atoi (substring);
	}

	else if (!strcmp (substring, "hls_rotate_time"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->hls_rotate_time = atoi (substring);
		if (unicast_vars->hls_rotate_time < 1) {
                        log_message( log_module,  MSG_WARN,"HLS configuration warning: hls_rotate_time=%d is less than 1 second, setting to 1 second minimum.\n", unicast_vars->hls_rotate_time);
                        unicast_vars->hls_rotate_time = 1;
                }
	}

	else if (!strcmp (substring, "hls_rotate_count"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->hls_rotate_count = atoi (substring);
		if (unicast_vars->hls_rotate_count < 1) {
                        log_message( log_module,  MSG_WARN,"HLS configuration warning: hls_rotate_count=%d is less than 1, setting to 1 segment minimum.\n", unicast_vars->hls_rotate_count);
                        unicast_vars->hls_rotate_count = 1;
                }
	}

	else if (!strcmp (substring, "hls_rotate_iframe"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->hls_rotate_iframe = atoi (substring);
		if (unicast_vars->hls_rotate_iframe < 0) {
                        log_message( log_module,  MSG_WARN,"HLS configuration warning: hls_rotate_iframe=%d is less than 0, setting to 0 (disabled).\n", unicast_vars->hls_rotate_iframe);
                        unicast_vars->hls_rotate_iframe = 0;
                }
	}

    	else if (!strcmp (substring, "hls_storage_dir"))
        {
            	substring = strtok (NULL, delimiteurs);
                strncpy(unicast_vars->hls_storage_dir,strtok(substring,"\n"),MAX_NAME_LEN-1);
                unicast_vars->hls_storage_dir[MAX_NAME_LEN-1]='\0';
        }

    	else if (!strcmp (substring, "hls_playlist_name"))
        {
            	substring = strtok (NULL, delimiteurs);
                strncpy(unicast_vars->hls_playlist_name,strtok(substring,"\n"),MAX_NAME_LEN-1);
                unicast_vars->hls_playlist_name[MAX_NAME_LEN-1]='\0';
        }
	else if (!strcmp (substring, "tcp_keepalive"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->tcp_keepalive = atoi (substring);
	}
	else if (!strcmp (substring, "tcp_keepalive_idle"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->tcp_keepalive_idle = atoi (substring);
		if (unicast_vars->tcp_keepalive_idle < 1) {
			log_message( log_module,  MSG_WARN,"TCP keepalive idle time %d is less than 1, setting to 1 second\n", unicast_vars->tcp_keepalive_idle);
			unicast_vars->tcp_keepalive_idle = 1;
		}
	}
	else if (!strcmp (substring, "tcp_keepalive_interval"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->tcp_keepalive_interval = atoi (substring);
		if (unicast_vars->tcp_keepalive_interval < 1) {
			log_message( log_module,  MSG_WARN,"TCP keepalive interval %d is less than 1, setting to 1 second\n", unicast_vars->tcp_keepalive_interval);
			unicast_vars->tcp_keepalive_interval = 1;
		}
	}
	else if (!strcmp (substring, "tcp_keepalive_count"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->tcp_keepalive_count = atoi (substring);
		if (unicast_vars->tcp_keepalive_count < 1) {
			log_message( log_module,  MSG_WARN,"TCP keepalive count %d is less than 1, setting to 1\n", unicast_vars->tcp_keepalive_count);
			unicast_vars->tcp_keepalive_count = 1;
		}
	}
	else if (!strcmp (substring, "tcp_window_scaling"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->tcp_window_scaling = atoi (substring);
	}
	else if (!strcmp (substring, "tcp_selective_acks"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->tcp_selective_acks = atoi (substring);
	}
	else if (!strcmp (substring, "scan_results_refresh_delay"))
	{
		substring = strtok (NULL, delimiteurs);
		unicast_vars->scan_results_refresh_delay = atoi (substring);
		if (unicast_vars->scan_results_refresh_delay < 1) {
			log_message( log_module,  MSG_WARN,"Scan results refresh delay %d is less than 1, setting to 1 second\n", unicast_vars->scan_results_refresh_delay);
			unicast_vars->scan_results_refresh_delay = 1;
		}
		if (unicast_vars->scan_results_refresh_delay > 3600) {
			log_message( log_module,  MSG_WARN,"Scan results refresh delay %d is greater than 3600, setting to 3600 seconds (1 hour)\n", unicast_vars->scan_results_refresh_delay);
			unicast_vars->scan_results_refresh_delay = 3600;
		}
	}

	else
		return 0; //Nothing concerning tuning, we return 0 to explore the other possibilities

	return 1;//We found something for tuning, we tell main to go for the next line

}



/** @brief Create a listening socket and add it to the list of polling file descriptors if success
 *
 *
 *
 */
int unicast_create_listening_socket(int socket_type, int socket_channel, char *ipOut, int port, int *socketIn, unicast_parameters_t *unicast_vars)
{
	*socketIn = makeTCPclientsocket(ipOut, port);

	//We add them to the poll descriptors
	if(*socketIn>0)
	{
		unicast_vars->pfdsnum++;
		log_message( log_module, MSG_DEBUG, "unicast : creating socket type=%d, channel=%d, socket=%d, pfdsnum=%d\n", 
		           socket_type, socket_channel, *socketIn, unicast_vars->pfdsnum);
		unicast_vars->pfds=realloc(unicast_vars->pfds,(unicast_vars->pfdsnum+1)*sizeof(struct pollfd));
		if (unicast_vars->pfds==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with realloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			return -1;
		}
		unicast_vars->pfds[unicast_vars->pfdsnum-1].fd = *socketIn;
		unicast_vars->pfds[unicast_vars->pfdsnum-1].events = POLLIN | POLLPRI;
		unicast_vars->pfds[unicast_vars->pfdsnum-1].revents = 0;
		unicast_vars->pfds[unicast_vars->pfdsnum].fd = 0;
		unicast_vars->pfds[unicast_vars->pfdsnum].events = POLLIN | POLLPRI;
		unicast_vars->pfds[unicast_vars->pfdsnum].revents = 0;
		// No need to allocate fd_info array - we determine types dynamically
		
		// No need to initialize fd_info array - we determine types dynamically
	}
	else
	{
		log_message( log_module,  MSG_WARN, "Problem creating the socket %s:%d : %s\n",ipOut,port,strerror(errno) );
		return -1;
	}

	return 0;

}

/** @brief Handle an "event" on the unicast file descriptors
 * If the event is on an already open client connection, it handle the message
 * If the event is on the master connection, it accepts the new connection
 * If the event is on a channel specific socket, it accepts the new connection and starts streaming
 *
 */
int unicast_handle_fd_event(unicast_parameters_t *unicast_vars,
		strength_parameters_t *strengthparams,
		auto_p_t *auto_p,
		void *cam_p,
		void *scam_vars,
		eit_packet_t *eit_packets)
{
	int iRet;
	//We look what happened for which connection
	int actual_fd;
	
	// Debug logging for troubleshooting
	log_message(log_module, MSG_DEBUG, "unicast_handle_fd_event: pfdsnum=%d", 
	           unicast_vars->pfdsnum);

	for(actual_fd=0;actual_fd<unicast_vars->pfdsnum;actual_fd++)
	{
		iRet=0;
		
		// Bounds check to prevent accessing invalid array indices
		if (actual_fd >= unicast_vars->pfdsnum) {
			log_message(log_module, MSG_ERROR, "FATAL: Invalid fd index %d - pfdsnum=%d", 
			           actual_fd, unicast_vars->pfdsnum);
			abort();
		}
		
		// Determine file descriptor type dynamically
		int fd_type = get_fd_type(unicast_vars, unicast_vars->pfds[actual_fd].fd);
		
		if(((unicast_vars->pfds[actual_fd].revents&POLLHUP)||(unicast_vars->pfds[actual_fd].revents&POLLERR))
				&&(fd_type==UNICAST_CLIENT))
		{
			log_message( log_module, MSG_DEBUG,"We've got a POLLHUP or POLLERR. Actual_fd %d socket %d we close the connection \n", actual_fd, unicast_vars->pfds[actual_fd].fd );
			unicast_close_connection(unicast_vars,unicast_vars->pfds[actual_fd].fd);
			//We check if we have to parse unicast_vars->pfds[actual_fd].revents (the last fd moved to the actual one)
			if(unicast_vars->pfds[actual_fd].revents)
				actual_fd--;//Yes, we force the loop to see it
		}
		if((unicast_vars->pfds[actual_fd].revents&POLLIN)||(unicast_vars->pfds[actual_fd].revents&POLLPRI))
		{
			if((fd_type==UNICAST_MASTER)||
					(fd_type==UNICAST_LISTEN_CHANNEL))
			{
				//Event on the master connection or listening channel
				//New connection, we accept the connection
				log_message( log_module, MSG_FLOOD,"New client\n");
				int tempSocket;
				unicast_client_t *tempClient;
				//we accept the incoming connection
				tempClient=unicast_accept_connection(unicast_vars, unicast_vars->pfds[actual_fd].fd);

				if(tempClient!=NULL)
				{
					tempSocket=tempClient->Socket;
					unicast_vars->pfdsnum++;
					unicast_vars->pfds=realloc(unicast_vars->pfds,(unicast_vars->pfdsnum+1)*sizeof(struct pollfd));
					if (unicast_vars->pfds==NULL)
					{
						log_message( log_module, MSG_ERROR,"Problem with realloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
						set_interrupted(ERROR_MEMORY<<8);
						return -1;
					}
					//We poll the new socket
					unicast_vars->pfds[unicast_vars->pfdsnum-1].fd = tempSocket;
					unicast_vars->pfds[unicast_vars->pfdsnum-1].events = POLLIN | POLLPRI | POLLHUP | POLLERR; //We also poll the deconnections
					unicast_vars->pfds[unicast_vars->pfdsnum-1].revents = 0;
					unicast_vars->pfds[unicast_vars->pfdsnum].fd = 0;
					unicast_vars->pfds[unicast_vars->pfdsnum].events = POLLIN | POLLPRI;
					unicast_vars->pfds[unicast_vars->pfdsnum].revents = 0;

					// No need to allocate fd_info array - we determine types dynamically
					//client connection - no need to store in fd_info array


					log_message( log_module, MSG_FLOOD,"Number of clients : %d\n", unicast_vars->client_number);

					if(fd_type==UNICAST_LISTEN_CHANNEL)
					{
						//Event on a channel connection, we open a new socket for this client and
						//we store the wanted channel for when we will get the GET
						// Note: This functionality is deprecated with unified storage v2
						log_message( log_module, MSG_DEBUG,"Connection on a channel socket (deprecated with unified storage v2)\n");
						tempClient->askedChannel = -1; // No specific channel requested
					}
				}
			}
			else if(fd_type==UNICAST_CLIENT)
			{
				//Event on a client connection i.e. the client asked something
				log_message( log_module, MSG_FLOOD,"New message for socket %d\n", unicast_vars->pfds[actual_fd].fd);
				
				// Find the client associated with this socket
				unicast_client_t *client = unicast_vars->clients;
				while (client != NULL && client->Socket != unicast_vars->pfds[actual_fd].fd) {
					client = client->next;
				}
				
				if (client == NULL) {
					log_message(log_module, MSG_ERROR, "Client not found for socket %d", unicast_vars->pfds[actual_fd].fd);
					continue;
				}
				
				iRet=unicast_handle_message(unicast_vars, client, strengthparams, auto_p, cam_p, scam_vars, eit_packets);
				if (iRet==-2 ) //iRet==-2 --> 0 received data or error, we close the connection
				{
					unicast_close_connection(unicast_vars,unicast_vars->pfds[actual_fd].fd);
					//We check if we have to parse unicast_vars->pfds[actual_fd].revents (the last fd moved to the actual one)
					if(unicast_vars->pfds[actual_fd].revents)
						actual_fd--;//Yes, we force the loop to see it again
				}
			}
			else
			{
				log_message( log_module, MSG_WARN,"File descriptor with bad type, please contact\n Debug information : actual_fd %d fd_type %d\n",
						actual_fd, fd_type);
			}
		}
	}
	return 0;

}

/** @brief Accept an incoming connection
 *
 *
 * @param unicast_vars the unicast parameters
 * @param socketIn the socket on wich the connection was made
 */
unicast_client_t *unicast_accept_connection(unicast_parameters_t *unicast_vars, int socketIn)
{
	int fromSocket, iRet;
	socklen_t l;
	unicast_client_t *tempClient;
	struct sockaddr_storage fromAddrIn = { 0, };
	struct sockaddr_storage toAddrIn = { 0, };

	char fromBuf[INET6_ADDRSTRLEN];
	char toBuf[INET6_ADDRSTRLEN];

	l = sizeof(struct sockaddr_storage);
	fromSocket = accept(socketIn, (struct sockaddr *)&fromAddrIn, &l);
	if (fromSocket < 0)
	{
		log_message( log_module, MSG_WARN,"Error when accepting the incoming connection : %s\n", strerror(errno));
		return NULL;
	}

	// Configure TCP optimizations for the client socket
	configure_tcp_optimizations(fromSocket, 0, unicast_vars);  // 0 = client socket

	l = sizeof(struct sockaddr_storage);
	iRet = getsockname(fromSocket, (struct sockaddr *)&toAddrIn, &l);
	if (iRet < 0)
	{
		log_message( log_module,  MSG_ERROR,"getsockname failed : %s while accepting incoming connection", strerror(errno));
		close(fromSocket);
		return NULL;
	}

	/* turn both remote and local side into ip:addr strings for debug */
	sockaddr_to_string(&fromAddrIn, fromBuf, sizeof(fromBuf));
	sockaddr_to_string(&toAddrIn, toBuf, sizeof(toBuf));

	log_message(log_module, MSG_FLOOD, "New connection from %s to %s\n", fromBuf, toBuf);

	//Now we set this socket to be non blocking because we poll it
	int flags;
#ifndef _WIN32
	flags = fcntl(fromSocket, F_GETFL, 0);
	flags |= O_NONBLOCK;
	if (fcntl(fromSocket, F_SETFL, flags) < 0)
	{
		log_message( log_module, MSG_ERROR,"Set non blocking failed : %s\n",strerror(errno));
		close(fromSocket);
		return NULL;
	}
#else
	uint32_t iMode = 0;
	flags = ioctlsocket(fromSocket, FIONBIO, &iMode);
	if (flags != NO_ERROR) {
		log_message(log_module, MSG_ERROR, "Set non blocking failed : %s\n", strerror(errno));
		close(fromSocket);
		return NULL;
	}
#endif

	/* if the maximum number of clients is reached, raise a temporary error*/
	if((unicast_vars->max_clients>0)&&(unicast_vars->client_number>=unicast_vars->max_clients))
	{
		int iRet;

		log_message( log_module, MSG_INFO,"Too many clients connected, we raise an error to  %s\n", fromBuf);
		iRet = write(fromSocket,HTTP_503_REPLY, strlen(HTTP_503_REPLY));
		if (iRet < 0) {
			log_message(log_module, MSG_INFO, "Error writing to %s\n", fromBuf);
		}
		close(fromSocket);
		return NULL;
	}

	tempClient = unicast_add_client(unicast_vars, fromSocket, fromBuf);

	return tempClient;

}


/** @brief Close an unicast connection and delete the client
 *
 * @param unicast_vars the unicast parameters
 * @param fds The polling file descriptors
 * @param Socket The socket of the client we want to disconnect
 */
void unicast_close_connection(unicast_parameters_t *unicast_vars, int Socket)
{

	int actual_fd;
	actual_fd=0;
	//We find the FD correspondig to this client
	while((actual_fd<unicast_vars->pfdsnum) && (unicast_vars->pfds[actual_fd].fd!=Socket))
		actual_fd++;

	if(actual_fd==unicast_vars->pfdsnum)
	{
		log_message( log_module, MSG_ERROR,"close connection : we did't find the file descriptor this should never happend, please contact\n");
		actual_fd=0;
		//We find the FD correspondig to this client
		while(actual_fd<unicast_vars->pfdsnum)
		{
			log_message( log_module, MSG_ERROR,"unicast_vars->pfds[actual_fd].fd %d Socket %d \n", unicast_vars->pfds[actual_fd].fd,Socket);
			actual_fd++;
		}
		return;
	}

	log_message( log_module, MSG_FLOOD,"We close the connection\n");
	
	// Find the client associated with this socket
	unicast_client_t *client = unicast_vars->clients;
	while (client != NULL && client->Socket != unicast_vars->pfds[actual_fd].fd) {
		client = client->next;
	}
	
	// We delete the client if found
	if (client != NULL) {
		unicast_del_client(unicast_vars, client);
	}
	
	// We move the last fd to the actual/deleted one, and decrease the number of fds by one
	unicast_vars->pfds[actual_fd].fd = unicast_vars->pfds[unicast_vars->pfdsnum-1].fd;
	unicast_vars->pfds[actual_fd].events = unicast_vars->pfds[unicast_vars->pfdsnum-1].events;
	unicast_vars->pfds[actual_fd].revents = unicast_vars->pfds[unicast_vars->pfdsnum-1].revents;
	// No need to move fd_info array - we determine types dynamically
	//last one set to 0 for poll()
	unicast_vars->pfds[unicast_vars->pfdsnum-1].fd=0;
	unicast_vars->pfds[unicast_vars->pfdsnum-1].events=POLLIN|POLLPRI;
	unicast_vars->pfds[unicast_vars->pfdsnum-1].revents=0; //We clear it to avoid nasty bugs ...
	unicast_vars->pfdsnum--;
	unicast_vars->pfds=realloc(unicast_vars->pfds,(unicast_vars->pfdsnum+1)*sizeof(struct pollfd));
	if (unicast_vars->pfds==NULL)
	{
		log_message( log_module, MSG_ERROR,"Problem with realloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		set_interrupted(ERROR_MEMORY<<8);
	}
	// No need to allocate fd_info array - we determine types dynamically
	log_message( log_module, MSG_FLOOD,"Number of clients : %d\n", unicast_vars->client_number);

}




/** @brief Deal with an incoming message on the unicast client connection
 * This function will store and answer the HTTP requests
 *
 *
 * @param unicast_vars the unicast parameters
 * @param client The client from which the message was received
 * @param channels the channel array
 * @param number_of_channels quite explicit ...
 */
int unicast_handle_message(unicast_parameters_t *unicast_vars,
		unicast_client_t *client,
		strength_parameters_t *strengthparams,
		auto_p_t *auto_p,
		void *cam_p,
		void *scam_vars,
		eit_packet_t *eit_packets)
{
	int received_len;
	(void) unicast_vars;

	/************ auto increasing buffer to receive the message **************/
	if((client->buffersize-client->bufferpos)<RECV_BUFFER_MULTIPLE)
	{
		client->buffer=realloc(client->buffer,(client->buffersize + RECV_BUFFER_MULTIPLE+1)*sizeof(char)); //the +1 if for the \0 at the end
		if(client->buffer==NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with realloc for the client buffer : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			client->buffersize=0;
			client->bufferpos=0;
			return -1;
		}
		memset (client->buffer+client->buffersize, 0, RECV_BUFFER_MULTIPLE*sizeof(char)); //We fill the buffer with zeros to be sure
		client->buffersize += RECV_BUFFER_MULTIPLE;
	}

	received_len=recv(client->Socket, client->buffer+client->bufferpos, RECV_BUFFER_MULTIPLE, 0);

	if(received_len>0)
	{
		if(client->bufferpos==0)
		{
			log_message( log_module, MSG_FLOOD,"beginning of buffer %c%c%c%c%c%c\n",client->buffer[0],client->buffer[1],client->buffer[2],client->buffer[3],client->buffer[4],client->buffer[5]);
			log_message( log_module, MSG_FLOOD,"beginning of buffer %d %d %d %d %d %d\n",client->buffer[0],client->buffer[1],client->buffer[2],client->buffer[3],client->buffer[4],client->buffer[5]);
		}
		client->bufferpos+=received_len;
		log_message( log_module, MSG_FLOOD,"We received %d, buffer len %d new buffer pos %d\n",received_len,client->buffersize, client->bufferpos);
	}

	if(received_len==-1)
	{
		log_message( log_module, MSG_ERROR,"Problem with recv : %s\n",strerror(errno));
		return -1;
	}
	if(received_len==0)
		return -2; //To say to the main program to close the connection

	/***************** Now we parse the message to see if something was asked  *****************/
	client->buffer[client->buffersize]='\0'; //For avoiding strlen to look too far (other option is to use the gnu extension strnlen)
	//We search for the end of the HTTP request
	if(strlen(client->buffer)>5 && strstr(client->buffer, "\n\r\n\0"))
	{
		int pos,err404;
		char *substring=NULL;
		int requested_channel;
		int iRet;
		int bycard_error = 0;  // Track if this is a bycard path error
		int requested_card_id = -1;  // Track the requested card ID for error display
		requested_channel=0;
		pos=0;
		err404=0;
		struct unicast_reply* reply=NULL;

		log_message( log_module, MSG_FLOOD,"End of HTTP request, we parse it\n");

		if(strstr(client->buffer,"GET ")==client->buffer || strstr(client->buffer,"HEAD ")==client->buffer)
		{
			//to implement :
			//Information ???
			//GET /monitor/???

			pos=4;
			int is_head_request = (strstr(client->buffer,"HEAD ")==client->buffer);

			/* preselected channels via the port of the connection */
			//if the client have already an asked channel we don't parse the GET
			if(client->askedChannel!=-1)
			{
				requested_channel=client->askedChannel+1; //+1 because requested channel starts at 1 and asked channel starts at 0
				log_message( log_module, MSG_DEBUG,"Channel by socket, number %d\n",requested_channel);
				client->askedChannel=-1;
			}
			//Channel by number
			//GET /bynumber/channelnumber
			else if(strstr(client->buffer +pos ,"/bynumber/")==(client->buffer +pos))
			{
				if(client->chan_ptr!=NULL)
				{
					log_message( log_module, MSG_INFO,"A channel (%s) is already streamed to this client, it shouldn't ask for a new one without closing the connection, error 501\n",client->chan_ptr->name);
					iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));
					if(iRet<0)
						log_message( log_module, MSG_INFO,"Error writing reply\n");

					return -2; //to delete the client
				}

				pos+=strlen("/bynumber/");
				substring = strtok (client->buffer+pos, " ");
				if(substring == NULL)
					err404=1;
				else
				{
					requested_channel=atoi(substring);
					
					// Use enhanced channel data for validation (preserves frequency/card info)
					enhanced_channel_t *enhanced_channels = NULL;
					int enhanced_number_of_channels = 0;
					enhanced_channel_t *target_enhanced_channel = NULL;
					
					// Try to get enhanced channel data first
					if (get_unified_enhanced_channel_data(&enhanced_channels, &enhanced_number_of_channels) == 0) {
						// Use enhanced channel data
						target_enhanced_channel = find_enhanced_channel_by_number(requested_channel, enhanced_channels, enhanced_number_of_channels);
						if (target_enhanced_channel) {
							log_message( log_module, MSG_DEBUG,"Channel by number, number %d found in unified storage\n",requested_channel);
							
							// Get frequency and card info directly from enhanced channel
							double frequency = target_enhanced_channel->frequency;
							int card_id = target_enhanced_channel->card_id;
							
							if (frequency > 0) {
								// First, check if any card is already tuned to this frequency and serving clients
								int existing_card_id = -1;
								if (global_unified_system) {
									for (int i = 0; i < global_unified_system->num_cards; i++) {
										if (global_unified_system->cards[i].current_freq == frequency &&
											global_unified_system->cards[i].in_use) {
											existing_card_id = global_unified_system->cards[i].card_id;
											log_message( log_module, MSG_INFO,"Channel %d (%s) routing to existing card %d on frequency %.0f Hz (original card: %d)\n", 
													   requested_channel, target_enhanced_channel->base_channel.name, existing_card_id, frequency, card_id);
											break;
										}
									}
								}
								
								// If no card is serving this frequency, use the original card
								if (existing_card_id == -1) {
									// Use the original card from the enhanced channel data
									if (card_id >= 0) {
										// Bootstrap the card (tune it and load TS data) for this frequency
										if (bootstrap_card_for_frequency(card_id, frequency) == 0) {
											log_message( log_module, MSG_INFO,"Channel %d (%s) bootstrapped on card %d for frequency %.0f Hz (original card: %d)\n", 
													   requested_channel, target_enhanced_channel->base_channel.name, card_id, frequency, card_id);
											existing_card_id = card_id;
										} else {
											log_message( log_module, MSG_ERROR,"Failed to bootstrap card %d for channel %d frequency %.0f Hz\n", card_id, requested_channel, frequency);
											err404=1;
											requested_channel=0;
										}
									} else {
										log_message( log_module, MSG_ERROR,"No card available for channel %d frequency %.0f Hz\n", requested_channel, frequency);
										err404=1;
										requested_channel=0;
									}
								}
								
								// If we have a card (either existing or newly tuned), proceed with client addition
								if (existing_card_id >= 0) {
									// Convert enhanced channel to regular channel for client addition
									// We need to ensure the regular channels array has this channel
									if (requested_channel > 0) {
									// No need to copy to regular channels array - we use unified storage v2 directly
									log_message( log_module, MSG_DEBUG,"Using enhanced channel %d data directly from unified storage v2\n", requested_channel);
								}
								}
							} else {
								log_message( log_module, MSG_ERROR,"Channel %d has invalid frequency %.0f Hz\n", requested_channel, frequency);
								err404=1;
								requested_channel=0;
							}
						} else {
							log_message( log_module, MSG_INFO,"Channel by number, number %d not found or not ready in unified storage\n",requested_channel);
							err404=1;
							requested_channel=0;
						}
						// Free enhanced channels if we allocated them
						if (enhanced_channels) {
							free(enhanced_channels);
						}
					} else {
						// No fallback - unified storage v2 is required
						log_message( log_module, MSG_ERROR,"Unified storage v2 not available for bynumber lookup - cannot find channel %d\n",requested_channel);
						err404=1;
						requested_channel=0;
					}
				}
			}
			//Channel by autoconf_sid_list
			//GET /bysid/sid
			else if(strstr(client->buffer +pos ,"/bysid/")==(client->buffer +pos))
			{
				if(client->chan_ptr!=NULL)
				{
					log_message( log_module, MSG_INFO,"A channel (%s) is already streamed to this client, it shouldn't ask for a new one without closing the connection, error 501\n",client->chan_ptr->name);
					iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY)); //iRet is to make the copiler happy we will close the connection anyways
					return -2; //to delete the client
				}
				pos+=strlen("/bysid/");
				substring = strtok (client->buffer+pos, " ");
				if(substring == NULL)
					err404=1;
				else
				{
					int requested_sid;
					requested_sid=atoi(substring);
					
					// Check if we're in unified mode
					if (global_unified_system && global_unified_system->unified_storage_v2)
					{
						// In unified mode, SID lookups should be card-specific to avoid ambiguity
						// Find all cards that have this SID
						int found_cards[16] = {0}; // Max 16 cards
						int num_found_cards = 0;
						
						for (int card_idx = 0; card_idx < global_unified_system->num_cards && num_found_cards < 16; card_idx++)
						{
							enhanced_channel_t *card_channels = NULL;
							int num_card_channels = 0;
							
							if (get_channels_for_card_adapter(global_unified_system->cards[card_idx].card_id, &card_channels, &num_card_channels) == 0)
							{
								for (int i = 0; i < num_card_channels; i++)
								{
									if (card_channels[i].base_channel.service_id == requested_sid)
									{
										found_cards[num_found_cards] = global_unified_system->cards[card_idx].card_id;
										num_found_cards++;
										break;
									}
								}
								free(card_channels);
							}
						}
						
						if (num_found_cards == 0)
						{
							log_message( log_module, MSG_INFO,"Channel by service id, service_id %d not found in unified mode\n", requested_sid);
							err404=1;
							requested_channel=0;
						}
						else if (num_found_cards == 1)
						{
							// Only one card has this SID, use it directly
							int card_id = found_cards[0];
							enhanced_channel_t *card_channels = NULL;
							int num_card_channels = 0;
							
							if (get_channels_for_card_adapter(card_id, &card_channels, &num_card_channels) == 0)
							{
								for (int i = 0; i < num_card_channels; i++)
								{
									if (card_channels[i].base_channel.service_id == requested_sid)
									{
									// No fallback - unified storage v2 is required
									log_message( log_module, MSG_ERROR,"Unified storage v2 not available for bysid lookup - cannot find SID %d\n",requested_sid);
										break;
									}
								}
								free(card_channels);
							}
							
							if (requested_channel)
								log_message( log_module, MSG_DEBUG,"Channel by service id, service_id %d found on card %d, number %d\n", requested_sid, card_id, requested_channel);
							else
							{
								log_message( log_module, MSG_INFO,"Channel by service id, service_id %d found on card %d but not in main channel list\n", requested_sid, card_id);
								err404=1;
								requested_channel=0;
							}
						}
						else
						{
							// Multiple cards have this SID - provide helpful error message
							log_message( log_module, MSG_INFO,"Channel by service id, service_id %d found on multiple cards (%d cards). In unified mode, please use /bycard/card_id/bysid/%d format to specify which card. Available cards: ", 
										requested_sid, num_found_cards, requested_sid);
							for (int i = 0; i < num_found_cards; i++)
							{
								log_message( log_module, MSG_INFO,"%d%s", found_cards[i], (i < num_found_cards - 1) ? ", " : "");
							}
							log_message( log_module, MSG_INFO,"\n");
							err404=1;
							requested_channel=0;
						}
					}
					else
					{
						// No fallback - unified storage v2 is required
						log_message( log_module, MSG_ERROR,"Unified storage v2 not available for bysid lookup - cannot find SID %d\n",requested_sid);
						err404=1;
						requested_channel=0;
					}
				}
			}
			//Channel names list
			//GET /byname/
			else if(strstr(client->buffer +pos ,"/byname/ ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"Channel names list\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for channel names list", num_unified_channels);
						unicast_send_channel_names_list(num_unified_channels, unified_base_channels, client->Socket);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for channel names list - cannot serve request");
				err404=1;
			}
			//Channel by name
			//GET /byname/channelname
			else if(strstr(client->buffer +pos ,"/byname/")==(client->buffer +pos))
			{
				if(client->chan_ptr!=NULL)
				{
					log_message( log_module, MSG_INFO,"A channel (%s) is already streamed to this client, it shouldn't ask for a new one without closing the connection, error 501\n",client->chan_ptr->name);
					iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));//iRet is to make the copiler happy we will close the connection anyways
					return -2; //to delete the client
				}
				pos+=strlen("/byname/");
                log_message( log_module, MSG_DEBUG,"Channel by name\n");

                char *substring = client->buffer+pos;
                char *end = strstr(substring, " HTTP"); // find end of channel name (this way channel name can contain spaces)

                if(*substring == 0) {
					err404=1;
                }
                else if(end == NULL) {
                    err404=1;
                    log_message( log_module, MSG_DEBUG,"Channel name was not found in the URL `%s`\n", substring);
                }
				else
				{
                    end[0] = '\0'; // add string terminator to be able to get channel name

                    char requested_channel_name[MAX_NAME_LEN];
                    char current_channel_name[MAX_NAME_LEN];
                    strncpy(requested_channel_name, substring,MAX_NAME_LEN);
                    requested_channel_name[MAX_NAME_LEN-1] = '\0';
                    process_channel_name(requested_channel_name);

                    // Try to get channels from unified storage v2 first, fallback to regular channels
                    enhanced_channel_t *enhanced_channels = NULL;
                    int num_enhanced_channels = 0;
                    mumudvb_channel_t *unified_base_channels = NULL;
                    int num_unified_channels = 0;
                    
                    // Check if we have a unified system with storage v2
                    if (global_unified_system && global_unified_system->unified_storage_v2 &&
                        get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
                        num_enhanced_channels > 0) {
                        
                        // Convert enhanced channels to base channels for HTTP endpoint
                        if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
                                                             &unified_base_channels, &num_unified_channels) == 0) {
                            log_message(log_module, MSG_DEBUG, "Using %d channels from unified storage v2 for byname lookup", num_unified_channels);
                            
                            // Search in unified channels
                            for(int current_channel=0; current_channel<num_unified_channels;current_channel++)
                            {
                                strcpy(current_channel_name, unified_base_channels[current_channel].name);
                                process_channel_name(current_channel_name);

                                if(strcasecmp(current_channel_name, requested_channel_name) == 0)
                                    requested_channel=current_channel+1;
                            }
                            
                            free(unified_base_channels);
                            free(enhanced_channels);
                        } else {
                            free(enhanced_channels);
                        }
                    }
                    
                    // No fallback - unified storage v2 is required
                    if (requested_channel == 0) {
                        log_message(log_module, MSG_ERROR, "Unified storage v2 not available for byname lookup - cannot find channel '%s'", requested_channel_name);
                    }
                    if(requested_channel)
                        log_message( log_module, MSG_DEBUG,"Channel by name, name `%s` number `%d`\n", requested_channel_name, requested_channel);
                    else
                    {
                        log_message( log_module, MSG_INFO,"Channel by name, name `%s` not found in channel list\n", requested_channel_name);
                        err404=1;
                        requested_channel=0;
                    }
				}
			}
			//Channel by card (prefix for byname and bysid)
			//GET /bycard/card_id/byname/channelname or /bycard/card_id/bysid/sid
			else if(strstr(client->buffer +pos ,"/bycard/")==(client->buffer +pos))
			{
				if(client->chan_ptr!=NULL)
				{
					log_message( log_module, MSG_INFO,"A channel (%s) is already streamed to this client, it shouldn't ask for a new one without closing the connection, error 501\n",client->chan_ptr->name);
					iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));
					if(iRet<0)
						log_message( log_module, MSG_INFO,"Error writing reply\n");
					return -2; //to delete the client
				}
				
				pos+=strlen("/bycard/");
				char *card_substring = strtok (client->buffer+pos, "/");
				if(card_substring == NULL)
				{
					err404=1;
				}
				else
				{
					int requested_card_id = atoi(card_substring);
					log_message( log_module, MSG_DEBUG,"Channel by card, card_id %d\n", requested_card_id);
					
					// Check if we have a unified system with storage v2
					if (global_unified_system && global_unified_system->unified_storage_v2)
					{
						enhanced_channel_t *card_channels = NULL;
						int num_card_channels = 0;
						
						// Get channels for this specific card
						if (get_channels_for_card_adapter(requested_card_id, &card_channels, &num_card_channels) == 0 && num_card_channels > 0)
						{
							// Now check if the next part is byname or bysid
							char *remaining_path = strtok(NULL, " ");
							if(remaining_path == NULL)
							{
								err404=1;
							}
							else if(strstr(remaining_path, "/byname/") == remaining_path)
							{
								// Handle bycard/card_id/byname/channelname
								char *name_start = remaining_path + strlen("/byname/");
								char *end = strstr(name_start, " HTTP");
								
								if(*name_start == 0) {
									err404=1;
								}
								else if(end == NULL) {
									err404=1;
									log_message( log_module, MSG_DEBUG,"Channel name was not found in the URL `%s`\n", name_start);
								}
								else
								{
									end[0] = '\0'; // add string terminator to be able to get channel name
									
									char requested_channel_name[MAX_NAME_LEN];
									char current_channel_name[MAX_NAME_LEN];
									strncpy(requested_channel_name, name_start, MAX_NAME_LEN);
									requested_channel_name[MAX_NAME_LEN-1] = '\0';
									process_channel_name(requested_channel_name);
									
									// Search only in channels from this card
									for(int i = 0; i < num_card_channels; i++)
									{
										strcpy(current_channel_name, card_channels[i].base_channel.name);
										process_channel_name(current_channel_name);
										
										if(strcasecmp(current_channel_name, requested_channel_name) == 0)
										{
											// No fallback - unified storage v2 is required
											log_message( log_module, MSG_ERROR,"Unified storage v2 not available for bycard lookup - cannot find channel\n");
											break;
										}
									}
									
									if(requested_channel)
										log_message( log_module, MSG_DEBUG,"Channel by card and name, card_id %d, name `%s` number `%d`\n", requested_card_id, requested_channel_name, requested_channel);
									else
									{
										log_message( log_module, MSG_INFO,"Channel by card and name, card_id %d, name `%s` not found in card channel list\n", requested_card_id, requested_channel_name);
										err404=1;
										requested_channel=0;
									}
								}
							}
							else if(strstr(remaining_path, "/bysid/") == remaining_path)
							{
								// Handle bycard/card_id/bysid/sid
								char *sid_start = remaining_path + strlen("/bysid/");
								char *end = strstr(sid_start, " HTTP");
								
								if(*sid_start == 0) {
									err404=1;
								}
								else if(end == NULL) {
									err404=1;
									log_message( log_module, MSG_DEBUG,"Service ID was not found in the URL `%s`\n", sid_start);
								}
								else
								{
									end[0] = '\0'; // add string terminator to be able to get service id
									int requested_sid = atoi(sid_start);
									
									// Search only in channels from this card
									for(int i = 0; i < num_card_channels; i++)
									{
										if(card_channels[i].base_channel.service_id == requested_sid)
										{
											// No fallback - unified storage v2 is required
											log_message( log_module, MSG_ERROR,"Unified storage v2 not available for bycard bysid lookup - cannot find SID %d\n",requested_sid);
											break;
										}
									}
									
									if(requested_channel)
										log_message( log_module, MSG_DEBUG,"Channel by card and service id, card_id %d, service_id %d number %d\n", requested_card_id, requested_sid, requested_channel);
									else
									{
										log_message( log_module, MSG_INFO,"Channel by card and service id, card_id %d, service_id %d not found in card channel list\n", requested_card_id, requested_sid);
										err404=1;
										requested_channel=0;
									}
								}
							}
							else
							{
								log_message( log_module, MSG_INFO,"Invalid bycard path, expected /byname/ or /bysid/ after card_id\n");
								err404=1;
								bycard_error = 1;  // Mark this as a bycard path error
								// requested_card_id is already set above
							}
							
							free(card_channels);
						}
						else
						{
							log_message( log_module, MSG_INFO,"No channels found for card_id %d\n", requested_card_id);
							err404=1;
						}
					}
					else
					{
						log_message( log_module, MSG_INFO,"Unified system v2 not available for bycard method\n");
						err404=1;
					}
				}
			}
			//Channels list
			else if(strstr(client->buffer +pos ,"/channels_list.html ")==(client->buffer +pos))
			{
				//We get the host name if availaible
				char *hoststr;
				hoststr=strstr(client->buffer ,"Host: ");
				if(hoststr)
				{
					substring = strtok (hoststr+6, "\r");
				}
				else
					substring=NULL;
				log_message( log_module, MSG_DETAIL,"Channel list\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				log_message(log_module, MSG_INFO, "Channels list: checking unified storage v2 (global_unified_system=%p, unified_storage_v2=%p)", 
				           (void*)global_unified_system, (void*)(global_unified_system ? global_unified_system->unified_storage_v2 : NULL));
				
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2", num_unified_channels);
						unicast_send_streamed_channels_list(num_unified_channels, unified_base_channels, client->Socket, substring);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				} else {
					log_message(log_module, MSG_INFO, "Channels list: unified storage v2 not available or empty, falling back to regular channels (num_enhanced_channels=%d)", 
					           num_enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for channels list - cannot serve request");
				err404=1;
			}
			//Card utilization status
			else if(strstr(client->buffer +pos ,"/card_status.json ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"Card utilization status\n");
				unicast_send_card_utilization_status(client->Socket);
				return -2; //We close the connection afterwards
			}
			//playlist, m3u
			else if(strstr(client->buffer +pos ,"/playlist.m3u ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"play list\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for playlist", num_unified_channels);
						unicast_send_play_list_unicast(num_unified_channels, unified_base_channels, client->Socket, unicast_vars->portOut, 0, unicast_vars);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for playlist - cannot serve request");
				err404=1;
			}
			//playlist, m3u
			else if(strstr(client->buffer +pos ,"/playlist_port.m3u ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"play list\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for playlist_port", num_unified_channels);
						unicast_send_play_list_unicast(num_unified_channels, unified_base_channels, client->Socket, unicast_vars->portOut, 1, unicast_vars);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for playlist_port - cannot serve request");
				err404=1;
			}
			else if(strstr(client->buffer +pos ,"/playlist_multicast.m3u ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"play list\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for playlist_multicast", num_unified_channels);
						unicast_send_play_list_multicast(num_unified_channels, unified_base_channels, client->Socket, 0, unicast_vars);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for playlist_multicast - cannot serve request");
				err404=1;
			}
			else if(strstr(client->buffer +pos ,"/playlist_multicast_vlc.m3u ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"play list\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for playlist_multicast_vlc", num_unified_channels);
						unicast_send_play_list_multicast(num_unified_channels, unified_base_channels, client->Socket, 1, unicast_vars);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for playlist_multicast_vlc - cannot serve request");
				err404=1;
			}
			//statistics, text version
			else if(strstr(client->buffer +pos ,"/channels_list.json ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"Channel list Json\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (global_unified_system && global_unified_system->unified_storage_v2 &&
				    get_all_channels_adapter(&enhanced_channels, &num_enhanced_channels) == 0 &&
				    num_enhanced_channels > 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for JSON", num_unified_channels);
						unicast_send_streamed_channels_list_js(num_unified_channels, unified_base_channels, scam_vars, client->Socket);
						free(unified_base_channels);
						free(enhanced_channels);
						return -2;
					}
					free(enhanced_channels);
				}
				
				// No fallback - unified storage v2 is required
				log_message(log_module, MSG_ERROR, "Unified storage v2 not available for channels_list.json - cannot serve request");
				err404=1;
			}
			else if(strstr(client->buffer +pos ,"/monitor/state.json ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"HTTP request for state in Json\n");
				// TODO: Update unicast_send_json_state to use unified storage v2
				log_message(log_module, MSG_ERROR, "unicast_send_json_state needs to be updated for unified storage v2");
				err404=1;
				return -2; //We close the connection afterwards
			}
			else if(strstr(client->buffer +pos ,"/monitor/signal_power.json ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"Signal power json\n");
				unicast_send_signal_power_js(client->Socket, strengthparams);
				return -2; //We close the connection afterwards
			}
			else if(strstr(client->buffer +pos ,"/monitor/channels_traffic.json ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"Channel traffic json\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (get_unified_enhanced_channel_data(&enhanced_channels, &num_enhanced_channels) == 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for channel traffic", num_unified_channels);
						unicast_send_channel_traffic_js(num_unified_channels, unified_base_channels, client->Socket);
						free(unified_base_channels);
					} else {
						log_message(log_module, MSG_ERROR, "Failed to convert enhanced channels to base channels for channel traffic");
					}
				} else {
					log_message(log_module, MSG_ERROR, "No channels available for channel traffic");
				}
				
				return -2; //We close the connection afterwards
			}
			else if(strstr(client->buffer +pos ,"/monitor/state.xml ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"HTTP request for XML State\n");
				
				// Try to get channels from unified storage v2 first, fallback to regular channels
				enhanced_channel_t *enhanced_channels = NULL;
				int num_enhanced_channels = 0;
				mumudvb_channel_t *unified_base_channels = NULL;
				int num_unified_channels = 0;
				
				// Check if we have a unified system with storage v2
				if (get_unified_enhanced_channel_data(&enhanced_channels, &num_enhanced_channels) == 0) {
					
					// Convert enhanced channels to base channels for HTTP endpoint
					if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
					                                     &unified_base_channels, &num_unified_channels) == 0) {
						log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for XML state", num_unified_channels);
						unicast_send_xml_state(num_unified_channels, unified_base_channels, client->Socket, strengthparams, auto_p, cam_p, scam_vars);
						free(unified_base_channels);
					} else {
						log_message(log_module, MSG_ERROR, "Failed to convert enhanced channels to base channels for XML state");
					}
				} else {
					log_message(log_module, MSG_ERROR, "No channels available for XML state");
				}
				
				return -2; //We close the connection afterwards
			}
			//statistics, text version
			else if(strstr(client->buffer +pos ,"/monitor/EIT.json ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"EIT Json\n");
				unicast_send_EIT (eit_packets,  client->Socket);
				return -2; //We close the connection afterwards
			}
			else if(strstr(client->buffer +pos ,"/cam/menu.xml ")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"HTTP request for CAM menu display \n");
				unicast_send_cam_menu(client->Socket, cam_p);
				return -2; //We close the connection afterwards
			}
			else if(strstr(client->buffer +pos ,"/cam/action.xml?key=")==(client->buffer +pos))
			{
				log_message( log_module, MSG_DETAIL,"HTTP request for CAM menu action\n");
				pos+=strlen("/cam/action.xml?key=");
				unicast_send_cam_action(client->Socket,client->buffer+pos, cam_p);
				return -2; //We close the connection afterwards
			}
			else if((strstr(client->buffer +pos ,"/index.html")==(client->buffer +pos))||
					(strstr(client->buffer +pos ,"/index.htm")==(client->buffer +pos))||
					(strstr(client->buffer +pos ,"/ ")==(client->buffer +pos)))
			{
				log_message( log_module, MSG_DETAIL,"Index page\n");
				unicast_send_index_page(client->Socket);
				return -2; //We close the connection afterwards
			}
            //Prometheus exporter
            else if(strstr(client->buffer +pos ,"/metrics")==(client->buffer +pos))
            {
                log_message( log_module, MSG_DETAIL,"HTTP request for prometheus data\n");
                
                // Try to get channels from unified storage v2 first, fallback to regular channels
                enhanced_channel_t *enhanced_channels = NULL;
                int num_enhanced_channels = 0;
                mumudvb_channel_t *unified_base_channels = NULL;
                int num_unified_channels = 0;
                
                // Check if we have a unified system with storage v2
                if (get_unified_enhanced_channel_data(&enhanced_channels, &num_enhanced_channels) == 0) {
                    
                    // Convert enhanced channels to base channels for HTTP endpoint
                    if (convert_enhanced_to_base_channels(enhanced_channels, num_enhanced_channels, 
                                                            &unified_base_channels, &num_unified_channels) == 0) {
                        log_message(log_module, MSG_INFO, "Using %d channels from unified storage v2 for prometheus", num_unified_channels);
                        unicast_send_prometheus(num_unified_channels, unified_base_channels, client->Socket, strengthparams);
                        free(unified_base_channels);
                    } else {
                        log_message(log_module, MSG_ERROR, "Failed to convert enhanced channels to base channels for prometheus");
                    }
                } else {
                    log_message(log_module, MSG_ERROR, "No channels available for prometheus");
                }
                
                return -2; //We close the connection afterwards
            }
            //Tuner scan results
            else if(strstr(client->buffer +pos ,"/tuner_scan_results.html")==(client->buffer +pos))
            {
                log_message( log_module, MSG_DETAIL,"HTTP request for tuner scan results\n");
                unicast_send_tuner_scan_results(client->Socket);
                return -2; //We close the connection afterwards
            }
			//Not implemented path --> 404
			else
				err404=1;


			if(err404)
			{
				log_message( log_module, MSG_INFO,"Path not found i.e. 404\n");
				reply = unicast_reply_init();
				if (NULL == reply) {
					log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
					return -2;
				}
				
				// Check if this is a bycard path error and show card list
				if (bycard_error) {
					unicast_reply_write(reply, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml10/DTD/xhtml10strict.dtd\">\r\n");
					unicast_reply_write(reply, "<html lang=\"en\">\r\n");
					unicast_reply_write(reply, "<head>\r\n");
					unicast_reply_write(reply, "<title>Invalid bycard path - MuMuDVB</title>\r\n");
					unicast_reply_write(reply, "<style>");
					unicast_reply_write(reply, "body { font-family: Arial, sans-serif; margin: 20px; }");
					unicast_reply_write(reply, "h1 { color: #d32f2f; }");
					unicast_reply_write(reply, "h3 { color: #1976d2; }");
					unicast_reply_write(reply, "table { border-collapse: collapse; width: 100%%; margin: 10px 0; }");
					unicast_reply_write(reply, "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }");
					unicast_reply_write(reply, "th { background-color: #f2f2f2; }");
					unicast_reply_write(reply, "code { background-color: #f5f5f5; padding: 2px 4px; border-radius: 3px; }");
					unicast_reply_write(reply, "</style>");
					unicast_reply_write(reply, "</head>\r\n");
					unicast_reply_write(reply, "<body>\r\n");
					unicast_reply_write(reply, "<h1>404 - Invalid bycard path</h1>\r\n");
					unicast_reply_write(reply, "<p><strong>Error:</strong> Invalid bycard path, expected /byname/ or /bysid/ after card_id</p>\r\n");
					unicast_reply_write(reply, "<hr />\r\n");
					
					// Generate the cards list
					unicast_generate_cards_list_html(reply, requested_card_id);
					
					unicast_reply_write(reply, "<hr />\r\n");
					unicast_reply_write(reply, "<a href=\"http://mumudvb.net/\">MuMuDVB</a> version %s\r\n", VERSION);
					unicast_reply_write(reply, "</body>\r\n");
					unicast_reply_write(reply, "</html>\r\n");
				} else {
					// Use the standard 404 response for other errors
					unicast_reply_write(reply, HTTP_404_REPLY_HTML, VERSION);
				}
				
				unicast_reply_send(reply, client->Socket, 404, "text/html");
				if (0 != unicast_reply_free(reply)) {
					log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
					return -2;
				}
				return -2; //to delete the client
			}
			//We have found a channel, we add the client
			if(requested_channel)
			{
				if(is_head_request)
				{
					// For HEAD requests, just send the headers without adding to the channel
					log_message( log_module, MSG_DEBUG,"HEAD request for channel %d, sending headers only\n", requested_channel);
					iRet = write(client->Socket, HTTP_OK_REPLY, strlen(HTTP_OK_REPLY));
					if(iRet!=strlen(HTTP_OK_REPLY))
					{
						log_message( log_module, MSG_INFO,"Error when sending the HTTP reply for HEAD request\n");
						return -2;
					}
					return -2; // Close connection after sending headers
				}
				else
				{
					// For GET requests, add the client to the channel for streaming
					// Check if requested_channel is valid before adding client
					if (requested_channel > 0) {
						// For now, we'll use a placeholder channel structure
						// TODO: Implement proper channel management with unified storage v2
						log_message(log_module, MSG_DEBUG, "Adding client for channel %d (unified storage v2)", requested_channel);
						// Note: channel_add_unicast_client needs to be updated to work with unified storage v2
						// For now, we'll just set the channel pointer to NULL
						client->chan_ptr = NULL;
					} else {
						log_message(log_module, MSG_ERROR, "Invalid channel number %d, cannot add client", requested_channel);
						return -2;
					}
				}
			}

		}
		else
		{
			//We don't implement this http method, but if the client is already connected, we keep the connection
			if(client->chan_ptr==NULL)
			{
				log_message( log_module, MSG_INFO,"Unhandled HTTP method : \"%s\", error 501\n",  strtok (client->buffer+pos, " "));
				iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));//iRet is to make the copiler happy we will close the connection anyways
				return -2; //to delete the client
			}
			else
			{
				log_message( log_module, MSG_INFO,"Unhandled HTTP method : \"%s\", error 501 but we keep the client connected\n",  strtok (client->buffer+pos, " "));
				iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));//iRet is to make the copiler happy we will close the connection anyways
				return 0;
			}
		}
		//We don't need the buffer anymore
		free(client->buffer);
		client->buffer=NULL;
		client->bufferpos=0;
		client->buffersize=0;
	}

	return 0;
}


//////////////////
// HTTP Toolbox //
//////////////////


/** @brief Init reply structure
 *
 */
struct unicast_reply* unicast_reply_init()
{
	struct unicast_reply* reply = malloc(sizeof (struct unicast_reply));
	if (NULL == reply)
	{
		log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		return NULL;
	}
	reply->buffer_header = malloc(REPLY_SIZE_STEP * sizeof (char));
	if (NULL == reply->buffer_header)
	{
		free(reply);
		log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		return NULL;
	}
	reply->length_header = REPLY_SIZE_STEP;
	reply->used_header = 0;
	reply->buffer_body = malloc(REPLY_SIZE_STEP * sizeof (char));
	if (NULL == reply->buffer_body)
	{
		free(reply->buffer_header);
		free(reply);
		log_message( log_module, MSG_ERROR,"Problem with malloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
		return NULL;
	}
	reply->length_body = REPLY_SIZE_STEP;
	reply->used_body = 0;
	reply->type = REPLY_BODY;
	return reply;
}

/** @brief Release the reply structure
 *
 */
int unicast_reply_free(struct unicast_reply *reply)
{
	if (NULL == reply)
		return 1;
	if ((NULL == reply->buffer_header)&&(NULL == reply->buffer_body))
		return 1;
	if(reply->buffer_header != NULL)
		free(reply->buffer_header);
	if(reply->buffer_body != NULL)
		free(reply->buffer_body);
	free(reply);
	return 0;
}

/** @brief Write data in a buffer using the same syntax that printf()
 *
 * auto-realloc buffer if needed
 */
int unicast_reply_write(struct unicast_reply *reply, const char* msg, ...)
{
	char **buffer;
	char *temp_buffer;
	int *length;
	int *used;
	buffer=NULL;
	va_list args;
	if (NULL == msg)
		return -1;
	switch(reply->type)
	{
	case REPLY_HEADER:
		buffer=&reply->buffer_header;
		length=&reply->length_header;
		used=&reply->used_header;
		break;
	case REPLY_BODY:
		buffer=&reply->buffer_body;
		length=&reply->length_body;
		used=&reply->used_body;
		break;
	default:
		log_message( log_module, MSG_WARN,"unicast_reply_write with wrong type, please contact\n");
		return -1;
	}
	va_start(args, msg);
	int estimated_len = vsnprintf(NULL, 0, msg, args); /* !! imply gcc -std=c99 */
	//Since vsnprintf put the mess we reinitiate the args
	va_end(args);
	va_start(args, msg);
	// Must add 1 byte more for the terminating zero (not counted)
	while (*length - *used < estimated_len + 1) {
		temp_buffer = realloc(*buffer, *length + REPLY_SIZE_STEP);
		if(temp_buffer == NULL)
		{
			log_message( log_module, MSG_ERROR,"Problem with realloc : %s file : %s line %d\n",strerror(errno),__FILE__,__LINE__);
			va_end(args);
			return -1;
		}
		*buffer=temp_buffer;
		*length += REPLY_SIZE_STEP;
	}
	int real_len = vsnprintf(*buffer+*used, *length - *used, msg, args);
	if (real_len != estimated_len) {
		log_message( log_module, MSG_ERROR,"Error when writing the HTTP reply: estimated=%d, actual=%d, buffer_size=%d, used=%d\n", 
		           estimated_len, real_len, *length, *used);
		va_end(args);
		return -1;
	}
	*used += real_len;
	va_end(args);
	return 0;
}

/** @brief Dump the filled buffer on the socket adding HTTP header informations
 */
int unicast_reply_send(struct unicast_reply *reply, int socket, int code, const char* content_type)
{
	int size=0;
	int temp_size=0;
	//we add the header information
	reply->type = REPLY_HEADER;
	unicast_reply_write(reply, "HTTP/1.0 ");
	switch(code)
	{
	case 200:
		unicast_reply_write(reply, "200 OK\r\n");
		break;
	case 404:
		unicast_reply_write(reply, "404 Not found\r\n");
		break;
	default:
		log_message( log_module, MSG_ERROR,"reply send with bad code please contact\n");
		return 0;
	}
	unicast_reply_write(reply, "Access-Control-Allow-Origin: *\r\n");
	unicast_reply_write(reply, "Server: mumudvb/" VERSION "\r\n");
	unicast_reply_write(reply, "Content-type: %s; charset=utf-8\r\n", content_type);
	unicast_reply_write(reply, "Content-length: %d\r\n", reply->used_body);
	unicast_reply_write(reply, "\r\n"); /* end header */
	//we merge the header and the body
	reply->buffer_header = realloc(reply->buffer_header, reply->used_header+reply->used_body);
	memcpy(&reply->buffer_header[reply->used_header],reply->buffer_body,sizeof(char)*reply->used_body);
	reply->used_header+=reply->used_body;

	//now we write the data
	while (size<reply->used_header){
		temp_size = write(socket, reply->buffer_header+size, reply->used_header-size);
		if (temp_size != -1) {
			size += temp_size;
		} else {
			if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
				return -1;
			}
		}
	}
	return size;
}


//////////////////////
// End HTTP Toolbox //
//////////////////////



/** @brief Send a basic html file containing the list of streamed channels
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 * @param host The server ip address/name (got in the HTTP GET request)
 */
int
unicast_send_streamed_channels_list (int number_of_channels, mumudvb_channel_t *channels, int Socket, char *host)
{

	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	unicast_reply_write(reply, HTTP_CHANNELS_REPLY_START);

	for (int curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
		if (channels[curr_channel].channel_ready>=READY)
		{
			if(host)
				unicast_reply_write(reply, "Channel number %d : %s<br>Unicast links : <a href=\"http://%s/bynumber/%d\">/bynumber/%d</a> | <a href=\"http://%s/byname/%s\">/byname/%s</a><br>Multicast ip : %s:%d<br><br>\r\n",
						curr_channel+1,
						channels[curr_channel].name,
						host, curr_channel+1,
						curr_channel+1,
						host, channels[curr_channel].name,
						channels[curr_channel].name,
						channels[curr_channel].ip4Out,channels[curr_channel].portOut);
			else
				unicast_reply_write(reply, "Channel number %d : \"%s\"<br>Unicast links : /bynumber/%d | /byname/%s<br>Multicast ip : %s:%d<br><br>\r\n",
						curr_channel+1,
						channels[curr_channel].name,
						curr_channel+1,
						channels[curr_channel].name,
						channels[curr_channel].ip4Out,channels[curr_channel].portOut);
		}
	unicast_reply_write(reply, HTTP_CHANNELS_REPLY_END);

	unicast_reply_send(reply, Socket, 200, "text/html");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}

	return 0;
}

/** @brief Send a basic html file containing the list of available channel names
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 */
int
unicast_send_channel_names_list (int number_of_channels, mumudvb_channel_t *channels, int Socket)
{

	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	unicast_reply_write(reply, "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml10/DTD/xhtml10strict.dtd\">\r\n");
	unicast_reply_write(reply, "<html lang=\"en\">\r\n");
	unicast_reply_write(reply, "<head>\r\n");
	unicast_reply_write(reply, "<title>Available Channel Names</title>\r\n");
	unicast_reply_write(reply, "</head>\r\n");
	unicast_reply_write(reply, "<body>\r\n");
	unicast_reply_write(reply, "   <h1>Available Channel Names</h1>\r\n");
	unicast_reply_write(reply, "<hr />\r\n");
	unicast_reply_write(reply, "This is the list of available channel names for streaming.\r\n");
	unicast_reply_write(reply, "<hr />\r\n");

	for (int curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
		if (channels[curr_channel].channel_ready>=READY)
		{
			unicast_reply_write(reply, "Channel name: <a href=\"/byname/%s\">%s</a><br>\r\n",
						channels[curr_channel].name,
						channels[curr_channel].name);
		}
	
	unicast_reply_write(reply, "<hr />\r\n");
	unicast_reply_write(reply, "See <a href=\"http://mumudvb.net/\">MuMuDVB</a> website for more details.\r\n");
	unicast_reply_write(reply, "</body>\r\n");
	unicast_reply_write(reply, "</html>\r\n");
	unicast_reply_write(reply, "\r\n");

	unicast_reply_send(reply, Socket, 200, "text/html");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}

	return 0;
}

/** @brief Generate HTML for available cards list
 *
 * @param reply the unicast reply structure to write to
 * @param requested_card_id the card ID that was requested (if any)
 * @return 0 on success, -1 on error
 */
int
unicast_generate_cards_list_html(struct unicast_reply* reply, int requested_card_id)
{
	if (!reply) {
		return -1;
	}

	// Check if we have a unified system
	if (!global_unified_system || global_unified_system->num_cards <= 0) {
		unicast_reply_write(reply, "<p><strong>No cards available.</strong> The unified system is not initialized or no cards are configured.</p>");
		return 0;
	}

	unicast_reply_write(reply, "<h3>Available Cards:</h3>");
	unicast_reply_write(reply, "<table border=\"1\" cellpadding=\"5\" cellspacing=\"0\" style=\"border-collapse: collapse;\">");
	unicast_reply_write(reply, "<tr><th>Card ID</th><th>Device Path</th><th>Frontend Name</th><th>Status</th><th>Current Frequency</th><th>In Use</th><th>Usage Examples</th></tr>");

	for (int card_idx = 0; card_idx < global_unified_system->num_cards; card_idx++) {
		unified_card_t *card = &global_unified_system->cards[card_idx];
		const char *status_class = (card_idx == requested_card_id) ? "style=\"background-color: #ffeb3b;\"" : "";
		
		unicast_reply_write(reply, "<tr %s>", status_class);
		unicast_reply_write(reply, "<td><strong>%d</strong></td>", card->card_id);
		unicast_reply_write(reply, "<td>/dev/dvb/adapter%d/</td>", card->card_id);
		unicast_reply_write(reply, "<td>%s</td>", 
		                   card->tune_params ? card->tune_params->fe_name : "Unknown");
		unicast_reply_write(reply, "<td>%s</td>", 
		                   card->tune_params && card->tune_params->card_tuned ? "Tuned" : "Not Tuned");
		unicast_reply_write(reply, "<td>%.1f MHz</td>", card->current_freq / 1000000.0);
		unicast_reply_write(reply, "<td>%s</td>", card->in_use ? "Yes" : "No");
		unicast_reply_write(reply, "<td><small>");
		unicast_reply_write(reply, "/bycard/%d/byname/&lt;channel_name&gt;<br/>", card->card_id);
		unicast_reply_write(reply, "/bycard/%d/bysid/&lt;service_id&gt;</small></td>", card->card_id);
		unicast_reply_write(reply, "</tr>");
	}

	unicast_reply_write(reply, "</table>");
	
	if (requested_card_id >= 0) {
		unicast_reply_write(reply, "<p><strong>Note:</strong> The requested card ID %d is highlighted above. ", requested_card_id);
		unicast_reply_write(reply, "Make sure to use the correct path format: <code>/bycard/&lt;card_id&gt;/byname/&lt;channel_name&gt;</code> or <code>/bycard/&lt;card_id&gt;/bysid/&lt;service_id&gt;</code></p>");
	}
	
	unicast_reply_write(reply, "<p><strong>Usage:</strong> To access channels on a specific card, use one of these URL patterns:</p>");
	unicast_reply_write(reply, "<ul>");
	unicast_reply_write(reply, "<li><code>/bycard/&lt;card_id&gt;/byname/&lt;channel_name&gt;</code> - Access channel by name on specific card</li>");
	unicast_reply_write(reply, "<li><code>/bycard/&lt;card_id&gt;/bysid/&lt;service_id&gt;</code> - Access channel by service ID on specific card</li>");
	unicast_reply_write(reply, "<li><code>/byname/&lt;channel_name&gt;</code> - Access channel by name (any available card)</li>");
	unicast_reply_write(reply, "<li><code>/bysid/&lt;service_id&gt;</code> - Access channel by service ID (any available card)</li>");
	unicast_reply_write(reply, "</ul>");

	return 0;
}


/** @brief Send a basic text file containig the playlist
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 * @param perport says if the channel have to be given by the url /bysid or by their port
 */
int
unicast_send_play_list_unicast (int number_of_channels, mumudvb_channel_t *channels, int Socket, int unicast_portOut, int perport, unicast_parameters_t *unicast_vars)
{
	int curr_channel,iRet;
	struct sockaddr_storage tempSocketAddr;
	socklen_t l = sizeof(struct sockaddr_storage);

	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	/* We get the ip address on which the client is connected */
	iRet = getsockname(Socket, (struct sockaddr *)&tempSocketAddr, &l);
	if (iRet < 0)
	{
		log_message( log_module,  MSG_ERROR,"getsockname failed : %s while making HTTP reply", strerror(errno));
		if (0 != unicast_reply_free(reply))
			log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply");
		return -1;
	}
	//we write the playlist
	unicast_reply_write(reply, "#EXTM3U\r\n");

	//"#EXTINF:0,title\r\nURL"
	for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
		if (channels[curr_channel].channel_ready>=READY
		    && (channels[curr_channel].has_traffic == 1 || unicast_vars->playlist_ignore_dead == 0)
		    && (channels[curr_channel].ratio_scrambled < unicast_vars->playlist_ignore_scrambled_ratio || unicast_vars->playlist_ignore_scrambled_ratio == 0)
		)
		{
			char addr_buf[IPV6_CHAR_LEN] = { 0, };
			char http_buf[IPV6_CHAR_LEN] = { 0, };

			getnameinfo((struct sockaddr *)&tempSocketAddr, sizeof(struct sockaddr_storage), addr_buf, sizeof(addr_buf), NULL, 0, NI_NUMERICHOST | NI_NUMERICSERV);
			/* IPv6 requires address in []'s */
			snprintf(http_buf, IPV6_CHAR_LEN, (tempSocketAddr.ss_family == AF_INET6) ? "[%s]" : "%s", addr_buf);

			if(!perport)
			{
				unicast_reply_write(reply, "#EXTINF:0,%s\r\nhttp://%s:%d/bysid/%d\r\n",
						channels[curr_channel].name,
						http_buf,
						unicast_portOut ,
						channels[curr_channel].service_id);
			}
			else if(channels[curr_channel].unicast_port)
			{
				unicast_reply_write(reply, "#EXTINF:0,%s\r\nhttp://%s:%d/\r\n",
						channels[curr_channel].name,
						http_buf,
						channels[curr_channel].unicast_port);
			}
		}

	unicast_reply_send(reply, Socket, 200, "audio/x-mpegurl");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}

	return 0;
}

/** @brief Send a basic index.html
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 * @param perport says if the channel have to be given by the url /bysid or by their port
 */
int
unicast_send_index_page (int Socket)
{
	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	unicast_reply_write(reply, HTTP_INDEX_REPLY_START);


	unicast_reply_write(reply, "<br>Channels by number : /bynumber/[channel number]<br><br>\r\n");
	unicast_reply_write(reply, "<br>Channels by name : /byname/[channel name]<br><br>\r\n");
	unicast_reply_write(reply, "<br>Note: /bysid/ links are not shown in the channel list due to potential SID overlap in unified mode<br><br>\r\n");


	unicast_reply_write(reply, "<br>  <a href=\"/channels_list.html\">Channels list</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/byname/\">Available channel names</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/playlist.m3u\">Playlist (m3u)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/playlist_port.m3u\">Playlist by port(m3u)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/playlist_multicast.m3u\">Playlist multicast (m3u)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/playlist_multicast_vlc.m3u\">Playlist multicast for VLC(m3u)</a><br><br>\r\n");

	unicast_reply_write(reply, "<br>  <a href=\"/channels_list.json\">Channels list (json)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/monitor/signal_power.json\">Signal strength (json)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/monitor/channels_traffic.json\">Channels traffic (json)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/monitor/state.xml\">Server state : channel list, pids, traffic (XML)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/monitor/state.json\">Server state : channel list, pids, traffic (json)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/monitor/EIT.json\">Contents of the EIT tables (json)</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/tuner_scan_results.html\">Tuner Scan Results</a><br><br>\r\n");
	unicast_reply_write(reply, "<br>  <a href=\"/cam/menu.xml\">CAM menu</a><br><br>\r\n");
	unicast_reply_write(reply, "<br> make an action on the cam menu : /cam/action.xml?key=<br><br>\r\n");



	unicast_reply_write(reply, HTTP_INDEX_REPLY_END);

	unicast_reply_send(reply, Socket, 200, "text/html");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}

	return 0;
}




/** @brief Send a basic text file containig the playlist
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 */
int
unicast_send_play_list_multicast (int number_of_channels, mumudvb_channel_t *channels, int Socket, int vlc, unicast_parameters_t *unicast_vars)
{
	int curr_channel;
	char urlheader[4];
	char vlcchar[2];


	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message( log_module, MSG_INFO,"Error when creating the HTTP reply\n");
		return -1;
	}

	unicast_reply_write(reply, "#EXTM3U\r\n");

	if(vlc)
		strcpy(vlcchar,"@");
	else
		vlcchar[0]='\0';


	//"#EXTINF:0,title\r\nURL"
	for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
		if (channels[curr_channel].channel_ready>=READY && (channels[curr_channel].has_traffic == 1 || unicast_vars->playlist_ignore_dead == 0))
		{
			if(channels[curr_channel].rtp)
				strcpy(urlheader,"rtp");
			else
				strcpy(urlheader,"udp");

			unicast_reply_write(reply, "#EXTINF:0,%s\r\n%s://%s%s:%d\r\n",
					channels[curr_channel].name,
					urlheader,
					vlcchar,
					channels[curr_channel].ip4Out,
					channels[curr_channel].portOut);
		}

	unicast_reply_send(reply, Socket, 200, "audio/x-mpegurl");

	if (0 != unicast_reply_free(reply)) {
		log_message( log_module, MSG_INFO,"Error when releasing the HTTP reply after sendinf it\n");
		return -1;
	}

	return 0;
}

/** @brief Trims name of a channel to remove leading and trailing spaces, and replaces all spaces by '-' character.
 * Note that the string will be modified so do a copy prior to running this function if modifications to the original shall be prevented.
 *
 * @param str Channel name to process
 */
void process_channel_name(char *str) {
    int i;
    int begin = 0;
    int end = strlen(str) - 1;

    while (isspace(str[begin]))
        begin++;
    while ((end >= begin) && isspace(str[end]))
        end--;

    // shift all characters back to the start of the string array
    for (i = begin; i <= end; i++) {
        if (isspace(str[i]))
            str[i - begin] = '-'; // replace spaces by '-'
        else
            str[i - begin] = str[i];
    }

    str[i - begin] = '\0';
}

/** @brief Send card utilization status as JSON
 *
 * @param Socket the socket on which the information has to be sent
 */
int unicast_send_card_utilization_status(int Socket)
{
	char json_buffer[8192];
	int json_length = generate_card_utilization_json(json_buffer, sizeof(json_buffer));
	
	if (json_length <= 0) {
		log_message(log_module, MSG_ERROR, "Failed to generate card utilization JSON");
		return -1;
	}
	
	// Send HTTP headers
	char http_headers[512];
	int header_length = snprintf(http_headers, sizeof(http_headers),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %d\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Connection: close\r\n"
		"\r\n", json_length);
	
	// Send headers
	if (write(Socket, http_headers, header_length) != header_length) {
		log_message(log_module, MSG_ERROR, "Failed to send card utilization HTTP headers");
		return -1;
	}
	
	// Send JSON data
	if (write(Socket, json_buffer, json_length) != json_length) {
		log_message(log_module, MSG_ERROR, "Failed to send card utilization JSON data");
		return -1;
	}
	
	return 0;
}

/** @brief Send tuner scan results as HTML table
 * @param Socket the socket on which the information have to be sent
 */
int unicast_send_tuner_scan_results(int Socket)
{
	struct unicast_reply* reply = unicast_reply_init();
	if (NULL == reply) {
		log_message(log_module, MSG_INFO, "Error when creating the HTTP reply\n");
		return -1;
	}

	// Get scan results from parallel card manager
	extern int get_parallel_scan_results_count(void);
	extern int get_parallel_scan_results(card_frequency_result_t *results, int max_results);
	extern int is_initial_scan_complete(void);
	
	// Check if we're in single card/frequency mode
	extern unified_channel_system_t unified_system;
	int is_single_card_mode = (unified_system.num_cards == 1 && unified_system.num_frequencies == 1);
	
	int max_results = get_parallel_scan_results_count();
	int scan_complete = is_initial_scan_complete();
	
	// Get refresh delay from configuration (default: 300 seconds)
	extern unicast_parameters_t *global_unicast_params;
	int refresh_delay = 300; // Default fallback
	if (global_unicast_params) {
		refresh_delay = global_unicast_params->scan_results_refresh_delay;
	}
	
	// Start HTML response
	unicast_reply_write(reply, "<html><head><title>MuMuDVB - Tuner Scan Results</title>");
	unicast_reply_write(reply, "<meta http-equiv=\"refresh\" content=\"%d\">", refresh_delay); // Auto-refresh with configurable delay
	unicast_reply_write(reply, "<style>");
	unicast_reply_write(reply, "table { border-collapse: collapse; width: 100%; }");
	unicast_reply_write(reply, "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }");
	unicast_reply_write(reply, "th { background-color: #f2f2f2; }");
	unicast_reply_write(reply, ".success { background-color: #d4edda; }");
	unicast_reply_write(reply, ".failed { background-color: #f8d7da; }");
	unicast_reply_write(reply, ".scanning { background-color: #fff3cd; }");
	unicast_reply_write(reply, ".progress { background-color: #e7f3ff; padding: 10px; margin: 10px 0; border-radius: 5px; }");
	unicast_reply_write(reply, "</style></head><body>");
	
	unicast_reply_write(reply, "<h1>MuMuDVB Tuner Scan Results</h1>");
	
	// Check if we're in single card/frequency mode
	if (is_single_card_mode) {
		unicast_reply_write(reply, "<div class=\"progress\" style=\"background-color: #e7f3ff;\">");
		unicast_reply_write(reply, "<h3>ℹ️ Single Card/Frequency Mode</h3>");
		unicast_reply_write(reply, "<p>System is operating in single card/frequency mode. Tuner scan results are not applicable in this configuration.</p>");
		unicast_reply_write(reply, "<p><strong>Current configuration:</strong> %d card, %d frequency</p>", unified_system.num_cards, unified_system.num_frequencies);
		unicast_reply_write(reply, "<p>For tuner scan results, configure multiple cards or frequencies using the unified system.</p>");
		unicast_reply_write(reply, "</div>");
		unicast_reply_write(reply, "<p><a href=\"/\">Back to main page</a></p>");
		unicast_reply_write(reply, "</body></html>");
		unicast_reply_send(reply, Socket, 200, "text/html");
		unicast_reply_free(reply);
		return 0;
	}
	
	// Show scanning status for parallel mode
	if (!scan_complete) {
		unicast_reply_write(reply, "<div class=\"progress\">");
		unicast_reply_write(reply, "<h3>🔄 Scanning in Progress...</h3>");
		unicast_reply_write(reply, "<p>Results are being updated in real-time. This page will refresh automatically every %d seconds.</p>", refresh_delay);
		if (max_results > 0) {
			unicast_reply_write(reply, "<p><strong>Partial results available:</strong> %d scan results completed so far</p>", max_results);
		} else {
			unicast_reply_write(reply, "<p><strong>Status:</strong> Starting scan, no results yet...</p>");
		}
		unicast_reply_write(reply, "</div>");
	} else {
		unicast_reply_write(reply, "<div class=\"progress\" style=\"background-color: #d4edda;\">");
		unicast_reply_write(reply, "<h3>✅ Scan Complete</h3>");
		unicast_reply_write(reply, "<p>All cards have been tested against all frequencies.</p>");
		unicast_reply_write(reply, "</div>");
	}
	
	if (max_results <= 0) {
		unicast_reply_write(reply, "<p>No scan results available yet. Please wait for scanning to complete.</p>");
		unicast_reply_write(reply, "</body></html>");
		unicast_reply_send(reply, Socket, 200, "text/html");
		unicast_reply_free(reply);
		return 0;
	}
	
	card_frequency_result_t *results = malloc(max_results * sizeof(card_frequency_result_t));
	if (!results) {
		unicast_reply_write(reply, "<p>Error: Unable to allocate memory for scan results.</p>");
		unicast_reply_write(reply, "</body></html>");
		unicast_reply_send(reply, Socket, 200, "text/html");
		unicast_reply_free(reply);
		return -1;
	}
	
	int actual_results = get_parallel_scan_results(results, max_results);
	
	
	// Calculate summary statistics
	int total_channels = 0;
	int successful_scans = 0;
	for (int i = 0; i < actual_results; i++) {
		total_channels += results[i].channel_count;
		if (results[i].status == 1) successful_scans++;
	}
	
	// Show summary with progress information
	if (!scan_complete) {
		unicast_reply_write(reply, "<p><strong>Progress:</strong> %d results completed | Successful: %d | Channels found: %d | <em>Scanning continues...</em></p>", 
		                    actual_results, successful_scans, total_channels);
	} else {
		unicast_reply_write(reply, "<p><strong>Final Results:</strong> %d total results | Successful scans: %d | Total channels found: %d</p>", 
		                    actual_results, successful_scans, total_channels);
	}
	
	// Sort results based on query parameter
	// For now, use default sorting since we don't have access to the client buffer here
	// TODO: Pass query parameters as function parameter or use a different approach
	char *sort_by = "card"; // default sort
	
	// Add sorting options with current sort indicator
	unicast_reply_write(reply, "<p>Sort by: ");
	unicast_reply_write(reply, "<a href=\"/tuner_scan_results.html?sort=card\">%sCard%s</a> | ", 
	                    (strcmp(sort_by, "card") == 0) ? "<strong>" : "", 
	                    (strcmp(sort_by, "card") == 0) ? "</strong>" : "");
	unicast_reply_write(reply, "<a href=\"/tuner_scan_results.html?sort=frequency\">%sFrequency%s</a> | ", 
	                    (strcmp(sort_by, "frequency") == 0) ? "<strong>" : "", 
	                    (strcmp(sort_by, "frequency") == 0) ? "</strong>" : "");
	unicast_reply_write(reply, "<a href=\"/tuner_scan_results.html?sort=channels\">%sChannels%s</a> | ", 
	                    (strcmp(sort_by, "channels") == 0) ? "<strong>" : "", 
	                    (strcmp(sort_by, "channels") == 0) ? "</strong>" : "");
	unicast_reply_write(reply, "<a href=\"/tuner_scan_results.html?sort=status\">%sStatus%s</a></p>", 
	                    (strcmp(sort_by, "status") == 0) ? "<strong>" : "", 
	                    (strcmp(sort_by, "status") == 0) ? "</strong>" : "");
	
	// Sort the results array
	if (strcmp(sort_by, "card") == 0) {
		// Sort by card ID
		for (int i = 0; i < actual_results - 1; i++) {
			for (int j = i + 1; j < actual_results; j++) {
				if (results[i].card_id > results[j].card_id) {
					card_frequency_result_t temp = results[i];
					results[i] = results[j];
					results[j] = temp;
				}
			}
		}
	} else if (strcmp(sort_by, "frequency") == 0) {
		// Sort by frequency
		for (int i = 0; i < actual_results - 1; i++) {
			for (int j = i + 1; j < actual_results; j++) {
				if (results[i].frequency > results[j].frequency) {
					card_frequency_result_t temp = results[i];
					results[i] = results[j];
					results[j] = temp;
				}
			}
		}
	} else if (strcmp(sort_by, "channels") == 0) {
		// Sort by channel count (descending)
		for (int i = 0; i < actual_results - 1; i++) {
			for (int j = i + 1; j < actual_results; j++) {
				if (results[i].channel_count < results[j].channel_count) {
					card_frequency_result_t temp = results[i];
					results[i] = results[j];
					results[j] = temp;
				}
			}
		}
	} else if (strcmp(sort_by, "status") == 0) {
		// Sort by status (successful first)
		for (int i = 0; i < actual_results - 1; i++) {
			for (int j = i + 1; j < actual_results; j++) {
				if (results[i].status < results[j].status) {
					card_frequency_result_t temp = results[i];
					results[i] = results[j];
					results[j] = temp;
				}
			}
		}
	}
	
	unicast_reply_write(reply, "<table>");
	unicast_reply_write(reply, "<tr><th>Card</th><th>Frequency (MHz)</th><th>FE Status</th><th>Signal</th><th>SNR</th><th>Channels</th><th>Lock Time</th><th>Quality</th></tr>");
	
	for (int i = 0; i < actual_results; i++) {
		const char *status_class;
		if (results[i].status == 1) {
			status_class = "success";
		} else if (results[i].status == 0) {
			status_class = "failed";
		} else {
			status_class = "scanning"; // For any other status
		}
		
		// Format FE_STATUS flags as readable text
		char fe_status_text[256] = "";
		if (results[i].fe_status_flags & 0x01) strcat(fe_status_text, "SIGNAL ");
		if (results[i].fe_status_flags & 0x02) strcat(fe_status_text, "CARRIER ");
		if (results[i].fe_status_flags & 0x04) strcat(fe_status_text, "VITERBI ");
		if (results[i].fe_status_flags & 0x08) strcat(fe_status_text, "SYNC ");
		if (results[i].fe_status_flags & 0x10) strcat(fe_status_text, "LOCK ");
		if (strlen(fe_status_text) == 0) strcat(fe_status_text, "NONE");
		
		int quality_score = results[i].signal_strength + (results[i].snr / 10);
		
		unicast_reply_write(reply, "<tr class=\"%s\">", status_class);
		unicast_reply_write(reply, "<td>%d</td>", results[i].card_id);
		unicast_reply_write(reply, "<td>%.1f</td>", results[i].frequency / 1000000.0);
		unicast_reply_write(reply, "<td>%s</td>", fe_status_text);
		unicast_reply_write(reply, "<td>%d</td>", results[i].signal_strength);
		unicast_reply_write(reply, "<td>%d</td>", results[i].snr);
		// Display channel count with highlighting and debugging info
		if (results[i].channel_count > 0) {
			unicast_reply_write(reply, "<td style=\"background-color: #d1ecf1; font-weight: bold; color: #0c5460;\">%d</td>", results[i].channel_count);
		} else {
			unicast_reply_write(reply, "<td style=\"color: #6c757d;\">%d</td>", results[i].channel_count);
		}
		unicast_reply_write(reply, "<td>%d ms</td>", results[i].lock_time_ms);
		unicast_reply_write(reply, "<td>%d</td>", quality_score);
		unicast_reply_write(reply, "</tr>");
	}
	
	unicast_reply_write(reply, "</table>");
	// Add debug information section (only show if there are issues)
	if (total_channels == 0 && actual_results > 0) {
		unicast_reply_write(reply, "<div style=\"background-color: #fff3cd; border: 1px solid #ffeaa7; padding: 10px; margin: 10px 0; border-radius: 5px;\">");
		unicast_reply_write(reply, "<h4>🔍 Debug Information</h4>");
		unicast_reply_write(reply, "<p><strong>Issue:</strong> No channels found in scan results. This might indicate:</p>");
		unicast_reply_write(reply, "<ul>");
		unicast_reply_write(reply, "<li>Channels are being discovered but not properly stored in scan results</li>");
		unicast_reply_write(reply, "<li>Channel discovery is happening after scan results are generated</li>");
		unicast_reply_write(reply, "<li>There's a timing issue between parallel scanning and channel storage</li>");
		unicast_reply_write(reply, "</ul>");
		unicast_reply_write(reply, "<p><strong>Debug data:</strong> %d results, %d successful scans, %d total channels</p>", 
		                   actual_results, successful_scans, total_channels);
		unicast_reply_write(reply, "</div>");
	}
	
	unicast_reply_write(reply, "<p><a href=\"/\">Back to main page</a></p>");
	unicast_reply_write(reply, "</body></html>");
	
	unicast_reply_send(reply, Socket, 200, "text/html");
	
	free(results);
	unicast_reply_free(reply);
	return 0;
}

