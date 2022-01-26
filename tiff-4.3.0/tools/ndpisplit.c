/* ndpisplit
 v. 1.5-3
 Copyright (c) 2011-2021 Christophe Deroulers
 Distributed under the GNU General Public License v3 -- contact the 
 author for commercial use
 Based on libtiff's tiffsplit.c and tiffcp.c -- copyright notice of 
 tiffsplit.c below */

/*
 * Copyright (c) 1992-1997 Sam Leffler
 * Copyright (c) 1992-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

#include "tif_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdarg.h>

#include "tiffio.h"

#include "jpeglib.h"

#define COMPRESSION_JPEG_IN_JPEG_FILE ((uint16_t) -2)
#define ORDINARY_JPEG_MAX_DIMENSION  65500L

#define TIFF_INT32_FORMAT "%"PRId32
#define TIFF_UINT32_FORMAT "%"PRIu32
#define TIFF_UINT64_FORMAT "%"PRIu64

typedef struct {
	float magnification;
	uint32_t width, length;
} MagnificationDescription;

typedef struct {
	int isempty;
	uint32_t map_xmin, map_ymin, map_xmax, map_ymax;
	/*uint32_t image_xmin, image_ymin, image_xmax, image_ymax;*/
} ScannedZoneBox;

typedef struct {
	double relxmin, relymin, relwidth, rellength;
	uint32_t xmin, ymin, width, length;
	unsigned numberofmagnificationstoextract;
	float * magnificationstoextract;
	unsigned numberofzoffsetstoextract;
	int32_t * zoffsetstoextract;
	char * label;
} BoxToExtract;

#ifndef HAVE_GETOPT
extern int getopt(int, char**, char*);
#endif

#define	CopyField(tag, v) \
    if (TIFFGetField(in, tag, &v)) TIFFSetField(out, tag, v)
#define	CopyField2(tag, v1, v2) \
    if (TIFFGetField(in, tag, &v1, &v2)) TIFFSetField(out, tag, v1, v2)
#define	CopyField3(tag, v1, v2, v3) \
    if (TIFFGetField(in, tag, &v1, &v2, &v3)) TIFFSetField(out, tag, v1, v2, v3)

#define MIN(a,b) \
    if (b < a) a = b
#define MAX(a,b) \
    if (b > a) a = b

static	const char TIFF_SUFFIX[] = ".tif";
static	const char JPEG_SUFFIX[] = ".jpg";
static	float * magnificationstoextract = NULL;
static	unsigned numberofmagnificationstoextract = (unsigned) -1;
static	int32_t * zoffsetstoextract = NULL;
static	unsigned numberofzoffsetstoextract = (unsigned) -1;
static	tmsize_t mosaicpiecesizelimit = 1 << 30; /* 2^30 = 1024 MiB = 1 GiB */
static	tmsize_t previewimagesizelimit = 1 << 20; /* 2^20 = 1 MiB */
static	uint32_t previewimagewidthlimit= 0;
static	uint32_t previewimagelengthlimit= 0;
static  tmsize_t compressionformatchangebuffersizelimit= 1 << 30; /* 1 GiB */
static	uint32_t requestedpiecewidth = 0;
static	uint32_t requestedpiecelength = 0;
static	uint32_t overlapinpixels = 0;
static	long double overlapinpercent = 0;
static	int mosaic_JPEG_quality = -1, splitimage_JPEG_quality = -1, default_JPEG_quality = 75;
#ifndef NDPISPLIT_VERBOSE
    #define NDPISPLIT_VERBOSE 0
#endif
static	int verbose = NDPISPLIT_VERBOSE;
static	int printcontroldata = 0;

static	int parseBoxLabel(const char *, const char *, BoxToExtract *);
static	int processNDPIFile(char*, int, int, unsigned, BoxToExtract*, int, uint16_t, uint16_t);
static	int magnificationShouldNotBeExtracted(float, unsigned, const float *);
static	int zoffsetShouldNotBeExtracted(int32_t, unsigned, const int32_t *);
static	int rewindToBeginningOfTIFF(TIFF*);
static	int cropNDPI2TIFF(TIFF*, TIFF*, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t);
static	void tiffMakeMosaic(TIFF*, uint16_t, int);
static	void computeMaxPieceMemorySize(uint32_t, uint32_t, uint16_t, uint16_t, uint32_t, uint32_t, uint32_t, long double, tmsize_t*, tmsize_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*);
static	void tiffCopyFieldsButDimensions(TIFF*, TIFF*);
static	int cpStrips(TIFF*, TIFF*, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t);
static	int cpStripsNoClipping(TIFF*, TIFF*, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t);
static	int cpTiles(TIFF*, TIFF*, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t);
static	int cpStrips2Tiles(TIFF*, TIFF*, uint32_t, uint32_t, uint32_t, uint32_t, uint16_t);
static	int cpTiles2Strip(TIFF*, void*, int, uint32_t, uint32_t, uint32_t, uint32_t, unsigned char*, uint16_t);
static	int cpStrips2Strip(TIFF*, void*, int, uint32_t, uint32_t, uint32_t, uint32_t, unsigned char*, uint16_t, uint32_t*, uint32_t);
static	int getNumberOfBlankLanes(TIFF*);
static	float getNDPIMagnification(TIFF*);
static	int getWidthAndLength(TIFF*, uint32_t*, uint32_t*, float);
static	unsigned int getScannedZonesFromMap(TIFF*, ScannedZoneBox **);
static	int writeOutTIFF(TIFF*, char*, int, uint32_t, uint32_t, uint32_t, uint32_t, int, uint16_t, uint16_t);
static	void findUnitsAtMagnification(TIFF*, float, uint32_t*, uint32_t*);
static	float* extendArrayOfFloats(float**, unsigned*, const char*);
static	MagnificationDescription* extendArrayOfMagnificationDescriptions(MagnificationDescription**, unsigned*, const char*);
static	int32_t* extendArrayOfInt32s(int32_t**, unsigned*, const char*);
static	BoxToExtract* extendArrayOfBoxes(BoxToExtract**, unsigned*, const char*);
/*static	int addToSetOfFloats(float**, unsigned*, const char*, float);*/
static	int addToSetOfMagnificationDescriptions(MagnificationDescription**, unsigned*, const char*, MagnificationDescription);
static	int addToSetOfInt32s(int32_t**, unsigned*, const char*, int32_t);
static	int searchNumberOfDigits(uint32_t);
static	int buildFileNameForExtract(const char *, float, int32_t, const char *, char **);
static	void my_asprintf(char** ret, const char* format, ...);
static	uint32_t my_floor(double);
static	uint32_t my_ceil(double);
static	void greetings();
static	void shortusage();
static	void usage(const char* message, ...);
static	void stderrErrorHandler(const char* module, const char* fmt, va_list ap);
static	void stderrWarningHandler(const char* module, const char* fmt, va_list ap);


