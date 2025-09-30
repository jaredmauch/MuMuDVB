# what flags you want to pass to the C compiler & linker
AM_CFLAGS = -Wall -Wextra -pthread
AM_LDFLAGS =

bin_PROGRAMS = mumudvb
mumudvb_SOURCES = autoconf.c crc32.c dvb.h log.c log.h multicast.c mumudvb.h network.h rewrite.h \
		  rtp.h sap.h ts.h tune.h unicast_http.h autoconf.h dvb.c errors.h \
		  mumudvb.c mumudvb_mon.c mumudvb_mon.h mumudvb_common.c network.c rewrite_pmt.c rewrite_pat.c rewrite.c rewrite_sdt.c rewrite_eit.c \
		  rtp.c sap.c ts.c t2mi.c tune.c unicast_http.c unicast_queue.c unicast_EIT.c autoconf_sdt.c autoconf_atsc.c \
		  autoconf_pmt.c autoconf_nit.c unicast_clients.c unicast_monit.c mumudvb_channels.c \
		  autoconf_pat.c autoconf_cat.c hls.c unified_channels.c unified_channel_storage_v2.c \
		  unified_storage_adapter.c parallel_card_manager.c http_thread.c \
		  bitrate_monitor.c event_timing.c thread_manager.c main_thread_poll.c \
		  thread_shutdown_monitor.c thread_loop_helpers.c

mumudvb_LDADD = -lm -lpthread

# Test program for storage migration (disabled for now)
# noinst_PROGRAMS = test_storage_migration
# test_storage_migration_SOURCES = test_storage_migration.c unified_channel_storage_v2.c unified_storage_adapter.c
# test_storage_migration_LDADD = -lm -lpthread

if BUILD_CAMSUPPORT
mumudvb_SOURCES += $(SOURCES_camsupport)
endif

SOURCES_camsupport = \
        cam.c \
	cam.h \
        $(NULL)
        
if BUILD_SCAMSUPPORT
mumudvb_SOURCES += $(SOURCES_scamsupport)
endif

SOURCES_scamsupport = \
        scam_capmt.c \
	scam_capmt.h \
        scam_common.c \
	scam_common.h \
        scam_getcw.c \
	scam_getcw.h \
        scam_decsa.c \
	scam_decsa.h \
        scam_send.c \
	scam_send.h \
        $(NULL)

