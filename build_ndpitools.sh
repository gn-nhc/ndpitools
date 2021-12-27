#!/bin/sh

USE_LIBJPEGTURBO=yes

LIBJPEGTURBOVERSION=1.4.1
JPEGVERSION=9a
TIFFVERSION=4.0.6

BINARIES="ndpi2tiff ndpisplit ndpisplit-s ndpisplit-m ndpisplit-mJ ndpisplit-s-m ndpisplit-s-mJ"

BASEDIR=$PWD

if [ x${USE_LIBJPEGTURBO} = xyes ] ; then
  ln -s libjpeg-turbo-${LIBJPEGTURBOVERSION} jpeg
  cd jpeg
  autoreconf -fiv
  cd ..
else
  ln -s jpeg-${JPEGVERSION} jpeg
fi
cd jpeg
  ./configure --prefix=$BASEDIR
  make
  make install
cd $BASEDIR

JPEGDIR=$BASEDIR/jpeg
cd tiff-${TIFFVERSION}
  sh -c "LDFLAGS=\"-L$BASEDIR/lib $LDSTATICOPTION\" \
    CFLAGS=\"-O2\" ./configure \
    --with-jpeg-include-dir=$JPEGDIR \
    --with-jpeg-lib-dir=$JPEGDIR/.libs --prefix=$BASEDIR"
  cd port ; make ; cd ..
  cd libtiff ; make ; cd ..
  cd tools
  make > make.out
  rm -f cmds-remake-static
  grep '^libtool:' make.out | \
    sed -e "s/^libtool: link://;s|$JPEGDIR/\.libs/libjpeg\.[a-z][a-z.]*||" \
    > cmds-remake-static
  cat cmds-remake-static
  sh cmds-remake-static
  make install

  BINDIR=$BASEDIR/bin
  cd $BINDIR
  STRIP="strip"
  $STRIP *
  cd $BASEDIR