int
main(int argc, char* argv[])
{
	TIFFErrorHandler oerror;
	/*TIFFErrorHandler owarning;*/

	int arg = 1;
#ifndef NDPISPLIT_SHOULDSUBDIVIDEINTOSCANNEDZONES
    #define NDPISPLIT_SHOULDSUBDIVIDEINTOSCANNEDZONES 0
#endif
	int shouldsubdivideintoscannedzones = NDPISPLIT_SHOULDSUBDIVIDEINTOSCANNEDZONES;
#ifndef NDPISPLIT_SHOULDMAKEMOSAICOFFILES
    #define NDPISPLIT_SHOULDMAKEMOSAICOFFILES 0
#endif
	int shouldmakemosaicoffiles = NDPISPLIT_SHOULDMAKEMOSAICOFFILES;
#ifndef NDPISPLIT_MOSAICCOMPRESSIONFORMAT
    #define NDPISPLIT_MOSAICCOMPRESSIONFORMAT COMPRESSION_JPEG
#endif
	int shouldmakepreviewonly = 0;
	unsigned numberofboxestoextract= 0;
	BoxToExtract * boxestoextract= NULL;
	uint16_t splitimagecompressionformat = -1;
	uint16_t mosaiccompressionformat = NDPISPLIT_MOSAICCOMPRESSIONFORMAT;
	int errorcode = 0;

	oerror = TIFFSetErrorHandler(NULL);
	/*owarning =*/ TIFFSetWarningHandler(NULL);

	while (arg < argc &&
	    argv[arg][0] == '-') {
		if (argv[arg][1] == 'v') {
			char * p = &(argv[arg][1]);
			while (*p == 'v') {
				p++;
				verbose++;
			}
		} else if (argv[arg][1] == 'h') {
			usage(NULL);
			return (0);
		}
		else if (argv[arg][1] == 'K')
			printcontroldata = 1;
		else if (argv[arg][1] == 'T' && argv[arg][2] == 'E') {
			TIFFSetErrorHandler(oerror);
		}
		else if (argv[arg][1] == 's')
			shouldsubdivideintoscannedzones = 1;
		else if (argv[arg][1] == 'p') {
			char * p = argv[arg]+2;

			shouldmakepreviewonly = 1;
			if (*p != 0) {
				unsigned long ul = strtoul(p, &p, 10);

				if (errno) {
					usage("size limit of preview image not understood.\n"); return (-3);
				}
				previewimagesizelimit= ul;
				while (isblank(*p))
					p++;
			}
			if (*p != 0) {
				unsigned long ul;

				if (*p != ',') {
					usage("comma expected after size limit on preview image.\n"); return (-3);
				}
				p++;
				ul= strtoul(p, &p, 10);
				if (errno) {
					usage("width limit of preview image not understood.\n"); return (-3);
				}
				previewimagewidthlimit= ul;
				while (isblank(*p))
					p++;
				if (*p != 'x') {
					usage("'x' expected between width and length limits on preview image.\n", ul, p-2); return (-3);
				}
				p++;
				ul= strtoul(p, NULL, 10);
				if (errno) {
					usage("length limit of preview image not understood.\n"); return (-3);
				}
				previewimagelengthlimit= ul;
			}
		} else if (argv[arg][1] == 'm' || argv[arg][1] == 'M') {
			char * p = argv[arg]+2;

			shouldmakemosaicoffiles =
				argv[arg][1] == 'M' ? 2 : 1 ;
			if (*p >= '0' && *p <= '9') {
				double mosaicpiecesizelimit_in_MiB =
					strtod(p, &p);
				if (errno || mosaicpiecesizelimit_in_MiB < 0 ||
				    !isfinite(mosaicpiecesizelimit_in_MiB)) {
					usage("Syntax error in maximum memory argument to option '-m' or '-M'.\n");
					return(-3);
				}
				mosaicpiecesizelimit = (tmsize_t) (1024. *
					mosaicpiecesizelimit_in_MiB) * 1024;
			}

			switch (*p) {
				case 0: break;
				case 'n': mosaiccompressionformat =
				    COMPRESSION_NONE; break;
				case 'l': mosaiccompressionformat =
				    COMPRESSION_LZW; break;
				case 'J': mosaiccompressionformat =
				    COMPRESSION_JPEG_IN_JPEG_FILE;
				    goto read_mosaic_JPEG_quality;
				case 'j': mosaiccompressionformat =
				    COMPRESSION_JPEG;
read_mosaic_JPEG_quality:
				    p++;
				    if (*p >= '0' && *p <= '9') {
					    unsigned long u;
					    u = strtoul(p, NULL, 10);
					    if (u == 0 || u > 100) {
						usage("Syntax error or value out of range in JPEG quality specification to option '-m' or '-M'.\n");
						return (-3);
					    }
					    mosaic_JPEG_quality = (int) u;
				    }
				    break;
				default: usage("Unsupported compression format in argument to option '-m' or '-M'.\n");
					return (-3);
			}
		} else if (argv[arg][1] == 'g') {
			char * p = argv[arg]+2;

			while (*p == ' ')
				p++;
			if (*p != 'x' && *p != 'X') {
				uint32_t u = strtoull(p, &p, 10);
				if (errno) {
					usage("syntax error in piece geometry option.\n");
					return(-3);
				}
				requestedpiecewidth = u;
			}
			while (*p == ' ')
				p++;
			if (*p != 'x' && *p != 'X') {
				usage("syntax error in piece geometry option.\n");
				return(-3);
			}
			p++;
			while (*p == ' ')
				p++;
			if (*p != 'x' && *p != 'X') {
				uint32_t u = strtoull(p, &p, 10);
				if (errno) {
					usage("Syntax error in piece geometry option.\n");
					return(-3);
				}
				requestedpiecelength = u;
			}
		} else if (argv[arg][1] == 'o') {
			size_t arglength;
			char * p = argv[arg]+2;

			if ((arglength= strlen(p)) == 0) {
				usage(NULL); return (-3);
			}

			if (p[arglength-1] == '%') {
				long double ld = strtold(p, NULL);

				if (errno || !isfinite(ld) ||
				    ld < 0 || ld > 100) {
					usage(NULL); return (-3);
				}
				overlapinpercent = ld;
				overlapinpixels = 0;
			} else {
				uint32_t u= strtoull(p, NULL, 10);

				if (errno) {
					usage(NULL); return (-3);
				}

				overlapinpixels = u;
				overlapinpercent = 0;
			}
		} else if (argv[arg][1] == 'x') {
			char * p = argv[arg]+2;

			numberofmagnificationstoextract = 0;
			p = strtok(p, ",");
			while (p != NULL) {
				float * p_magnificationtoextract;
				int consumedcharacters;

				p_magnificationtoextract = extendArrayOfFloats(
				    &magnificationstoextract,
				    &numberofmagnificationstoextract,
				    "specifications of magnifications to extract.\n");
				if (p_magnificationtoextract == NULL)
					return (-3);

				if (sscanf(p, "%f%n",
				    p_magnificationtoextract,
				    &consumedcharacters) != 1) {
					usage("unable to parse magnification to extract \"%s\".\n", p);
					return (-3);
				}
				if (*p_magnificationtoextract != -1 &&
				    *p_magnificationtoextract != -2 &&
				    *p_magnificationtoextract < 0) {
					usage("invalid magnification \"%s\".\n", p);
					return (-3);
				}

				p= strtok(NULL, ",");
				if (verbose >= 4) {
					fprintf(stderr, "Finished parsing magnification to extract %g\n",
					    *p_magnificationtoextract);
				}
			}
		} else if (argv[arg][1] == 'z') {
			char * p = argv[arg]+2;

			numberofzoffsetstoextract = 0;
			p = strtok(p, ",");
			while (p != NULL) {
				int32_t * p_zoffsettoextract;
				int consumedcharacters;

				p_zoffsettoextract = extendArrayOfInt32s(
				    &zoffsetstoextract,
				    &numberofzoffsetstoextract,
				    "specifications of magnifications to extract.\n");
				if (p_zoffsettoextract == NULL)
					return (-3);

				if (sscanf(p, "%d%n", p_zoffsettoextract,
				    &consumedcharacters) != 1) {
					usage("unable to parse z-offset to extract \"%s\".\n", p);
					return (-3);
				}

				p= strtok(NULL, ",");
				if (verbose >= 4) {
					fprintf(stderr, "Finished parsing z-offset to extract %d\n",
					    *p_zoffsettoextract);
				}
			}
		} else if (argv[arg][1] == 'e') {
			char * p = argv[arg]+2;

			p = strtok(p, ":");
			while (p != NULL) {
				BoxToExtract * box;
				char * r;
				int consumedcharacters;

				box = extendArrayOfBoxes(
				    &boxestoextract,
				    &numberofboxestoextract,
				    "specifications of boxes to extract.\n");
				if (box == NULL)
					return (-3);

				box->numberofmagnificationstoextract =
					(unsigned) -1;
				box->magnificationstoextract = NULL;
				box->numberofzoffsetstoextract =
					(unsigned) -1;
				box->zoffsetstoextract = NULL;
				box->width = 0;
				box->length = 0;

				if (sscanf(p, "%lf,%lf,%lf,%lf%n",
				    &(box->relxmin), &(box->relymin),
				    &(box->relwidth),
				    &(box->rellength),
				    &consumedcharacters) != 4) {
					usage("unable to parse coordinates for box to extract \"%s\".\n", p);
					return (-3);
				}
				if (box->relxmin < 0 || box->relymin < 0 ||
				    box->relwidth < 0 ||
				    box->rellength < 0 ||
				    box->relxmin + box->relwidth > 1 ||
				    box->relymin + box->rellength > 1) {
					usage("coordinate(s) out of range for box to extract \"%s\".\n", p);
					return (-3);
				}
				r = p + consumedcharacters;
				{
				int error = parseBoxLabel(r, p, box);
				if (error)
					return error;
				}

				p= strtok(NULL, ":");
				if (verbose >= 4) {
					fprintf(stderr, "Finished parsing box"
						" to extract %g,%g,%g,%g, ",
					    box->relxmin, box->relymin,
					    box->relwidth, box->rellength);
					if (box->label == NULL)
						fprintf(stderr, "no specified label.\n");
					else
						fprintf(stderr, "label \"%s\".\n",
						    box->label);
				}
			}
		} else if (argv[arg][1] == 'E') {
			char * p = argv[arg]+2;

			p = strtok(p, ":");
			while (p != NULL) {
				BoxToExtract * box;
				char * r;
				int consumedcharacters;

				box = extendArrayOfBoxes(
				    &boxestoextract,
				    &numberofboxestoextract,
				    "specifications of boxes to extract.\n");
				if (box == NULL)
					return (-3);

				box->magnificationstoextract = _TIFFmalloc(
				    sizeof(*(box->magnificationstoextract)));
				if (box->magnificationstoextract == NULL) {
					fprintf(stderr, "Error: insufficient memory for specifications of boxes to extract.\n");
					return (-3);
				}
				box->numberofmagnificationstoextract = 1;
				box->numberofzoffsetstoextract = -1;
				box->zoffsetstoextract = NULL;
				box->relwidth = 0;
				box->rellength = 0;

				if (*p != 'x' && *p != 'X') {
					usage("syntax error: magnification specification should start with an 'x' in box to extract \"%s\".\n", p);
					return (-3);
				}
				if (sscanf(p+1, "%f%n",
				    &(box->magnificationstoextract[0]),
				    &consumedcharacters) != 1) {
					usage("unable to parse magnification for box to extract \"%s\".\n", p);
					return (-3);
				}
				r = p + 1 + consumedcharacters;
				while (*r == ',' &&
				    (*(r+1) == 'z' || *(r+1) == 'Z')) {
					int32_t * p_zoffset;
					r += 2;
					p_zoffset = extendArrayOfInt32s(
					    &(box->zoffsetstoextract),
					    &(box->numberofzoffsetstoextract),
					    "specifications of boxes to extract.\n");
					if (p_zoffset == NULL)
						return (-3);
					if (sscanf(r, TIFF_INT32_FORMAT "%n",
					    p_zoffset,
					    &consumedcharacters) != 1) {
						usage("unable to parse z-offset for box to extract \"%s\".\n", p);
						return (-3);
					}
					r += consumedcharacters;
				}
				if (sscanf(r, "," TIFF_UINT32_FORMAT
				    "," TIFF_UINT32_FORMAT ","
				    TIFF_UINT32_FORMAT ","
				    TIFF_UINT32_FORMAT "%n",
				    &(box->xmin), &(box->ymin),
				    &(box->width), &(box->length),
				    &consumedcharacters) != 4) {
					usage("unable to parse coordinates for box to extract \"%s\".\n", p);
					return (-3);
				}

				r += consumedcharacters;
				{
				int error = parseBoxLabel(r, p, box);
				if (error)
					return error;
				}

				p= strtok(NULL, ":");
				if (verbose >= 4) {
					fprintf(stderr, "Finished parsing box"
						" to extract " 
						TIFF_INT32_FORMAT ","
						TIFF_INT32_FORMAT ","
						TIFF_INT32_FORMAT ","
						TIFF_INT32_FORMAT ", ",
					    box->xmin, box->ymin,
					    box->width, box->length);
					if (box->label == NULL)
						fprintf(stderr, "no specified label.\n");
					else
						fprintf(stderr, "label \"%s\".\n",
						    box->label);
				}
			}
		} else if (argv[arg][1] == 'c') {
			char * p = argv[arg]+2;

			if (*p >= '0' && *p <= '9') {
				double compressionformatchangebuffersizelimit_in_MiB =
					strtod(p, &p);
				if (errno ||
				    compressionformatchangebuffersizelimit_in_MiB < 0 ||
				    !isfinite(compressionformatchangebuffersizelimit_in_MiB)) {
					usage("Syntax error in maximum memory argument to option '-c'.\n");
					return(-3);
				}
				compressionformatchangebuffersizelimit =
					(tmsize_t) (1024. *
					compressionformatchangebuffersizelimit_in_MiB)
					* 1024;
			}

			switch (*p) {
				case 0:
				    usage("Option '-c' requires an argument.\n");
				    return (-3);
				case 'n': splitimagecompressionformat =
				    COMPRESSION_NONE; break;
				case 'l': splitimagecompressionformat =
				    COMPRESSION_LZW; break;
				case 'j': splitimagecompressionformat =
				    COMPRESSION_JPEG;
				    p++;
				    if (*p >= '0' && *p <= '9') {
					    unsigned long u;
					    u = strtoul(p, NULL, 10);
					    if (u == 0 || u > 100) {
						usage("Syntax error or value out of range in JPEG quality specification to option '-c'.\n");
						return (-3);
					    }
					    splitimage_JPEG_quality = (int) u;
				    }
				    break;
				default: usage("Unsupported compression format in argument to option '-c'.\n");
					return (-3);
			}
		} else {
			usage("%s: option not recognized.\n", argv[arg]);
			return(-3);
		}
		arg++;
	}

	if (printcontroldata) {
		printf("Ndpisplit version:1.5-2\n");
	}

	if (arg > argc-1) {
		shortusage(); return (-3);
	}

	if (numberofboxestoextract > 0)
		shouldsubdivideintoscannedzones= 0;

	if (verbose) {
		TIFFSetErrorHandler(stderrErrorHandler);
	}
	if (verbose >= 3) {
		TIFFSetWarningHandler(stderrWarningHandler);
	}

	for (; arg < argc ; arg++) {
		int r = processNDPIFile(argv[arg],
		    shouldmakepreviewonly,
		    shouldsubdivideintoscannedzones,
		    numberofboxestoextract, boxestoextract,
		    shouldmakemosaicoffiles, mosaiccompressionformat,
		    splitimagecompressionformat);
		if (r)
			errorcode = r;
	}
	return errorcode;
}

static int parseBoxLabel(const char * r, const char * p, BoxToExtract * box)
{
	if (*r == 0)
		box->label = NULL;
	else {
		if (*r != ',') {
			usage("unable to parse label for box to extract \"%s\".\n", p);
			return (-3);
		}
		r++;
		if (*r == '"' || strchr(r, ',') != NULL) {
			usage("in this version of ndpisplit, label for box to extract \"%s\" is not allowed to begin with '\"' or contain ','.\n", p);
			return (-3);
		} else {
			box->label= malloc(strlen(r)+1);
			if (box->label == NULL) {
				fprintf(stderr, "Insufficient memory to store a label of box to extract.\n");
				return (-3);
			}
			strcpy(box->label, r);
		}
	}
	return 0;
}

