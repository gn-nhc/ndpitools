# Tag Image File Format (TIFF) Software
#
# Copyright (C) 2004, Andrey Kiselev <dron@ak4719.spb.edu>
#
# Permission to use, copy, modify, distribute, and sell this software and 
# its documentation for any purpose is hereby granted without fee, provided
# that (i) the above copyright notices and this permission notice appear in
# all copies of the software and related documentation, and (ii) the names of
# Sam Leffler and Silicon Graphics may not be used in any advertising or
# publicity relating to the software without the specific, prior written
# permission of Sam Leffler and Silicon Graphics.
# 
# THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
# EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
# WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
# 
# IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
# ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
# OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
# WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
# LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
# OF THIS SOFTWARE.

# Process this file with automake to produce Makefile.in.

LIBPORT = $(top_builddir)/port/libport.la
LIBTIFF = $(top_builddir)/libtiff/.libs/libtiff.a
LIBJPEG = $(top_builddir)/../lib/libjpeg.a

EXTRA_DIST = \
	CMakeLists.txt

bin_PROGRAMS = \
	ndpi2tiff \
	ndpisplit \
	ndpisplit-s \
	ndpisplit-m \
	ndpisplit-mJ \
	ndpisplit-s-m \
	ndpisplit-s-mJ

if HAVE_RPATH
AM_LDFLAGS = $(LIBDIR)
endif

ndpi2tiff_SOURCES = ndpi2tiff.c
ndpi2tiff_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)
  
ndpisplit_SOURCES = ndpisplit.c
ndpisplit_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)
  
ndpisplit_s_SOURCES = ndpisplit-s.c
ndpisplit_s_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)
  
ndpisplit_m_SOURCES = ndpisplit-m.c
ndpisplit_m_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)
  
ndpisplit_mJ_SOURCES = ndpisplit-mJ.c
ndpisplit_mJ_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)
  
ndpisplit_s_m_SOURCES = ndpisplit-s-m.c
ndpisplit_s_m_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)
  
ndpisplit_s_mJ_SOURCES = ndpisplit-s-mJ.c
ndpisplit_s_mJ_LDADD = $(LIBTIFF) $(LIBPORT) $(LIBJPEG)

AM_CPPFLAGS = -I$(top_srcdir)/libtiff -I$(top_srcdir)/port

echo:
	(echo $(CFLAGS))
	(echo $(GL_CFLAGS))
	(echo $(GLU_CFLAGS))
	(echo $(GLUT_CFLAGS))
