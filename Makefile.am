AUTOMAKE_OPTIONS = foreign
SUBDIRS = src doc

# Install manpages
man_MANS = doc/mumudvb.8

doc:
	cd doc && $(MAKE) $(AM_MAKEFLAGS) doc

.PHONY: $(SUBDIRS) pedantic

# Remove core dump files during build
clean-local:
	rm -f core.*

# Pedantic build target - adds -pedantic flag to gcc along with -Wall
pedantic:
	$(MAKE) $(AM_MAKEFLAGS) CFLAGS="$(CFLAGS) -Wall -pedantic" all