static int
processNDPIFile(char * NDPIfilename, int shouldmakepreviewonly,
	int shouldsubdivideintoscannedzones,
	unsigned numberofboxestoextract, BoxToExtract * boxestoextract,
	int shouldmakemosaicoffiles, uint16_t mosaiccompressionformat,
	uint16_t splitimagecompressionformat)
{
	TIFF * in;
	unsigned int nscannedzones = 0;
	ScannedZoneBox * scannedzoneboxes = NULL;
	uint32_t map_xmin = -1, map_ymin = -1, map_xmax = -1,
		map_ymax = -1;
	float maxndpimagnification = 0,
		ndpimagnificationofpreviewimage = 0;
	/*int ndpihasmacroimage = 0, ndpihasmap = 0;*/
	uint32_t maxmagn_width, maxmagn_length;
	/*uint32_t preview_width, preview_length;*/
	double ximagetomapratio = 0, yimagetomapratio = 0;
	tmsize_t previewimagesize = 0;
	/*tmsize_t macroimagesize = 0;*/
	MagnificationDescription * availablendpimagnifications = NULL;
	int32_t * availablendpizoffsets = NULL;
	unsigned numberofavailablendpimagnifications = 0,
	    numberofavailablendpizoffsets = 0;

	in = TIFFOpen(NDPIfilename, "r");
	if (in == NULL) {
		fprintf(stderr, "Unable to open file \"%s\", ignoring it.\n",
			NDPIfilename);
		return 1;
	}

	if (verbose)
		fprintf(stderr, "Processing file \"%s\"\n",
			NDPIfilename);

	if (shouldmakepreviewonly || printcontroldata) {
		/* Do a first pass to select the most appropriate
		 magnification and/or find what is available. */
		do {
			float ndpimagnification = getNDPIMagnification(in);

			if (ndpimagnification > 0) {
				tmsize_t imagesize;
				int32_t ndpizoffset;
				MagnificationDescription d = {
				    ndpimagnification, 0, 0};

				if (getWidthAndLength(in, &d.width,
				    &d.length, ndpimagnification)) {
					(void) TIFFClose(in);
					return (1);
				}

				if (addToSetOfMagnificationDescriptions(
				    &availablendpimagnifications,
				    &numberofavailablendpimagnifications,
				    "available magnifications", d))
					return (1);

				if (ndpimagnification >
				    maxndpimagnification) {
					maxndpimagnification =
						ndpimagnification;
					maxmagn_width = d.width;
					maxmagn_length = d.length;
				}

				if (! TIFFGetField(in, NDPITAG_ZOFFSET,
				    &ndpizoffset)) {
					TIFFError(TIFFFileName(in),
					"Error, z-Offset not found in NDPI file subdirectory");
					(void) TIFFClose(in);
					return (1);
				}
				if (addToSetOfInt32s(&availablendpizoffsets,
				    &numberofavailablendpizoffsets,
				    "available z-offsets",
				    ndpizoffset))
					return (1);

				imagesize = (tmsize_t) d.width * d.length;
				if (previewimagesizelimit &&
				    imagesize > previewimagesizelimit)
					continue;
				if (previewimagewidthlimit &&
				    d.width > previewimagewidthlimit)
					continue;
				if (previewimagelengthlimit &&
				    d.length > previewimagelengthlimit)
					continue;
				if (imagesize >
				    previewimagesize) {
					previewimagesize = imagesize;
					/*preview_width = width;
					preview_length = length;*/
					ndpimagnificationofpreviewimage
					    = ndpimagnification;
				}
			} else if (ndpimagnification == -1) {
				uint32_t width, length;

				if (getWidthAndLength(in, &width,
				    &length, ndpimagnification)) {
					(void) TIFFClose(in);
					return (1);
				}
				/*macroimagesize = (tmsize_t) width * length;*/
				/*ndpihasmacroimage = 1;*/
			} else if (ndpimagnification == -2) {
				/*ndpihasmap = 1;*/
			}

		} while (TIFFReadDirectory(in));

		if (rewindToBeginningOfTIFF(in))
			return (1);

		if (shouldmakepreviewonly && printcontroldata) {
			printf("Factor from preview image to largest image:%f\n",
			    ndpimagnificationofpreviewimage ?
				maxndpimagnification /
				ndpimagnificationofpreviewimage :
				0); /* todo: treat case when preview ==
				      macro */
			if (ndpimagnificationofpreviewimage)
				printf("Type of preview image:%gx\n",
				    ndpimagnificationofpreviewimage);
		}

		if (printcontroldata) {
			unsigned u;
			printf("Found images at magnifications:");
			for (u = 0 ; u < numberofavailablendpimagnifications ; u++)
				printf("%gx,",
				    availablendpimagnifications[u].magnification);
			printf("\nFound images of sizes:");
			for (u = 0 ; u < numberofavailablendpimagnifications ; u++)
				printf("%ux%u,",
				    availablendpimagnifications[u].width,
				    availablendpimagnifications[u].length);
			printf("\nFound images at z-offsets:");
			for (u = 0 ; u < numberofavailablendpizoffsets ; u++)
				printf(TIFF_INT32_FORMAT ",",
				    availablendpizoffsets[u]);
			printf("\n");
		}

	} else
		/* If asked for subdivision, look first at the
		 list of blank lanes to see if there is at least
		 one blank lane. If not, there should be no
		 subdivision; then avoid unnecessary first pass
		 on the file. */
	if (shouldsubdivideintoscannedzones &&
	    getNumberOfBlankLanes(in) > 0) {
		/* Do a first pass to find the map of scanned 
		 zoned, and read this map to get a list of 
		 scanned zones. */
		do {
			float ndpimagnification = getNDPIMagnification(in);
			if (ndpimagnification > 0 &&
			    ndpimagnification > maxndpimagnification) {
				if (getWidthAndLength(in,
					&maxmagn_width,
					&maxmagn_length,
					ndpimagnification)) {
					(void) TIFFClose(in);
					return (1);
				}
				maxndpimagnification= ndpimagnification;
			} else if (ndpimagnification == -2) {
				unsigned int n, first_non_empty;
				nscannedzones= getScannedZonesFromMap(in,
				    &scannedzoneboxes);
				if (verbose >= 2)
					fprintf(stderr, "Found map with %u (possibly empty) zones.\n",
						nscannedzones);

				if (nscannedzones == 0)
					break;

				for (first_non_empty = 0 ;
				    first_non_empty < nscannedzones ;
				    first_non_empty++)
					if (! scannedzoneboxes[first_non_empty].isempty)
						break;

				if (first_non_empty >= nscannedzones) {
					nscannedzones= 0;
					continue;
				}

				map_xmin= scannedzoneboxes[first_non_empty].map_xmin;
				map_ymin= scannedzoneboxes[first_non_empty].map_ymin;
				map_xmax= scannedzoneboxes[first_non_empty].map_xmax;
				map_ymax= scannedzoneboxes[first_non_empty].map_ymax;
				for (n = first_non_empty+1 ; n < nscannedzones ; n++) {
					if (scannedzoneboxes[n].isempty)
						continue;

					MIN(map_xmin, scannedzoneboxes[n].map_xmin);
					MIN(map_ymin, scannedzoneboxes[n].map_ymin);
					MAX(map_xmax, scannedzoneboxes[n].map_xmax);
					MAX(map_ymax, scannedzoneboxes[n].map_ymax);
				}
			}

		} while (TIFFReadDirectory(in));

		if (rewindToBeginningOfTIFF(in))
			return (1);

		{
			uint32_t xunit, yunit;
			findUnitsAtMagnification(in, maxndpimagnification,
				&xunit, &yunit);
			ximagetomapratio = (maxmagn_width/(31.*xunit))/
					(map_xmax+1-map_xmin);
			yimagetomapratio = (maxmagn_length/(1.*yunit))/
					(map_ymax+1-map_ymin);
		}

		if (verbose >= 3)
			fprintf(stderr, "Parameters for scanned zones "
				"extraction: maxndpimagnification=%f "
				"maxmagn_width=" TIFF_UINT32_FORMAT
				" maxmagn_length=" TIFF_UINT32_FORMAT
				" coeff_x=%f coeff_y=%f\n",
				maxndpimagnification, maxmagn_width, 
				maxmagn_length, ximagetomapratio,
				yimagetomapratio);
	}

	do {
		float ndpimagnification= getNDPIMagnification(in);
		char *path;
		int l;

		l = strlen(NDPIfilename);
		if ((NDPIfilename[l-1] == 'i' ||
		     NDPIfilename[l-1] == 'I') &&
		    (NDPIfilename[l-2] == 'p' ||
		     NDPIfilename[l-2] == 'P') &&
		    (NDPIfilename[l-3] == 'd' ||
		     NDPIfilename[l-3] == 'D') &&
		    (NDPIfilename[l-4] == 'n' ||
		     NDPIfilename[l-4] == 'N') &&
		    (NDPIfilename[l-5] == '.'))
			NDPIfilename[l-5] = 0;

		if (ndpimagnification == -1) {
			int r;

			if (shouldmakepreviewonly &&
			    ndpimagnificationofpreviewimage != 0)
				continue;
			if (magnificationShouldNotBeExtracted(ndpimagnification,
			    numberofmagnificationstoextract,
			    magnificationstoextract))
				continue;
			if (numberofboxestoextract > 0)
				continue;
			/* TODO: if macroimagesize >
			 previewimagesizelimit, create e.g. a black
			 image with the right proportions */
			my_asprintf(&path, "%s_macro%s",
				NDPIfilename,
				TIFF_SUFFIX);
			if (verbose)
				fprintf(stderr, "Extracting macroscopic image\n");
			if (printcontroldata) {
				printf("File containing macroscopic image:%s\n",
				    path);
				if (shouldmakepreviewonly)
					printf(
				    "Type of preview image:macroscopic\n"
				    "File containing a preview image:%s\n",
					    path);
			}
			r = writeOutTIFF(in, path, -2, 0, 0, 0, 0, 0,
				(uint16_t) -1, splitimagecompressionformat);
			if (r)
				return r;
		} else if (ndpimagnification == -2) {
			int r;
			if (magnificationShouldNotBeExtracted(ndpimagnification,
			    numberofmagnificationstoextract,
			    magnificationstoextract))
				continue;
			if (numberofboxestoextract > 0)
				continue;
			my_asprintf(&path, "%s_map%s",
				NDPIfilename,
				TIFF_SUFFIX);
			if (verbose)
				fprintf(stderr, "Extracting map of scanned zones\n");
			if (printcontroldata)
				printf("File containing map:%s\n",
					path);
			r = writeOutTIFF(in, path, -2, 0, 0, 0, 0, 0,
				(uint16_t) -1, splitimagecompressionformat);
			if (r)
				return r;
		} else if (! isnan(ndpimagnification)) {
			int r;
			uint32_t xunit, yunit;
			int32_t ndpizoffset=0;

			if (shouldmakepreviewonly &&
			    ndpimagnificationofpreviewimage !=
			    ndpimagnification)
				continue;
			if (magnificationShouldNotBeExtracted(ndpimagnification,
			    numberofmagnificationstoextract,
			    magnificationstoextract))
				continue;

			if (! TIFFGetField(in, NDPITAG_ZOFFSET,
				&ndpizoffset)) {
				TIFFError(TIFFFileName(in),
				"Error, z-Offset not found in NDPI file subdirectory");
				(void) TIFFClose(in);
				return (1);
			}

			if (zoffsetShouldNotBeExtracted(ndpizoffset,
			    numberofzoffsetstoextract, zoffsetstoextract))
				continue;

			if (verbose)
				fprintf(stderr, "Processing slice at magnification x%g at z-offset "
					TIFF_INT32_FORMAT "\n",
					ndpimagnification, ndpizoffset);

			findUnitsAtMagnification(in, ndpimagnification, &xunit, &yunit);

			if (numberofboxestoextract == 0 &&
				(nscannedzones == 0 || xunit == 0 ||
				yunit == 0)) {
				/* If the image is so small compared to
				 the largest available image that its
				 dimensions are not divisors of the
				 largest dimensions, or if there was an
				 error during computation of the
				 "units", don't subdivide, thus avoid
				 rounding problems */
				my_asprintf(&path, "%s_x%g_z"
				    TIFF_INT32_FORMAT "%s",
				    NDPIfilename,
				    ndpimagnification,
				    ndpizoffset, TIFF_SUFFIX);
				r = writeOutTIFF(in, path, -2, 0, 0, 0, 0,
					shouldmakemosaicoffiles,
					mosaiccompressionformat,
					splitimagecompressionformat);
				if (printcontroldata)
					printf(
					    shouldmakepreviewonly ?
				"File containing a preview image:%s\n" :
				"File containing a TIFF scanned image:%s\n",
						    path);
				if (r)
					return r;
			} else if (numberofboxestoextract > 0) {
				unsigned int n;
				uint32_t width, length;

				if (getWidthAndLength(in, &width,
					&length, ndpimagnification)) {
					(void) TIFFClose(in);
					return (1);
				}

				for (n = 0 ; n < numberofboxestoextract ; n++) {
					uint32_t xmin, xnextmax, ymin, ynextmax;
					int fd;
					BoxToExtract * box=
						&(boxestoextract[n]);

					if (magnificationShouldNotBeExtracted(
					    ndpimagnification,
			    		    box->numberofmagnificationstoextract,
					    box->magnificationstoextract))
						continue;
					if (zoffsetShouldNotBeExtracted(
					    ndpizoffset,
			    		    box->numberofzoffsetstoextract,
					    box->zoffsetstoextract))
						continue;

					if (box->relwidth == 0 &&
					    box->rellength == 0) {
						xmin = box->xmin;
						xnextmax = box->xmin+box->width;
						ymin = box->ymin;
						ynextmax = box->ymin+box->length;
					} else {
						xmin= my_floor(width*box->relxmin);
						xnextmax= my_ceil(width*
						    (box->relxmin+box->relwidth));
						ymin= my_floor(length*box->relymin);
						ynextmax= my_ceil(length*
						    (box->relymin+box->rellength));
					}

					if (xmin >= width || ymin >= length) {
						if (verbose >= 3)
							fprintf(stderr,
							    " Box to extract is outside of the image -- no extraction done.\n");
						continue;
					}

					if (verbose >= 3) {
						fprintf(stderr, " Box to extract %u, ",
							n);

						if (box->label == NULL)
							fprintf(stderr,
								"no label");
						else
							fprintf(stderr,
								"label=\"%s\"",
								box->label);

						fprintf(stderr,
							", relxmin=%f relymin=%f"
							" relwidth=%f rellength=%f"
							" -> xmin=" TIFF_UINT32_FORMAT
							" xmax=" TIFF_UINT32_FORMAT
							", ymin=" TIFF_UINT32_FORMAT
							" ymax=" TIFF_UINT32_FORMAT
							"\n",
							box->relxmin,
							box->relymin,
							box->relwidth,
							box->rellength,
							xmin, xnextmax-1, ymin, ynextmax-1);
					}

					fd= buildFileNameForExtract(
						NDPIfilename,
						ndpimagnification,
						ndpizoffset,
						box->label, &path);

					if (verbose >= 2)
						fprintf(stderr, "  Writing to \"%s\"...\n",
							path);

					r = writeOutTIFF(in, path, fd,
					    xmin, ymin,
 					    xnextmax-xmin, ynextmax-ymin,
					    shouldmakemosaicoffiles,
					    mosaiccompressionformat,
					    splitimagecompressionformat);
					if (printcontroldata)
						printf(
				"File containing a TIFF scanned image:%s\n",
						    path);
					_TIFFfree(path);
					if (r)
						return r;
				}
			} else {
				unsigned int n;

				for (n = 0 ; n < nscannedzones ; n++) {
					uint32_t xmin, xnextmax, ymin, ynextmax;

					if (scannedzoneboxes[n].isempty)
						continue;

					xmin= floor(ximagetomapratio*(scannedzoneboxes[n].map_xmin - map_xmin)) * 31 * xunit;
					xnextmax= ceil(ximagetomapratio*(scannedzoneboxes[n].map_xmax+1 - map_xmin)) * 31 * xunit;
					ymin= floor(yimagetomapratio*(scannedzoneboxes[n].map_ymin - map_ymin)) * yunit;
					ynextmax= ceil(yimagetomapratio*(scannedzoneboxes[n].map_ymax+1 - map_ymin)) * yunit;

					if (verbose >= 3) {
						fprintf(stderr, "  Scanned zone %u, "
							"map_xmin=" TIFF_UINT32_FORMAT " map_xmax=" TIFF_UINT32_FORMAT
							" map_ymin=" TIFF_UINT32_FORMAT " map_ymax=" TIFF_UINT32_FORMAT
							" -> xmin=" TIFF_UINT32_FORMAT " xmax=" TIFF_UINT32_FORMAT
							", ymin=" TIFF_UINT32_FORMAT " ymax=" TIFF_UINT32_FORMAT "\n",
							n,
							scannedzoneboxes[n].map_xmin,
							scannedzoneboxes[n].map_xmax,
							scannedzoneboxes[n].map_ymin,
							scannedzoneboxes[n].map_ymax,
							xmin, xnextmax-1, ymin, ynextmax-1);

						fprintf(stderr, "  (xunit=" TIFF_UINT32_FORMAT " xmin=floor(%f)*xunit xnextmax=ceil(%f)*xunit)\n",
							xunit, 1./7*(scannedzoneboxes[n].map_xmin - map_xmin),
							1./7*(scannedzoneboxes[n].map_xmax+1 - map_xmin) );
						fprintf(stderr, "  (yunit=" TIFF_UINT32_FORMAT " ymin=floor(%f)*yunit ynextmax=ceil(%f)*yunit)\n",
							yunit, 2.25*(scannedzoneboxes[n].map_ymin - map_ymin),
							2.25*(scannedzoneboxes[n].map_ymax+1 - map_ymin) );
					}

					my_asprintf(&path, "%s_x%g_z" TIFF_INT32_FORMAT
					    "_roi%u%s",
					    NDPIfilename,
					    ndpimagnification,
					    ndpizoffset, n+1,
					    TIFF_SUFFIX);

					if (verbose >= 2)
						fprintf(stderr, "  Writing to \"%s\"...\n",
							path);

					r = writeOutTIFF(in, path, -2,
					    xmin, ymin,
 					    xnextmax-xmin, ynextmax-ymin,
					    shouldmakemosaicoffiles,
					    mosaiccompressionformat,
					    splitimagecompressionformat);
					if (printcontroldata)
						printf(
				"File containing a TIFF scanned image:%s\n",
						    path);
					_TIFFfree(path);
					if (r)
						return r;
				}
			}
		}
	} while (TIFFReadDirectory(in));
	(void) TIFFClose(in);
	return (0);
}

