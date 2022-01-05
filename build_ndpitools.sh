#!/bin/sh

USE_LIBJPEGTURBO=yes

LIBJPEGTURBOVERSION=2.1.2
JPEGVERSION=9d
TIFFVERSION=4.3.0

BINARIES="ndpi2tiff ndpisplit ndpisplit-s ndpisplit-m ndpisplit-mJ ndpisplit-s-m ndpisplit-s-mJ"

BASEDIR=$PWD

if [ x${USE_LIBJPEGTURBO} = xyes ] ; then
  ln -s libjpeg-turbo-${LIBJPEGTURBOVERSION} jpeg
  cd jpeg
  cmake -G"Unix Makefiles" -DCMAKE_INSTALL_PREFIX:PATH=$BASEDIR -DENABLE_SHARED=0 .
  make
  make install
  cd ..
fi

if [ x${USE_LIBJPEGTURBO} != xyes -o ! -e lib/libjpeg.a ] ; then
  rm -f jpeg
  ln -s jpeg-${JPEGVERSION} jpeg
  cd jpeg
  ./configure --prefix=$BASEDIR
  make
  make install
  cd ..
fi

cd $BASEDIR

JPEGDIR=$BASEDIR/jpeg
cd tiff-${TIFFVERSION}
  sh -c "LDFLAGS=\"-L$BASEDIR/lib $LDSTATICOPTION\" \
    CFLAGS=\"-O2\" ./configure \
    --with-jpeg-include-dir=$BASEDIR/include \
    --with-jpeg-lib-dir=$BASEDIR/lib --prefix=$BASEDIR \
    --enable-shared=no"
  cd port ; make ; cd ..
  cd libtiff ; make ; cd ..
  cd tools
  make > make.out
  rm -f cmds-remake-static
  grep '^libtool:' make.out | \
    sed -e "s/^libtool: link://;s|$BASEDIR/lib/libjpeg\.[a-z][a-z.]*||" \
    > cmds-remake-static
  cat cmds-remake-static
  sh cmds-remake-static
  make install

  BINDIR=$BASEDIR/bin
  cd $BINDIR
  STRIP="strip"
  $STRIP *
  cd $BASEDIR
