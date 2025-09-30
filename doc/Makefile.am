# Documentation and manpage generation
ASCIIDOCFLAGS   = -a toc -a numbered
htmldocdir	= html
html_DOC	= $(htmldocdir)/README.html $(htmldocdir)/README_CONF.html $(htmldocdir)/QUICKSTART.html $(htmldocdir)/WEBSERVICES.html $(htmldocdir)/TRANSCODE_EXTERNAL.html $(htmldocdir)/FAQ.html $(htmldocdir)/index.html

# Manpage configuration
man_MANS = mumudvb.8
EXTRA_DIST = mumudvb.8.in

# Clean up generated files
DISTCLEANFILES = $(html_DOC) mumudvb.8

doc: $(html_DOC)

$(htmldocdir)/%.html: %.asciidoc
	install -d $(htmldocdir)
	-asciidoc $(ASCIIDOCFLAGS) -o $@ $^