static int magnificationShouldNotBeExtracted(float magnification,
	unsigned numberofmagnificationstoextract,
	const float * magnificationstoextract)
{
	unsigned u;
	if (numberofmagnificationstoextract == (unsigned) -1)
		return 0;
	for (u = 0 ; u < numberofmagnificationstoextract ; u++)
		if (magnificationstoextract[u] == magnification)
			return 0;
	return 1;
}

static int zoffsetShouldNotBeExtracted(int32_t zoffset,
	unsigned numberofzoffsetstoextract, const int32_t * zoffsetstoextract)
{
	unsigned u;
	if (numberofzoffsetstoextract == (unsigned) -1)
		return 0;
	for (u = 0 ; u < numberofzoffsetstoextract ; u++)
		if (zoffsetstoextract[u] == zoffset)
			return 0;
	return 1;
}

static int rewindToBeginningOfTIFF(TIFF* in)
{
	if (! TIFFSetDirectory(in, 0)) {
		TIFFError(TIFFFileName(in),
			"Error, impossible to rewind to beginning of NDPI file");
		(void) TIFFClose(in);
		return (1);
	}
	return 0;
}

static int
writeOutTIFF(TIFF* in, char* path, int fd, uint32_t xmin, uint32_t ymin,
	uint32_t width, uint32_t length, int shouldmakemosaicoffiles,
	uint16_t mosaiccompressionformat, uint16_t splitimagecompressionformat)
{
	TIFF* out= fd < 0 ?
		TIFFOpen(path, TIFFIsBigEndian(in)?"wb":"wl") :
		TIFFFdOpen(fd, path, TIFFIsBigEndian(in)?"wb":"wl");

	if (out == NULL)
		return (-2);
	if (!cropNDPI2TIFF(in, out, xmin, ymin, width, length,
	    splitimagecompressionformat) ||
	    !TIFFWriteDirectory(out))
		return (-1);

	if (shouldmakemosaicoffiles) {
	/* If the output file that has just been written is not tiled,
	 * there seems to be no easy way to re-read it without closing
	 * and reopening it: even resetting the current directory fails
	 * (there is an error "TIFFFillStrip: Data buffer too small to
	 * hold strip" during TIFFReadScanline).
	 * Alternatively, one could try to use TIFFFlush, but it seems
	 * to bump into a bug: after that, e.g., reading the
	 * bitspersample field yields an extravagant value. */
/*	if (! TIFFIsTiled(out) || fd >= 0)
{
fprintf(stderr, "$ will flush...\n");
		TIFFFlush(out);
fprintf(stderr, "$ have flushed.\n");
}*/
		if (! TIFFIsTiled(out) || fd >= 0) {
			if (verbose >= 5)
				fprintf(stderr, " Closing and reopening \"%s\"\n",
					TIFFFileName(out));
			TIFFClose(out);

			out = TIFFOpen(path, TIFFIsBigEndian(in)?"rb":"rl");
			if (out == NULL)
				return (-3);
			if (TIFFReadDirectory(out) == 0 &&
			    TIFFCurrentDirectory(out) != 0)
				return (-3);
		}

		tiffMakeMosaic(out, mosaiccompressionformat,
				shouldmakemosaicoffiles);
	}

	TIFFClose(out);

	return 0;
}

static int
cropNDPI2TIFF(TIFF* in, TIFF* out, uint32_t xmin, uint32_t ymin,
	uint32_t width, uint32_t length, uint16_t splitimagecompressionformat)
{
	uint32_t imagewidth, imagelength;
	int clipping= 1;

	tiffCopyFieldsButDimensions(in, out);

	 /* At this stage, the TIFF directory has already been read and
	  * the length already been fixed -- it is safe to read the
	  * length directly rather than calling getWidthAndLength */
	CopyField(TIFFTAG_IMAGEWIDTH, imagewidth);
	CopyField(TIFFTAG_IMAGELENGTH, imagelength);
	if (length > 0) { /* convention: length=0 <=> ignore x/ymin, width, length */
		if (xmin+width > imagewidth ) width= imagewidth-xmin;
		if (ymin+length > imagelength) length= imagelength-ymin;
		TIFFSetField(out, TIFFTAG_IMAGEWIDTH, width);
		TIFFSetField(out, TIFFTAG_IMAGELENGTH, length);
	} else {
		xmin = 0;
		ymin = 0;
		width = imagewidth;
		length = imagelength;
		clipping= 0;
	}

	if (splitimagecompressionformat == (uint16_t) -1)
		TIFFGetField(in, TIFFTAG_COMPRESSION, &splitimagecompressionformat);

	if (TIFFIsTiled(in))
		return (cpTiles(in, out, xmin, ymin, width, length, splitimagecompressionformat));
	else
		if (imagewidth >= 65500 || imagelength >= 65500)
			return (cpStrips2Tiles(in, out, xmin, ymin, width, length, splitimagecompressionformat));
		else
			if (! clipping)
				return (cpStripsNoClipping(in, out, xmin, ymin, width, length, splitimagecompressionformat));
			else
				return (cpStrips(in, out, xmin, ymin, width, length, splitimagecompressionformat));
}

static void
tiffMakeMosaic(TIFF* in, uint16_t mosaiccompressionformat,
		int shouldmakemosaicoffile)
{
	char * infilename;
	uint32_t inimagewidth, inimagelength, outwidth, outlength;
	uint32_t hoverlap, voverlap;
	uint32_t hnpieces, vnpieces;
	uint32_t ndigitshpiecenumber, ndigitsvpiecenumber, x, y;
	uint16_t spp, bitspersample;
	tmsize_t outmemorysize, ouroutmemorysize;
	unsigned char * outbuf = NULL;

	TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &inimagewidth);
	TIFFGetField(in, TIFFTAG_IMAGELENGTH, &inimagelength);
	TIFFGetField(in, TIFFTAG_SAMPLESPERPIXEL, &spp);
	TIFFGetField(in, TIFFTAG_BITSPERSAMPLE, &bitspersample);
	assert( bitspersample % 8 == 0 );

	if (verbose >= 5)
		fprintf(stderr, " tiffMakeMosaic, infile \"%s\" has "
			"width=" TIFF_UINT32_FORMAT " length="
			TIFF_UINT32_FORMAT " spp=%u bitspersample=%u\n",
			TIFFFileName(in), inimagewidth, inimagelength,
			spp, bitspersample);

	outwidth= requestedpiecewidth ? requestedpiecewidth : inimagewidth;
	outlength= requestedpiecelength ? requestedpiecelength :
		inimagelength;
	computeMaxPieceMemorySize(inimagewidth, inimagelength, spp,
		bitspersample, outwidth, outlength, overlapinpixels,
		overlapinpercent,
		&outmemorysize, &ouroutmemorysize, &hnpieces, &vnpieces,
		&hoverlap, &voverlap);
	if (shouldmakemosaicoffile <= 1 &&
	    (requestedpiecewidth == 0 || inimagewidth <= requestedpiecewidth) &&
	    (requestedpiecelength == 0 ||
	     inimagelength <= requestedpiecelength) &&
	    (mosaicpiecesizelimit == 0 ||
	     outmemorysize <= mosaicpiecesizelimit))
		return; /* Nothing to do */

	{
		uint16_t planarconfig;
		(void) TIFFGetField(in, TIFFTAG_PLANARCONFIG, &planarconfig);
		if (verbose >= 5)
			fprintf(stderr, " tiffMakeMosaic, infile \"%s\" : planarconfig=%u (contig would be %u)\n",
				TIFFFileName(in), planarconfig,
				PLANARCONFIG_CONTIG);
		assert(planarconfig == PLANARCONFIG_CONTIG);
	}

	if (mosaiccompressionformat == COMPRESSION_JPEG_IN_JPEG_FILE &&
	    ( (requestedpiecewidth >= JPEG_MAX_DIMENSION) ||
	      (requestedpiecelength >= JPEG_MAX_DIMENSION) ) ) {
		fprintf(stderr, "File \"%s\": at least one requested "
			"piece dimension is too large for JPEG "
			"files.\n", TIFFFileName(in));
		return;
	}

	if (requestedpiecewidth == 0 || requestedpiecelength == 0)
		while ( (mosaicpiecesizelimit &&
		    outmemorysize > mosaicpiecesizelimit) ||
		    (mosaiccompressionformat == COMPRESSION_JPEG_IN_JPEG_FILE &&
		    (outwidth > ORDINARY_JPEG_MAX_DIMENSION ||
		    outlength > ORDINARY_JPEG_MAX_DIMENSION))) {
		if (outlength > outwidth && outlength % 2 == 0 &&
		    requestedpiecelength == 0)
			outlength /= 2;
		else if (outwidth % 2 == 0 && requestedpiecewidth == 0)
			outwidth /= 2;
		else { /* can't divide any dimension by 2 */
			outwidth = 0;
			outlength = 0;
			break;
		}

		computeMaxPieceMemorySize(inimagewidth,
		    inimagelength, spp, bitspersample,
		    outwidth, outlength, overlapinpixels,
		    overlapinpercent,
		    &outmemorysize, &ouroutmemorysize,
		    &hnpieces, &vnpieces, &hoverlap, &voverlap);
	}

	if (outwidth == 0 || outlength == 0) {
		if (verbose)
			fprintf(stderr, "File \"%s\": impossible to find suitable width and length for mosaic pieces. Maybe you requested too small a memory size?\n",
			TIFFFileName(in));
		return;
	}

	outbuf= _TIFFmalloc(ouroutmemorysize);
	while (outbuf == NULL) {
		if (outlength > outwidth && outlength % 2 == 0)
			outlength /= 2;
		else if (outwidth % 2 == 0)
			outwidth /= 2;
		else
			break; /* can't divide any dimension by 2 */

		computeMaxPieceMemorySize(inimagewidth,
		    inimagelength, spp, bitspersample,
		    outwidth, outlength, overlapinpixels,
		    overlapinpercent,
		    &outmemorysize, &ouroutmemorysize,
		    &hnpieces, &vnpieces, &hoverlap, &voverlap);

		outbuf= _TIFFmalloc(ouroutmemorysize);
	}
	if (outbuf == NULL) {
		if (verbose && outmemorysize > mosaicpiecesizelimit)
			fprintf(stderr, "File \"%s\": unable to find width and length of mosaic pieces that will suit into memory during mosaic creation.\n",
				TIFFFileName(in));
		return;
	}

	if (verbose) {
		fprintf(stderr, "Making mosaic from file \"%s\"\n",
			TIFFFileName(in));
		if (verbose >= 3) {
			fprintf(stderr, " for each piece: maximum "
				"memory requirement " TIFF_UINT64_FORMAT
				" bytes (%0.3f MiB); width="
				TIFF_UINT32_FORMAT " length="
				TIFF_UINT32_FORMAT
				" pixels with overlaps of resp. "
				TIFF_UINT32_FORMAT " and "
				TIFF_UINT32_FORMAT " pixels; allocated "
				TIFF_UINT64_FORMAT " bytes\n",
				(uint64_t) mosaicpiecesizelimit,
				mosaicpiecesizelimit / 1048576.,
				outwidth, outlength, hoverlap, voverlap,
				(uint64_t) outlength * outwidth * spp *
				    (bitspersample/8));
			fprintf(stderr, " will generate " TIFF_UINT32_FORMAT
				" x " TIFF_UINT32_FORMAT " = "
				TIFF_UINT32_FORMAT " tiles\n",
				(uint32_t) hnpieces,
				(uint32_t) vnpieces,
				(uint32_t) (hnpieces * vnpieces));
		}
	}

	my_asprintf(&infilename, "%s", TIFFFileName(in));
	{
		int l = strlen(infilename);
		if (infilename[l-sizeof(TIFF_SUFFIX)+1] == '.')
			infilename[l-sizeof(TIFF_SUFFIX)+1]= 0;
	}

	ndigitshpiecenumber= searchNumberOfDigits(hnpieces);
	ndigitsvpiecenumber= searchNumberOfDigits(vnpieces);
	/* Loop over x, loop over y in that order, so that, when in is 
	 * not tiled, TIFFReadScanline calls are done sequentially from 
	 * 0 to H-1 then 0 to H-1 then... Otherwise (0 to h-1 then 0 to 
	 * h-1 then h to 2*h-1 then... with h<H),, reading fails. */
	for (x = 0 ; x < inimagewidth ; x += outwidth) {
		uint32_t outwidthwithoverlap;
		uint32_t y_of_last_read_scanline= 0;

		uint32_t leftoverlap = x < hoverlap ? x : hoverlap;
		uint32_t xwithleftoverlap= x - leftoverlap;

		uint32_t outwidthwithrightoverlap = outwidth + hoverlap;
		uint32_t xrightboundary = x + outwidthwithrightoverlap;
		    /* equal to xwithleftoverlap + outwidth + 2*hoverlap */
		assert(xrightboundary >= x); /* detect overflows */
		if (xrightboundary > inimagewidth)
			outwidthwithrightoverlap = inimagewidth - x;
		outwidthwithoverlap = leftoverlap + outwidthwithrightoverlap;

		assert(xwithleftoverlap < inimagewidth); /* xwol would be < 0 */
		assert(xwithleftoverlap + outwidthwithoverlap <= inimagewidth);

		for (y = 0 ; y < inimagelength ; y += outlength) {
			char * outfilename;
			void * out; /* TIFF* or FILE* */
			uint32_t outlengthwithoverlap;

			uint32_t topoverlap = y < voverlap ? y : voverlap;
			uint32_t ywithtopoverlap= y-topoverlap;

			uint32_t outlengthwithbottomoverlap= outlength + voverlap;
			uint32_t ybottomboundary = y + outlengthwithbottomoverlap;
			assert(ybottomboundary >= y); /* detect overflows */
			if (ybottomboundary > inimagelength)
			    outlengthwithbottomoverlap = inimagelength - y;
			outlengthwithoverlap =
				topoverlap + outlengthwithbottomoverlap;

			assert(ywithtopoverlap < inimagelength);
			 /* ywtol would be < 0 */
			assert(ywithtopoverlap + outlengthwithoverlap <=
				inimagelength);

			my_asprintf(&outfilename, "%s_i%0*uj%0*u%s",
			    infilename, ndigitsvpiecenumber,
			    y/outlength+1, ndigitshpiecenumber,
			    x/outwidth+1,
			    mosaiccompressionformat == COMPRESSION_JPEG_IN_JPEG_FILE ?
				JPEG_SUFFIX : TIFF_SUFFIX);

			out = mosaiccompressionformat == COMPRESSION_JPEG_IN_JPEG_FILE ?
			    fopen(outfilename, "wb") :
			    (void *) TIFFOpen(outfilename,
				TIFFIsBigEndian(in)?"wb":"wl");
			if (verbose >= 2)
				fprintf(stderr, " Writing mosaic tile \"%s\"\n",
					outfilename);
			_TIFFfree(outfilename);
			if (out == NULL)
				continue;

			if (mosaiccompressionformat ==
			    COMPRESSION_JPEG_IN_JPEG_FILE) {
				struct jpeg_compress_struct cinfo;
				struct jpeg_error_mgr jerr;

				cinfo.err = jpeg_std_error(&jerr);
				jpeg_create_compress(&cinfo);
				jpeg_stdio_dest(&cinfo, out);
				cinfo.image_width = outwidthwithoverlap;
				cinfo.image_height = outlengthwithoverlap;
				cinfo.input_components = spp; /* # of
					color components per pixel */
				cinfo.in_color_space = JCS_RGB; /* colorspace
					of input image */
				jpeg_set_defaults(&cinfo);
				if (mosaic_JPEG_quality <= 0) {
					uint16_t in_compression;

					TIFFGetField(in, TIFFTAG_COMPRESSION, &in_compression);
					if (in_compression == COMPRESSION_JPEG) {
						int in_jpegquality;
						TIFFGetField(in,
						    TIFFTAG_JPEGQUALITY,
						    &in_jpegquality);
						mosaic_JPEG_quality = in_jpegquality;
					} else
						mosaic_JPEG_quality = default_JPEG_quality;
				}
				if (verbose >= 3)
					fprintf(stderr, "JPEG quality set to %d.\n",
						mosaic_JPEG_quality);
				jpeg_set_quality(&cinfo, mosaic_JPEG_quality,
				    TRUE /* limit to baseline-JPEG values */);
				jpeg_start_compress(&cinfo, TRUE);

				if (verbose >= 4)
					fprintf(stderr, "Copying portion at ("
						TIFF_UINT32_FORMAT ", "
						TIFF_UINT32_FORMAT
						") of size " TIFF_UINT32_FORMAT
						" x " TIFF_UINT32_FORMAT
						" from TIFF file \"%s\"\n",
						xwithleftoverlap, ywithtopoverlap,
						outwidthwithoverlap,
						outlengthwithoverlap,
						TIFFFileName(in));

				if (TIFFIsTiled(in))
					cpTiles2Strip(in, &cinfo, 1,
					    xwithleftoverlap, ywithtopoverlap,
					    outwidthwithoverlap,
					    outlengthwithoverlap,
					    outbuf, mosaiccompressionformat);
				else
					cpStrips2Strip(in, &cinfo, 1,
					    xwithleftoverlap, ywithtopoverlap,
					    outwidthwithoverlap,
					    outlengthwithoverlap,
					    outbuf, mosaiccompressionformat,
					    &y_of_last_read_scanline,
					    inimagelength);

				jpeg_finish_compress(&cinfo);
				fclose(out);
				jpeg_destroy_compress(&cinfo);
			} else {
				TIFFSetField(out, TIFFTAG_IMAGEWIDTH, outwidthwithoverlap);
				TIFFSetField(out, TIFFTAG_IMAGELENGTH, outlengthwithoverlap);
				TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, outlengthwithoverlap);
				tiffCopyFieldsButDimensions(in, out);

				if (TIFFIsTiled(in))
					cpTiles2Strip(in, out, 0,
						xwithleftoverlap,
						ywithtopoverlap,
						outwidthwithoverlap,
						outlengthwithoverlap,
						outbuf,
						mosaiccompressionformat);
				else
					cpStrips2Strip(in, out, 0,
						xwithleftoverlap,
						ywithtopoverlap,
						outwidthwithoverlap,
						outlengthwithoverlap,
						outbuf,
						mosaiccompressionformat,
						&y_of_last_read_scanline,
						inimagelength);

				TIFFClose(out);
			}
		}
	}

	_TIFFfree(infilename);
	_TIFFfree(outbuf);
}

static void
computeMaxPieceMemorySize(uint32_t inimagewidth, uint32_t inimagelength,
	uint16_t spp, uint16_t bitspersample,
	uint32_t outpiecewidth, uint32_t outpiecelength,
	uint32_t overlapinpixels, long double overlapinpercent,
	tmsize_t * maxoutmemorysize, tmsize_t * ourmaxoutmemorysize,
	uint32_t * hnpieces, uint32_t * vnpieces,
	uint32_t * hoverlap, uint32_t * voverlap)
{
	uint32_t maxpiecewidthwithoverlap, maxpiecelengthwithoverlap;

	if (overlapinpercent > 0) {
		assert(overlapinpixels == 0);
		*hoverlap= lroundl(overlapinpercent * outpiecewidth / 100);
		*voverlap= lroundl(overlapinpercent * outpiecelength / 100);
	} else {
		*hoverlap= overlapinpixels;
		*voverlap= overlapinpixels;
	}

	if (*hoverlap > outpiecewidth)
		*hoverlap= outpiecewidth;
	if (*voverlap > outpiecelength)
		*voverlap= outpiecelength;

	*hnpieces = (inimagewidth+outpiecewidth-1) / outpiecewidth;
	*vnpieces = (inimagelength+outpiecelength-1) / outpiecelength;

	maxpiecewidthwithoverlap= outpiecewidth + *hoverlap * (
		*hnpieces >= 3 ? 2 : *hnpieces-1);
	maxpiecelengthwithoverlap= outpiecelength + *voverlap * (
		*vnpieces >= 3 ? 2 : *vnpieces-1);

	*ourmaxoutmemorysize= (tmsize_t) maxpiecewidthwithoverlap *
		maxpiecelengthwithoverlap * (bitspersample/8);
	*maxoutmemorysize= *ourmaxoutmemorysize * (spp == 3 ? 4 : spp);
	*ourmaxoutmemorysize *= spp;

	if (verbose >= 3)
		fprintf(stderr, "Trying with pieces of "
			TIFF_UINT32_FORMAT " x " TIFF_UINT32_FORMAT
			" and horiz. overlap " TIFF_UINT32_FORMAT
			", vert. overlap " TIFF_UINT32_FORMAT
			" (would need %.3f MiB per piece)...\n",
			outpiecewidth, outpiecelength, *hoverlap,
			*voverlap, *maxoutmemorysize / 1048576.0);
}

static void
tiffCopyFieldsButDimensions(TIFF* in, TIFF* out)
{
	uint16_t bitspersample, samplesperpixel, compression, shortv, *shortav;
	float floatv;
	char *stringv;
	uint32_t longv;

	CopyField(TIFFTAG_SUBFILETYPE, longv);
	CopyField(TIFFTAG_BITSPERSAMPLE, bitspersample);
	CopyField(TIFFTAG_SAMPLESPERPIXEL, samplesperpixel);
	CopyField(TIFFTAG_COMPRESSION, compression);
	CopyField(TIFFTAG_PHOTOMETRIC, shortv);
	CopyField(TIFFTAG_PREDICTOR, shortv);
	CopyField(TIFFTAG_THRESHHOLDING, shortv);
	CopyField(TIFFTAG_FILLORDER, shortv);
	CopyField(TIFFTAG_ORIENTATION, shortv);
	CopyField(TIFFTAG_MINSAMPLEVALUE, shortv);
	CopyField(TIFFTAG_MAXSAMPLEVALUE, shortv);
	CopyField(TIFFTAG_XRESOLUTION, floatv);
	CopyField(TIFFTAG_YRESOLUTION, floatv);
	CopyField(TIFFTAG_GROUP3OPTIONS, longv);
	CopyField(TIFFTAG_GROUP4OPTIONS, longv);
	CopyField(TIFFTAG_RESOLUTIONUNIT, shortv);
	CopyField(TIFFTAG_PLANARCONFIG, shortv);
	CopyField(TIFFTAG_XPOSITION, floatv);
	CopyField(TIFFTAG_YPOSITION, floatv);
	CopyField(TIFFTAG_IMAGEDEPTH, longv);
	CopyField(TIFFTAG_TILEDEPTH, longv);
	CopyField(TIFFTAG_SAMPLEFORMAT, shortv);
	CopyField2(TIFFTAG_EXTRASAMPLES, shortv, shortav);
	{ uint16_t *red, *green, *blue;
	  CopyField3(TIFFTAG_COLORMAP, red, green, blue);
	}
	{ uint16_t shortv2;
	  CopyField2(TIFFTAG_PAGENUMBER, shortv, shortv2);
	}
	CopyField(TIFFTAG_ARTIST, stringv);
	CopyField(TIFFTAG_IMAGEDESCRIPTION, stringv);
	CopyField(TIFFTAG_MAKE, stringv);
	CopyField(TIFFTAG_MODEL, stringv);
	CopyField(TIFFTAG_SOFTWARE, stringv);
	CopyField(TIFFTAG_DATETIME, stringv);
	CopyField(TIFFTAG_HOSTCOMPUTER, stringv);
	CopyField(TIFFTAG_PAGENAME, stringv);
	CopyField(TIFFTAG_DOCUMENTNAME, stringv);
	CopyField(TIFFTAG_BADFAXLINES, longv);
	CopyField(TIFFTAG_CLEANFAXDATA, longv);
	CopyField(TIFFTAG_CONSECUTIVEBADFAXLINES, longv);
	CopyField(TIFFTAG_FAXRECVPARAMS, longv);
	CopyField(TIFFTAG_FAXRECVTIME, longv);
	CopyField(TIFFTAG_FAXSUBADDRESS, stringv);
	CopyField(TIFFTAG_FAXDCS, stringv);
}

 /* Much faster than cpStrips, but defaults to cpStrips if there's not 
  * enough memory */
static int
cpStripsNoClipping(TIFF* in, TIFF* out, uint32_t xmin, uint32_t ymin,
	uint32_t width, uint32_t length, uint16_t requestedcompressionformat)
{
	tmsize_t bufsize  = TIFFStripSize(in);
	unsigned char *buf;
	uint16_t compression;

	TIFFGetFieldDefaulted(in, TIFFTAG_COMPRESSION, &compression);
	if (requestedcompressionformat == (uint16_t) -1)
		requestedcompressionformat = compression;

	if (requestedcompressionformat == compression &&
	    (buf = (unsigned char *)_TIFFmalloc(bufsize))) {
		tstrip_t s, ns = TIFFNumberOfStrips(in);
		uint64_t *bytecounts;
		uint32_t longv;
		uint16_t compression;

		CopyField(TIFFTAG_ROWSPERSTRIP, longv);

		TIFFGetFieldDefaulted(in, TIFFTAG_COMPRESSION, &compression);
		if (compression == COMPRESSION_JPEG) {
			uint32_t count = 0; /* bug in TIFF's tiffsplit : uint16_t should be uint32_t */
				/* posted patch on the TIFF mailing list on 2011-10-19 and committed in libtiff CVS on 2011-10-22 */
			void *table = NULL;
			if (TIFFGetField(in, TIFFTAG_JPEGTABLES, &count, &table)
			    && count > 0 && table) {
			    TIFFSetField(out, TIFFTAG_JPEGTABLES, count, table);
			}
		}

		if (!TIFFGetField(in, TIFFTAG_STRIPBYTECOUNTS, &bytecounts)) {
			fprintf(stderr, "ndpisplit: strip byte counts are missing\n");
			return (0);
		}
		for (s = 0; s < ns; s++) {
			if (bytecounts[s] > (uint64_t)bufsize) {
				buf = (unsigned char *)_TIFFrealloc(buf, (tmsize_t)bytecounts[s]);
				if (!buf)
					return (0);
				bufsize = (tmsize_t)bytecounts[s];
			}
			if (TIFFReadRawStrip(in, s, buf, (tmsize_t)bytecounts[s]) < 0 ||
			    TIFFWriteRawStrip(out, s, buf, (tmsize_t)bytecounts[s]) < 0) {
				_TIFFfree(buf);
				return (0);
			}
		}
		_TIFFfree(buf);
		return (1);
	} else {
		/* Not enough memory to read an entire strip, or change
		 * of compression format, try something slower */
		return cpStrips(in, out, xmin, ymin, width, length, requestedcompressionformat);
	}
	return (0);
}

static int
cpStrips(TIFF* in, TIFF* out, uint32_t xmin, uint32_t ymin, uint32_t width, uint32_t length, uint16_t requestedcompressionformat)
{
	/*  This function needs to be written: read successive
	 * scanlines, writing only parts of the lines that are inside 
	 * the requested zone (xmin, ymin, ...).
	 * In the present state, it will convert strips to tiles even if 
	 * it is not requested. */

/*	tmsize_t bufsize  = TIFFScanLineSize(in);
	unsigned char *buf = (unsigned char *)_TIFFmalloc(bufsize);


	if (buf) {
		_TIFFfree(buf);
		return (1);
	} else { */
		return cpStrips2Tiles(in, out, xmin, ymin, width, length, requestedcompressionformat);
/*	} */
}

static int
cpTiles(TIFF* in, TIFF* out, uint32_t xmin, uint32_t ymin, uint32_t width, uint32_t length, uint16_t requestedcompressionformat)
{
	/*  This function needs to be finished: read tiles,
	 * writing only parts of the tiles that are inside
	 * the requested zone (xmin, ymin, ...).
	 * In the present state, it will copy all tiles, irrespective of
	 * the requested zone, and ignore the requested compression
	 * format. It should be OK with most NDPI files, which are not
	 * tiled. */

	/* prevent warnings */
	(void) xmin;
	(void) ymin;
	(void) width;
	(void) length;

	tmsize_t bufsize = TIFFTileSize(in);

	{
		uint16_t incompression;
		TIFFGetField(in, TIFFTAG_COMPRESSION, &incompression);
		if (incompression != requestedcompressionformat) {
			TIFFError(TIFFFileName(in),
				"Error, this version of the program "
				"can't set the compression format "
				"of the output as requested");
			return(1);
		}
	}

	unsigned char *buf = (unsigned char *)_TIFFmalloc(bufsize);

	{
		uint32_t w, l;
		CopyField(TIFFTAG_TILEWIDTH, w);
		CopyField(TIFFTAG_TILELENGTH, l);
	}

	if (buf) {
		ttile_t t, nt = TIFFNumberOfTiles(in);
		uint64_t *bytecounts;

		if (!TIFFGetField(in, TIFFTAG_TILEBYTECOUNTS, &bytecounts)) {
			fprintf(stderr, "ndpisplit: tile byte counts are missing\n");
			return (0);
		}
		for (t = 0; t < nt; t++) {
			if (bytecounts[t] > (uint64_t) bufsize) {
				buf = (unsigned char *)_TIFFrealloc(buf, (tmsize_t)bytecounts[t]);
				if (!buf)
					return (0);
				bufsize = (tmsize_t)bytecounts[t];
			}
			if (TIFFReadRawTile(in, t, buf, (tmsize_t)bytecounts[t]) < 0 ||
			    TIFFWriteRawTile(out, t, buf, (tmsize_t)bytecounts[t]) < 0) {
				_TIFFfree(buf);
				return (0);
			}
		}
		_TIFFfree(buf);
		return (1);
	} else {
		TIFFError(TIFFFileName(in),
			"Error, can't allocate memory buffer of size "
			"%"TIFF_SSIZE_FORMAT " to read tiles",
			bufsize);
	}
	return (0);
}

static int readContigStripsIntoBuffer(TIFF* in, uint8_t* buf,
	uint32_t firstrow, uint32_t lengthtoread, tmsize_t bufsize)
{
	tmsize_t scanlinesize = TIFFRasterScanlineSize(in);
	uint8_t* bufp = buf;
	uint32_t row;

	if (lengthtoread * scanlinesize > bufsize) {
		/* stderr rather than TIFFError since there may be
		 * many such messages */
		fprintf(stderr,
			"%s: Error in calculating dimensions --- resulting image may be corrupted.\nConsider sending the input NDPI file to the author of ndpisplit for improvement.\n",
			TIFFFileName(in));
		lengthtoread = bufsize / scanlinesize;
	}

	for (row = firstrow; row < firstrow + lengthtoread; row++) {
		if (TIFFReadScanline(in, (tdata_t) bufp, row, 0) < 0) {
			TIFFError(TIFFFileName(in),
			    "Error, can't read scanline " TIFF_UINT32_FORMAT,
			    row);
			return 0;
		}
		bufp += scanlinesize;
	}

	return 1;
}

static void cpBufToBuf(uint8_t* out, uint8_t* in, uint32_t rows,
	uint32_t bytesperline, int outskew, int inskew)
{
	while (rows-- > 0) {
		uint32_t j = bytesperline;
		while (j-- > 0)
			*out++ = *in++;
		out += outskew;
		in += inskew;
	}
}

static int
writeBufferToContigTiles(TIFF* out, uint8_t* buf,
	uint32_t inimagerowsizeinbytes, uint32_t firstrow,
	uint32_t lengthtowrite, uint32_t firstcol,
	uint32_t widthtowrite, uint16_t bytesperpixel)
{
	tmsize_t tilew = TIFFTileRowSize(out); /* in bytes */
	int iskew = inimagerowsizeinbytes - tilew; /* in bytes */
	tmsize_t tilesize = TIFFTileSize(out); /* in bytes */
	tdata_t obuf;
	uint8_t* bufp = (uint8_t*) buf;
	uint32_t tl, tw;
	uint32_t row;

	if (widthtowrite * bytesperpixel > inimagerowsizeinbytes) {
		/* stderr rather than TIFFError since there may be
		 * many such messages, one per tile */
		fprintf(stderr,
			"%s: Error in calculating dimensions --- resulting image may be corrupted.\nConsider sending the input NDPI file to the author of ndpisplit for improvement.\n",
			TIFFFileName(out));
		widthtowrite = inimagerowsizeinbytes / bytesperpixel;
	}

	obuf = _TIFFmalloc(tilesize);
	if (obuf == NULL)
		return 0;
	_TIFFmemset(obuf, 0, tilesize);
	(void) TIFFGetField(out, TIFFTAG_TILELENGTH, &tl);
	(void) TIFFGetField(out, TIFFTAG_TILEWIDTH, &tw);
	for (row = firstrow; row < firstrow+lengthtowrite; row += tl) {
		uint32_t nrow = (row+tl > firstrow+lengthtowrite) ?
			firstrow+lengthtowrite-row : tl;
		uint32_t colb = firstcol * bytesperpixel;
		uint32_t col;

		for (col = 0; col < widthtowrite; col += tw) {
			/*
			 * Tile is clipped horizontally.  Calculate
			 * visible portion and skewing factors.
			 */
			if (colb + tilew > inimagerowsizeinbytes || col + tw > widthtowrite) {
				uint32_t width_according_to_in = inimagerowsizeinbytes - colb;
				uint32_t width_according_to_out = (widthtowrite - col)*bytesperpixel;
				uint32_t width= width_according_to_in > width_according_to_out ? width_according_to_out : width_according_to_in;
				int oskew = tilew - width; /* in bytes, not pixels */
				cpBufToBuf(obuf, bufp + colb, nrow, width,
				    oskew, oskew + iskew);
			} else
				cpBufToBuf(obuf, bufp + colb, nrow, tilew,
				    0, iskew);
			if (TIFFWriteTile(out, obuf, col, row, 0, 0) < 0) {
				TIFFError(TIFFFileName(out),
				    "Error, can't write tile at "
				    TIFF_UINT32_FORMAT " " TIFF_UINT32_FORMAT,
				    col, row);
				_TIFFfree(obuf);
				return 0;
			}
			colb += tilew;
		}
		bufp += nrow * inimagerowsizeinbytes;
	}
	_TIFFfree(obuf);
	return 1;
}

static int
cpStrips2Tiles(TIFF* in, TIFF* out, uint32_t xmin, uint32_t ymin,
	uint32_t width, uint32_t length, uint16_t requestedcompression)
{
	uint32_t tilewidth = (uint32_t) -1, tilelength = (uint32_t) -1;
	uint16_t spp, bitspersample, bytesperpixel;
	uint32_t inimagelength, bufferlength;
	tmsize_t inimagerowsizeinbytes, bufsize;
	unsigned char *buf;

	TIFFDefaultTileSize(out, &tilewidth, &tilelength);
		/* NDPI images are acquired through 128 pixel-wide 
		 columns, thus try to align the tiles' limits on the 
		 columns -- this is useful at least for the highest 
		 resolution images */
	tilewidth = 128;
	TIFFSetField(out, TIFFTAG_TILEWIDTH, tilewidth);
	TIFFSetField(out, TIFFTAG_TILELENGTH, tilelength);

	TIFFGetField(in, TIFFTAG_SAMPLESPERPIXEL, &spp);
	TIFFGetField(in, TIFFTAG_BITSPERSAMPLE, &bitspersample);
	assert( bitspersample % 8 == 0 );
	bytesperpixel = (bitspersample/8) * spp;

	TIFFGetField(in, TIFFTAG_IMAGELENGTH, &inimagelength);
	if (inimagelength == (uint32_t) -1) {
		TIFFError(TIFFFileName(in),
				"Error, can't read reasonable image length and/or width");
		return (0);
	}

	if (requestedcompression == (uint16_t) -1)
		TIFFGetField(out, TIFFTAG_COMPRESSION, &requestedcompression);
	else
		TIFFSetField(out, TIFFTAG_COMPRESSION, requestedcompression);
	TIFFSetField(in, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
	if (requestedcompression == COMPRESSION_JPEG) {
		/* like in tiffcp.c -- otherwise the reserved size for
		 the tiles is too small and the program segfaults */
		TIFFSetField(out, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
	}

	inimagerowsizeinbytes= TIFFRasterScanlineSize(in);
	bufferlength = tilelength;
	bufsize = inimagerowsizeinbytes * bufferlength;

	buf = (unsigned char *)_TIFFmalloc(bufsize);
	if (!buf) {
		TIFFError(TIFFFileName(in),
				"Error, can't allocate space for image buffer");
		return (0);
	} else {
		int success = 1;
		uint32_t row, lengthtodo;

		/* Skip unwanted lines but read them to avoid error 
		 "Compression algorithm does not support random access" */
		for (row = 0 ; row < ymin ; row++) {
			if (verbose >= 1 &&
			    (ymin-row) % bufferlength == 0)
				fprintf(stderr, "  cpStrips2Tiles remaining lines: " TIFF_UINT32_FORMAT " \r",
					length + ymin-row);
			if (TIFFReadScanline(in, (tdata_t) buf, row, 0) < 0) {
				TIFFError(TIFFFileName(in),
				    "Error, can't read scanline " 
				    TIFF_UINT32_FORMAT,
				    row);
			}
		}

		/* Now process wanted lines */
		for (lengthtodo = length ; lengthtodo >= bufferlength ;
		    lengthtodo -= bufferlength) {
			if (readContigStripsIntoBuffer(in, (uint8_t*)buf, 
					ymin+length-lengthtodo,
					bufferlength, bufsize)) {
				success = writeBufferToContigTiles(out,
						(uint8_t*)buf,
						inimagerowsizeinbytes,
						length-lengthtodo,
						bufferlength, xmin,
						width, bytesperpixel);
			}

		if (verbose >= 1)
			fprintf(stderr, "  cpStrips2Tiles remaining lines: " TIFF_UINT32_FORMAT " \r",
				lengthtodo);
		}

		if (success && lengthtodo > 0) {
			if (readContigStripsIntoBuffer(in, (uint8_t*)buf,
				ymin+length-lengthtodo, lengthtodo, bufsize)) {

				success = writeBufferToContigTiles(out,
					(uint8_t*)buf,
					inimagerowsizeinbytes,
					length-lengthtodo,
					lengthtodo, xmin, width, bytesperpixel);
			}

		if (verbose >= 1)
			fprintf(stderr, "  cpStrips2Tiles remaining lines: " TIFF_UINT32_FORMAT " \r",
				lengthtodo);
		}
		if (verbose >= 2)
			fprintf(stderr, "  cpStrips2Tiles completed.        \n");
		_TIFFfree(buf);
		return (success);
	}
	return (0);
}

static int
cpTiles2Strip(TIFF* in, void * ambiguous_out,
    int output_to_jpeg_rather_than_tiff, uint32_t xmin, uint32_t ymin,
    uint32_t width, uint32_t length, unsigned char * outbuf,
    uint16_t compressionformat)
{
	struct jpeg_compress_struct * p_cinfo;
	TIFF* TIFFout;
	tmsize_t inbufsize;
	uint16_t in_compression, in_photometric;
	uint16_t spp, bitspersample, bytesperpixel;
	uint32_t intilewidth = (uint32_t) -1, intilelength = (uint32_t) -1;
	tmsize_t intilewidthinbytes = TIFFTileRowSize(in);
	uint32_t y;
	tmsize_t outscanlinesizeinbytes;
	unsigned char * inbuf, * bufp= outbuf;
	int success = 1;

	if (output_to_jpeg_rather_than_tiff) {
		p_cinfo = (struct jpeg_compress_struct *) ambiguous_out;
		TIFFout = NULL;
	} else {
		p_cinfo = NULL;
		TIFFout = (TIFF*) ambiguous_out;
	}

	TIFFGetField(in, TIFFTAG_TILEWIDTH, &intilewidth);
	TIFFGetField(in, TIFFTAG_TILELENGTH, &intilelength);
	TIFFGetField(in, TIFFTAG_SAMPLESPERPIXEL, &spp);
	TIFFGetField(in, TIFFTAG_BITSPERSAMPLE, &bitspersample);
	if (output_to_jpeg_rather_than_tiff) {
		assert( bitspersample == 8 );
		assert( spp == 3 );
	} else
		assert( bitspersample % 8 == 0 );
	bytesperpixel = (bitspersample/8) * spp;

	TIFFGetField(in, TIFFTAG_COMPRESSION, &in_compression);
	TIFFGetFieldDefaulted(in, TIFFTAG_PHOTOMETRIC, &in_photometric);
	if (in_compression == COMPRESSION_JPEG) {
		/* "in" is a file we have just written. If it was not 
		 * closed and reopen, then it is tiled and we have
		 * already set it to JPEGCOLORMODE_RGB to write it pixel
		 * by pixel. If it was closed and reopen, then we can 
		 * set JPEGCOLORMODE as we wish. In either case, we 
		 * won't make a conflict with a previously set value 
		 * while changing the JPEGCOLORMODE. But let's not reset 
		 * it if it was already set to avoid an error message 
		 * `Cannot modify tag "" while writing'. */

		int in_jpegcolormode;
		TIFFGetField(in, TIFFTAG_JPEGCOLORMODE, &in_jpegcolormode);

		if (in_jpegcolormode != JPEGCOLORMODE_RGB)
		/* like in tiffcp.c -- otherwise the reserved size for
		 * the tiles is too small and the program segfaults */
			TIFFSetField(in, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
	}

	if (output_to_jpeg_rather_than_tiff) {
		outscanlinesizeinbytes = width * bytesperpixel;
	} else {

		switch (compressionformat) {
		case COMPRESSION_LZW:
			TIFFSetField(TIFFout, TIFFTAG_COMPRESSION,
				COMPRESSION_LZW);
			TIFFSetField(TIFFout, TIFFTAG_PHOTOMETRIC,
				PHOTOMETRIC_RGB);
			break;
		case COMPRESSION_JPEG:
			TIFFSetField(TIFFout, TIFFTAG_COMPRESSION,
				COMPRESSION_JPEG);
			TIFFSetField(TIFFout, TIFFTAG_PHOTOMETRIC,
				in_photometric);
			TIFFSetField(TIFFout, TIFFTAG_JPEGCOLORMODE,
				JPEGCOLORMODE_RGB);
			break;
		case COMPRESSION_NONE:
			TIFFSetField(TIFFout, TIFFTAG_COMPRESSION,
				COMPRESSION_NONE);
			TIFFSetField(TIFFout, TIFFTAG_PHOTOMETRIC,
				PHOTOMETRIC_RGB);
			break;
		default:
			fprintf(stderr, "Bug: unknown compression method in cpTiles2Strip.\n");
			exit(EXIT_FAILURE);
		}

		/* To be done *after* setting compression -- otherwise,
		 * ScanlineSize may be wrong */
		outscanlinesizeinbytes= TIFFRasterScanlineSize(TIFFout);

/*{
uint16_t out_compression= -1, out_photometric= -1;

TIFFGetField(TIFFout, TIFFTAG_COMPRESSION, &out_compression);
TIFFGetField(TIFFout, TIFFTAG_PHOTOMETRIC, &out_photometric);
fprintf(stderr, "Infile had compression %u and photometric %u ; outfile will have compression %u and photometric %u\n",
	in_compression, in_photometric, out_compression, out_photometric);
}

fprintf(stderr, "Outfile \"%s\": compression set scanlinesize=%lld or %lld\n", 
TIFFFileName(TIFFout), outscanlinesizeinbytes, TIFFScanlineSize(TIFFout));*/

	}

	inbufsize= TIFFTileSize(in);
	inbuf = (unsigned char *)_TIFFmalloc(inbufsize);
	if (!inbuf) {
		TIFFError(TIFFFileName(in),
				"Error, can't allocate space for image buffer");
		return (0);
	}

	for (y = ymin ; y < ymin + length + intilelength ; y += intilelength) {
		uint32_t x, colb = 0;
		uint32_t yminoftile = (y/intilelength) * intilelength;
		uint32_t ymintocopy = ymin > yminoftile ? ymin : yminoftile;
		uint32_t ymaxplusone = yminoftile + intilelength;
		uint32_t lengthtocopy;
		unsigned char * inbufrow = inbuf +
		    intilewidthinbytes * (ymintocopy-yminoftile);

		if (ymaxplusone > ymin + length)
			ymaxplusone = ymin + length;
		if (ymaxplusone <= yminoftile)
			break;
		lengthtocopy = ymaxplusone - ymintocopy;

		for (x = xmin ; x < xmin + width + intilewidth ;
		    x += intilewidth) {
			uint32_t xminoftile = (x/intilewidth) * intilewidth;
			uint32_t xmintocopyintile= xmin > xminoftile ?
			    xmin : xminoftile;
			uint32_t xmaxplusone = xminoftile + intilewidth;
			tmsize_t widthtocopyinbytes;

			if (xmaxplusone > xmin + width)
				xmaxplusone = xmin + width;
			if (xmaxplusone <= xminoftile)
				break;
			widthtocopyinbytes = (xmaxplusone - xmintocopyintile) *
				bytesperpixel;

			if (TIFFReadTile(in, inbuf, xminoftile, 
			    yminoftile, 0, 0) < 0) {
				TIFFError(TIFFFileName(in),
				    "Error, can't read tile at "
				    TIFF_UINT32_FORMAT " " TIFF_UINT32_FORMAT,
				    xminoftile, yminoftile);
				success = 0;
				goto done;
			}

			cpBufToBuf(bufp + colb,
			    inbufrow + (xmintocopyintile-xminoftile) *
				bytesperpixel, lengthtocopy,
			    widthtocopyinbytes,
			    outscanlinesizeinbytes - widthtocopyinbytes,
			    intilewidthinbytes - widthtocopyinbytes);
			colb += widthtocopyinbytes;
		}
		bufp += outscanlinesizeinbytes * lengthtocopy;
	}

	if (output_to_jpeg_rather_than_tiff) {
		JSAMPROW row_pointer;
		JSAMPROW* row_pointers =
			_TIFFmalloc(length * sizeof(JSAMPROW));

		if (row_pointers == NULL) {
			TIFFError(TIFFFileName(in),
				"Error, can't allocate space for row_pointers");
			success = 0;
			goto done;
		}

		for (y = 0, row_pointer = outbuf ; y < length ;
		    y++, row_pointer += width * bytesperpixel)
			row_pointers[y]= row_pointer;

		jpeg_write_scanlines(p_cinfo, row_pointers, length);
	} else {
		if (TIFFWriteEncodedStrip(TIFFout,
			TIFFComputeStrip(TIFFout, 0, 0),
		    outbuf, TIFFStripSize(TIFFout)) < 0) {
			TIFFError(TIFFFileName(TIFFout),
			    "Error, can't write strip");
			success = 0;
		}
	}

	done:
	_TIFFfree(inbuf);
	return success;
}

static int
cpStrips2Strip(TIFF* in, void * ambiguous_out,
    int output_to_jpeg_rather_than_tiff, uint32_t xmin, uint32_t ymin,
    uint32_t width, uint32_t length, unsigned char * outbuf,
    uint16_t compressionformat, uint32_t * y_of_last_read_scanline,
    uint32_t inimagelength)
{
	struct jpeg_compress_struct * p_cinfo;
	TIFF* TIFFout;
	tmsize_t inbufsize;
	uint16_t in_compression, in_photometric;
	uint16_t spp, bitspersample, bytesperpixel;
	tmsize_t inwidthinbytes = TIFFRasterScanlineSize(in);
	uint32_t y;
	tmsize_t outscanlinesizeinbytes;
	unsigned char * inbuf, * bufp= outbuf;
	int success = 1;

	if (output_to_jpeg_rather_than_tiff) {
		p_cinfo = (struct jpeg_compress_struct *) ambiguous_out;
		TIFFout = NULL;
	} else {
		p_cinfo = NULL;
		TIFFout = (TIFF*) ambiguous_out;
	}

	TIFFGetField(in, TIFFTAG_SAMPLESPERPIXEL, &spp);
	TIFFGetField(in, TIFFTAG_BITSPERSAMPLE, &bitspersample);
	if (output_to_jpeg_rather_than_tiff) {
		assert( bitspersample == 8 );
		assert( spp == 3 );
	} else
		assert( bitspersample % 8 == 0 );
	bytesperpixel = (bitspersample/8) * spp;

	TIFFGetField(in, TIFFTAG_COMPRESSION, &in_compression);
	TIFFGetFieldDefaulted(in, TIFFTAG_PHOTOMETRIC, &in_photometric);
	if (in_compression == COMPRESSION_JPEG) {
		/* "in" is a file we have just written. If it was not 
		 * closed and reopen, then it is tiled and we have
		 * already set it to JPEGCOLORMODE_RGB to write it pixel
		 * by pixel. If it was closed and reopen, then we can 
		 * set JPEGCOLORMODE as we wish. In either case, we 
		 * won't make a conflict with a previously set value 
		 * while changing the JPEGCOLORMODE. */

		/* like in tiffcp.c -- otherwise the reserved size for
		 * the tiles is too small and the program segfaults */
		TIFFSetField(in, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
	}

	if (output_to_jpeg_rather_than_tiff) {
		outscanlinesizeinbytes = width * bytesperpixel;
	} else {

		switch (compressionformat) {
		case COMPRESSION_LZW:
			TIFFSetField(TIFFout, TIFFTAG_COMPRESSION,
				COMPRESSION_LZW);
			TIFFSetField(TIFFout, TIFFTAG_PHOTOMETRIC,
				PHOTOMETRIC_RGB);
			break;
		case COMPRESSION_JPEG:
			TIFFSetField(TIFFout, TIFFTAG_COMPRESSION,
				COMPRESSION_JPEG);
			TIFFSetField(TIFFout, TIFFTAG_PHOTOMETRIC,
				in_photometric);
			TIFFSetField(TIFFout, TIFFTAG_JPEGCOLORMODE,
				JPEGCOLORMODE_RGB);
			break;
		case COMPRESSION_NONE:
			TIFFSetField(TIFFout, TIFFTAG_COMPRESSION,
				COMPRESSION_NONE);
			TIFFSetField(TIFFout, TIFFTAG_PHOTOMETRIC,
				PHOTOMETRIC_RGB);
			break;
		default:
			fprintf(stderr, "Bug: unknown compression method in cpTiles2Strip.\n");
			exit(EXIT_FAILURE);
		}

		/* To be done *after* setting compression -- otherwise,
		 * ScanlineSize may be wrong */
		outscanlinesizeinbytes= TIFFRasterScanlineSize(TIFFout);
/*{
uint16_t out_compression= -1, out_photometric= -1;

TIFFGetField(TIFFout, TIFFTAG_COMPRESSION, &out_compression);
TIFFGetField(TIFFout, TIFFTAG_PHOTOMETRIC, &out_photometric);
fprintf(stderr, "Infile had compression %u and photometric %u ; outfile will have compression %u and photometric %u\n",
	in_compression, in_photometric, out_compression, out_photometric);
}

fprintf(stderr, "Outfile \"%s\": compression set scanlinesize="
  TIFF_UINT64_FORMAT " or " TIFF_UINT64_FORMAT "\n",
  TIFFFileName(TIFFout), outscanlinesizeinbytes, TIFFScanlineSize(TIFFout));*/

	}

	inbufsize= TIFFRasterScanlineSize(in);
	inbuf = (unsigned char *)_TIFFmalloc(inbufsize);
	if (!inbuf) {
		TIFFError(TIFFFileName(in),
				"Error, can't allocate space for image buffer");
		return (0);
	}

	/* Restart reading from the beginning if we need to go back
	 * (e.g. because we read more to give some overlap between
	 * pieces), since some compression methods don't support random
	 * access. */
	if (*y_of_last_read_scanline > ymin) {
		/* Finish reading to the end, then a restart will be
		 * automatic, then read up to the point we want to start
		 * copying at */
		uint32_t y;

		for (y = *y_of_last_read_scanline + 1 ; y < inimagelength ; y++)
			if (TIFFReadScanline(in, inbuf, y, 0) < 0) {
				TIFFError(TIFFFileName(in),
				    "Error, can't read scanline at "
				    TIFF_UINT32_FORMAT " for exhausting",
				    y);
				success = 0;
				goto done;
			} else
				*y_of_last_read_scanline= y;

		for (y = 0 ; y < ymin ; y++)
			if (TIFFReadScanline(in, inbuf, y, 0) < 0) {
				TIFFError(TIFFFileName(in),
				    "Error, can't read scanline at "
				    TIFF_UINT32_FORMAT " for exhausting",
				    y);
				success = 0;
				goto done;
			} else
				*y_of_last_read_scanline= y;
	}

	for (y = ymin ; y < ymin + length ; y++) {
		unsigned char * inbufrow = inbuf;
		uint32_t xmintocopyinscanline = xmin;
		tmsize_t widthtocopyinbytes = outscanlinesizeinbytes;

		if (TIFFReadScanline(in, inbuf, y, 0) < 0) {
			TIFFError(TIFFFileName(in),
				"Error, can't read scanline at "
				TIFF_UINT32_FORMAT " for copying",
				y);
			success = 0;
			goto done;
		} else
			*y_of_last_read_scanline= y;

		cpBufToBuf(bufp,
		    inbufrow + xmintocopyinscanline * bytesperpixel,
		    1, widthtocopyinbytes,
		    outscanlinesizeinbytes - widthtocopyinbytes,
		    inwidthinbytes - widthtocopyinbytes);
		bufp += outscanlinesizeinbytes;
	}

	if (output_to_jpeg_rather_than_tiff) {
		JSAMPROW row_pointer;
		JSAMPROW* row_pointers =
			_TIFFmalloc(length * sizeof(JSAMPROW));

		if (row_pointers == NULL) {
			TIFFError(TIFFFileName(in),
				"Error, can't allocate space for row_pointers");
			success = 0;
			goto done;
		}

		for (y = 0, row_pointer = outbuf ; y < length ;
		    y++, row_pointer += width * bytesperpixel)
			row_pointers[y]= row_pointer;

		jpeg_write_scanlines(p_cinfo, row_pointers, length);
	} else {
		if (TIFFWriteEncodedStrip(TIFFout,
			TIFFComputeStrip(TIFFout, 0, 0),
		    outbuf, TIFFStripSize(TIFFout)) < 0) {
			TIFFError(TIFFFileName(TIFFout),
			    "Error, can't write strip");
			success = 0;
		}
	}

	done:
	_TIFFfree(inbuf);
	return success;
}

static int
getNumberOfBlankLanes(TIFF* in)
{
	uint32_t nblanklanes;
	uint32_t *blanklanes;

	if (! TIFFGetField(in, NDPITAG_BLANKLANES, &nblanklanes, &blanklanes) )
		return 0;
	/* Now, blank lane numbers are stored in blanklanes[0], blanklanes[1]... */

	/* TODO/FIXME: for some files, where scanned regions where 
	 * delimited with freehand draws rather than rectangles,
	 * NDPITAG_BLANKLANES is an array with a single value, 4071. 
	 * This value is not a blank lane number. Some files with 
	 * freehand-drawn boundaries yet have a correct list of blank
	 * lanes. */

	return nblanklanes;
}

static float
getNDPIMagnification(TIFF* in)
{
	float f;

	if (! TIFFGetField(in, NDPITAG_MAGNIFICATION, &f) ) {
		TIFFError(TIFFFileName(in),
			"Error, Magnification not found in NDPI file subdirectory");
		return(NAN);
	}
	return f;
}

static int
getWidthAndLength(TIFF* in, uint32_t * width, uint32_t * length,
		float ndpimagnification)
{
	if (! TIFFGetField(in, TIFFTAG_IMAGEWIDTH, width) ||
	    ! TIFFGetField(in, TIFFTAG_IMAGELENGTH, length)) {
		TIFFError(TIFFFileName(in),
			"Error, impossible to find width or length of image at magnification %f",
			ndpimagnification);
		return -1;
	}

	return 0;
}

static unsigned int
getScannedZonesFromMap(TIFF* in, ScannedZoneBox ** ppboxes)
{
	uint16_t planarconfig, spp, bitspersample;
	uint32_t imagelength, imagewidth, x, y;
	tmsize_t bufsize;
	uint8_t * buf, * p;
	unsigned int numberscannedzones, n;

	assert(! TIFFIsTiled(in));

	(void) TIFFGetField(in, TIFFTAG_PLANARCONFIG, &planarconfig);
	assert(planarconfig == PLANARCONFIG_CONTIG);

	(void) TIFFGetField(in, TIFFTAG_BITSPERSAMPLE, &bitspersample);
	assert(bitspersample == 8);

	(void) TIFFGetField(in, TIFFTAG_IMAGELENGTH, &imagelength);
	(void) TIFFGetField(in, TIFFTAG_IMAGEWIDTH, &imagewidth);
	(void) TIFFGetField(in, TIFFTAG_SAMPLESPERPIXEL, &spp);

	bufsize = TIFFRasterScanlineSize(in) * (tmsize_t) imagelength;
	buf  = (unsigned char *)_TIFFmalloc(bufsize);

	if (!buf) {
		TIFFError(TIFFFileName(in),
			"Error, can't allocate memory buffer of size "
			"%"TIFF_SSIZE_FORMAT " to read map of scanned "
			"zones",
			bufsize);
		return 0;
	}

	if (! readContigStripsIntoBuffer(in, buf, 0, imagelength, bufsize) )
		return 0;

		/* First pass on the map to count the scanned zones. */
	numberscannedzones= 0;
	for (y = 0, p = buf ; y < imagelength ; y++)
		for (x = 0 ; x < imagewidth ; x++, p++) {
			if (*p > numberscannedzones)
				numberscannedzones= *p;
		}

	*ppboxes= _TIFFmalloc(numberscannedzones * sizeof(ScannedZoneBox));
	if (*ppboxes == NULL) {
		TIFFError(TIFFFileName(in),
			"Error, can't allocate memory buffer to store "
			"the limits of scanned zones");
		return 0;
	}

	for (n = 0 ; n < numberscannedzones ; n++)
		(*ppboxes)[n].isempty= 1;

		/* Second pass on the map to find the boxes. */
	for (y = 0, p = buf ; y < imagelength ; y++)
		for (x = 0 ; x < imagewidth ; x++, p++) {
			if (*p) {
				uint32_t n= (*p)-1;
				ScannedZoneBox * p= &((*ppboxes)[n]);
				if (p->isempty) {
					p->isempty= 0;
					p->map_xmin= x;
					p->map_ymin= y;
					p->map_xmax= x;
					p->map_ymax= y;
				} else {
					MIN(p->map_xmin, x);
					MIN(p->map_ymin, y);
					MAX(p->map_xmax, x);
					MAX(p->map_ymax, y);
				}
			}
		}

	return numberscannedzones;
}

static void
findUnitsAtMagnification(TIFF* in, float ndpimagnification, uint32_t* xunit, uint32_t* yunit)
{
	float m;

	if (ndpimagnification > 40) {
		TIFFError(TIFFFileName(in),
			"Error, can't handle magnification larger than 40");
		*xunit= 0; *yunit= 0;
		return;
	}

	*xunit=128; *yunit=256;

	for (m = 40 ; m > ndpimagnification ; m /= 2.) {
		*xunit /= 2; *yunit /= 2;
		if (m < 1e-6) {
			TIFFError(TIFFFileName(in),
				"Error during the computation of x- and y-units");
			*xunit= 0; *yunit= 0;
			return;
		}
	}
}

	/* Allocates memory for a new element at the end of the array
	 * and returns a pointer to that element (or NULL if no memory) */
#define extendArrayOf(nameOfTypeS, type) static type * \
extendArrayOf##nameOfTypeS(type ** array, unsigned * numberofelems, const char * message) \
{ \
	if (*numberofelems == (unsigned) -1) \
		*numberofelems = 1; \
	else \
		(*numberofelems)++; \
	*array = _TIFFrealloc(*array, sizeof(type) * (*numberofelems)); \
	if (*array == NULL) { \
		fprintf(stderr, "Error: insufficient memory for %s.\n", \
			message); \
		return NULL; \
	} \
	return *array + *numberofelems-1; \
}

extendArrayOf(Floats, float)
extendArrayOf(MagnificationDescriptions, MagnificationDescription)
extendArrayOf(Int32s, int32_t)
extendArrayOf(Boxes, BoxToExtract)

#define addToSetOf(nameOfTypeS, type) static int \
addToSetOf##nameOfTypeS(type ** set, unsigned * numberofelems, \
    const char * message, type newelem) \
{ \
	unsigned u; \
	type * p_new; \
	for (u = 0 ; u < *numberofelems ; u++) \
		if (memcmp(&((*set)[u]), &newelem, sizeof(newelem)) == 0) \
			return 0; \
	p_new = extendArrayOf##nameOfTypeS(set, numberofelems, message); \
	if (p_new == NULL) \
		return 1; \
	*p_new = newelem; \
	return 0; \
}

/*addToSetOf(Floats, float)*/
addToSetOf(MagnificationDescriptions, MagnificationDescription)
addToSetOf(Int32s, int32_t)

static int
searchNumberOfDigits(uint32_t u)
{
return snprintf(NULL, 0, TIFF_UINT32_FORMAT, u);
}

static int
buildFileNameForExtract(const char * NDPIfilename,
	float ndpimagnification, int32_t ndpizoffset, const char* label,
	char ** path)
{
	if (label != NULL) {
		my_asprintf(path, "%s_x%g_z" TIFF_INT32_FORMAT "_%s%s",
			NDPIfilename, ndpimagnification, ndpizoffset,
			label, TIFF_SUFFIX);
		return -2;
	} else {
			/* Find the first unused file name */
		unsigned u= 0;
		int fd;

		*path= NULL;
		do {
			if (*path != NULL)
				_TIFFfree(*path);
			u++;
			my_asprintf(path,
				"%s_x%g_z" TIFF_INT32_FORMAT "_%u%s",
				NDPIfilename, ndpimagnification,
				ndpizoffset, u, TIFF_SUFFIX);
			fd= open(*path, O_CREAT|O_EXCL|O_WRONLY,
#if defined(S_IRGRP)
				S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH
#elif defined(S_IREAD)
				S_IREAD|S_IWRITE
#else
				0
#endif
				);

			if (fd < 0 && errno != EEXIST) {
				fprintf(stderr, "Unable to create file \"%s\" -- aborting.\n",
					*path);
			}
		} while(fd == -1);
		return fd;
	}
}

static void
greetings()
{
	fprintf(stderr, "ndpisplit version 1.5-3 license GNU GPL v3 (c) 2011-2021 Christophe Deroulers\n");
	fprintf(stderr, "Please quote \"Diagnostic Pathology 2013, 8:92\" if you use for research\n\n");
	fprintf(stderr, "Splits images from NDPI file(s) into separate TIFF files, one for each magnification and each z level. Optionally splits largest images into mosaics of TIFF or JPEG files and/or subdivides images into scanned zones (removes blank filling added by scanner).\n\n");
}

static void
shortusage()
{
	greetings();
	fprintf(stderr, "usage: ndpisplit [options] file1.ndpi [file2.ndpi...]\n");
	fprintf(stderr, "Use ndipsplit -h to get full details of options.\n");
}

static void
usage(const char* errorMessage, ...)
{
	greetings();
	if (errorMessage) {
		va_list ap;

		fprintf(stderr, "Error: ");
		va_start(ap, errorMessage);
		vfprintf(stderr, errorMessage, ap);
		va_end(ap);
		fprintf(stderr, "\nUse ndipsplit -h to get help on options.\n");
		return;
	}
	fprintf(stderr, "usage: ndpisplit [options] file1.ndpi [file2.ndpi...]\n");
	fprintf(stderr, "where options are:\n");
	fprintf(stderr, " -h        display this help\n");
	fprintf(stderr, " -v        verbose monitoring (-v -v, -vvv... for more messages)\n");
	fprintf(stderr, " -K        print control data under the form Key:value on stdout\n");
	fprintf(stderr, " -TE       report TIFF errors (with dialog boxes under Windows)\n");
	fprintf(stderr, " -s        subdivide image into scanned zones (remove blank filling)\n");
	fprintf(stderr, " -x[m1[,m2...]]  extract only images at the specified magnification(s) m1,...\n");
	fprintf(stderr, " -z[o1[,o2...]]  extract only images at the specified z-offsets o1,...\n");
	fprintf(stderr, " -ex1,y1,W1,L1[,label1][:x2,...]  extract only specified box(es), ignoring -s\n");
	fprintf(stderr, "  xn,yn: relative coordinates of the top left corner of rectangle to extract; real numbers (x=0: left edge of slide, x=1: right edge of slide, y=0: top edge of slide, ...)\n");
	fprintf(stderr, "  Wn,Ln: relative width and length to extract (e.g. W=0.25: one fourth of the width, L=1: whole length)\n");
	fprintf(stderr, "  labeln: optional label that will be part of the name(s) of file(s) where extracted box will be stored\n");
	fprintf(stderr, " -ExM1[,zO1a[,zO1b...]],x1,y1,W1,L1[,label1][:M2,...]  like -e with px units\n");
	fprintf(stderr, "  Mn: magnification to extract (mandatory) -- note the 'x' prefix\n");
	fprintf(stderr, "  Onk: z-offsets to extract -- note the 'z' prefix\n");
	fprintf(stderr, "  xn,yn: absolute coordinates of the top left corner of rectangle to extract in pixels (x=0: left edge of slide, y=0: top edge of slide)\n");
	fprintf(stderr, "  Wn,Ln: absolute width and length to extract in pixels\n");
	fprintf(stderr, " -m[#][c]  make in addition mosaic of largest images\n");
	fprintf(stderr, "  #: memory size limit in MiB on each mosaic piece (default 1024.000; 0 for no limit)\n");
	fprintf(stderr, "  c: compression format of mosaic pieces ('n'one, 'l'zw,\n");
	fprintf(stderr, "       'j'peg in TIFF file (default), 'J'PEG stand-alone file;\n");
	fprintf(stderr, "       j and J may be followed by quality in range 1-100, default = input quality if applicable, 75 if not)\n");
	fprintf(stderr, " -g[w]x[h] width and height in pixels of each piece of the mosaic (overrides memory limit given with -m or -M if both width and height are given; 0 or no value for either dimension means default; default are largest dimensions that satisfy memory limit, divide the full image in equal pieces by powers of 2, and are close to each other)\n");
	fprintf(stderr, " -o#[%%]    overlap amount between adjacent mosaic pieces (in pixels or %%, default 0)\n");
	fprintf(stderr, " -M[#][c]  same as -m but a mosaic is always made (even for small images)\n");
	fprintf(stderr, " -cC  specify the compression format of split images\n");
	fprintf(stderr, "  C: compression format (as for mosaic pieces except that J isn't supported)\n");
	fprintf(stderr, " -p[s[,WxL]]     extract preview image(s) only (image(s) at lowest available magnification, or macroscopic image of the slide), of maximum size / width / length s / W / L pixels (default 1 Mpx for s and no limits on W and L; 0 for any dimension means no limit) and print a few parameters (useful to prepare selection of zones to extract at large magnification)\n\n");

	fprintf(stderr, "Examples: ndpisplit -e0,0.75,0.25,0.25 -m500J60 -o30 to split the lower left quarter of the images inside the NDPI file into separate TIFF files (one for each magnification and each z level), then produce a mosaic from each TIFF file that would require more than 500 MiB of memory to open. Mosaic pieces will require less than 500 MiB to open and be stored into JPEG files with quality level 60. There will be an overlap of 30 pixels between adjacent mosaic pieces.\n");
	fprintf(stderr, "    ndpisplit -Ex40,z-100,z100,1000,0,3000,2000 to extract, from the images at magnification 40x and z-offsets -100 or 100, a rectangle of 3000x2000 pixels with top left corner at position (1000,0).\n");
}

static void
my_asprintf(char** ret, const char* format, ...)
{
	int n;
	char * p;
	va_list ap;

	va_start(ap, format);
	n= vsnprintf(NULL, 0, format, ap);
	va_end(ap);
	p= _TIFFmalloc(n+1);
	if (p == NULL) {
		perror("Insufficient memory for a character string ");
		exit(EXIT_FAILURE);
	}
	va_start(ap, format);
	vsnprintf(p, n+1, format, ap);
	va_end(ap);
	*ret= p;
}

static uint32_t
my_floor(double x)
{
	double f = floor(x);
	return x - f > 0.999 ? f+1 : f;
}

static uint32_t
my_ceil(double x)
{
	double c = ceil(x);
	return c - x > 0.999 ? c-1 : c;
}

static void
stderrErrorHandler(const char* module, const char* fmt, va_list ap)
{
	if (module != NULL)
		fprintf(stderr, "%s: ", module);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ".\n");
}

static void
stderrWarningHandler(const char* module, const char* fmt, va_list ap)
{
	if (module != NULL)
		fprintf(stderr, "%s: ", module);
	fprintf(stderr, "Warning, ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ".\n");
}

/* vim: set ts=8 sts=8 sw=8 noet: */
/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 8
 * fill-column: 78
 * End:
 */
