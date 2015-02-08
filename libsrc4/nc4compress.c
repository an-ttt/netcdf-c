#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef SZIP_FILTER
#include <szlib.h>
#endif
#ifdef BZIP2_FILTER
#include <bzlib.h>
#endif
#ifdef FPZIP_FILTER
#include <fpzip.h>
#endif
#ifdef ZFP_FILTER
#include <zfp.h>
#endif

#include "netcdf.h"
#include "hdf5.h"
#include "nc4compress.h"

#define DEBUG

#ifndef SZIP_FILTER
#define SZ_MAX_PIXELS_PER_BLOCK 0
#endif

/* From hdf5.H5private.h */
#define H5_ASSIGN_OVERFLOW(dst, src, srctype, dsttype)  \
    (dst) = (dsttype)(src);
#define H5_CHECK_OVERFLOW(var, vartype, casttype)

/* From hdf5.H5Fprivate.h */
#  define UINT32DECODE(p, i) {						      \
   (i)	=  (uint32_t)(*(p) & 0xff);	   (p)++;			      \
   (i) |= ((uint32_t)(*(p) & 0xff) <<  8); (p)++;			      \
   (i) |= ((uint32_t)(*(p) & 0xff) << 16); (p)++;			      \
   (i) |= ((uint32_t)(*(p) & 0xff) << 24); (p)++;			      \
}
#  define UINT32ENCODE(p, i) {						      \
   *(p) = (uint8_t)( (i)        & 0xff); (p)++;				      \
   *(p) = (uint8_t)(((i) >>  8) & 0xff); (p)++;				      \
   *(p) = (uint8_t)(((i) >> 16) & 0xff); (p)++;				      \
   *(p) = (uint8_t)(((i) >> 24) & 0xff); (p)++;				      \
}

/* Define an "enum" for all potentially supported compressors;
   The max number of compressors is 127 to fit into signed char.
*/
typedef enum NC_compress_enum {
NC_NOZIP = 0,/*must be 0*/
NC_ZIP   = 1,
NC_SZIP  = 2,
NC_BZIP2 = 3,
NC_FPZIP = 4,
NC_ZFP   = 5,
NC_COMPRESSORS = (NC_ZFP+1)
} NC_compress_enum;

typedef struct NCC_COMPRESSOR {
    NC_compress_enum nccid;
    char name[NC_COMPRESSION_MAX_NAME+1]; /* canonical compressor name */
    int nelems; /* size of the compression parameters */
    H5Z_filter_t h5id;
    int (*_register)(const struct NCC_COMPRESSOR*, H5Z_class2_t*);
    int (*_attach)(const struct NCC_COMPRESSOR*,nc_compression_t*,hid_t);
    int (*_inq)(const struct NCC_COMPRESSOR*,hid_t,int*,unsigned int*,nc_compression_t*);
    int (*_valid)(const struct NCC_COMPRESSOR*, nc_compression_t*);
} NCC_COMPRESSOR;

#define H5Z_FILTER_BZIP2 307
#define H5Z_FILTER_FPZIP 256
#define H5Z_FILTER_ZFP 257

static H5Z_class2_t H5Z_INFO[NC_COMPRESSORS];
static int registered[NC_COMPRESSORS];

static int zip_valid(const NCC_COMPRESSOR*, nc_compression_t*);
static int szip_valid(const NCC_COMPRESSOR*, nc_compression_t*);
static int bzip2_valid(const NCC_COMPRESSOR*, nc_compression_t*);
static int fpzip_valid(const NCC_COMPRESSOR*, nc_compression_t*);
static int zfp_valid(const NCC_COMPRESSOR*, nc_compression_t*);

/*Forward*/
#ifdef SZIP_FILTER
static size_t H5Z_filter_szip(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
#endif
#ifdef BZIP2_FILTER
static size_t H5Z_filter_bzip2(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
#endif
#ifdef FPZIP_FILTER
static size_t H5Z_filter_fpzip(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
#endif
#ifdef ZFP_FILTER
static size_t H5Z_filter_zfp(unsigned,size_t,const unsigned[],size_t,size_t*,void**);
#endif

#ifndef DEBUG
#define THROW(e) (e)
#else
#define THROW(e) checkerr(e,__FILE__,__LINE__)
static int checkerr(int e, const char* file, int line)
{
   if(e != 0) {
     fprintf(stderr, "Error %d in file %s, line %d.\n", e, file, line);
     fflush(stderr);
     abort();
   }
   return e;
}
#endif

/* Forward */
static int available(const NCC_COMPRESSOR* info, H5Z_class2_t*);
static const NCC_COMPRESSOR compressors[NC_COMPRESSORS+1];

#if 0
/* get compressor info by enum */
const NC_compressor_info*
nc_compressor_by_index(NC_compress_enum index)
{
    return (NC_compressor_info*)&compressors[index];
}

NC_compress_enum
nc_compressor_by_name(const char* name)
{
    int e;
    for(e=0;e<NC_COMPRESSORS;e++) {
	if(strcmp(compressors[e].info.name,name) == 0)
	    return (NC_compress_enum)e;
    }
    return NC_NOZIP;
}
#endif

/*
Turn on compression for a variable's plist
*/
int
nc_compress_set(const char*algorithm, hid_t plistid, int nparams, unsigned int* parms)
{
    const NCC_COMPRESSOR* cmp;
    nc_compression_t* uparams = (nc_compression_t*)parms;
    for(cmp=compressors;cmp->nccid;cmp++) {
        if(strcmp(cmp->name,algorithm)==0) {
	    if(!registered[cmp->nccid]) return THROW(NC_ECOMPRESS);
            if(cmp->_attach(cmp,uparams,plistid) != NC_NOERR)
                return THROW(NC_ECOMPRESS);
            return THROW(NC_NOERR);
        }
    }
    return THROW(NC_ECOMPRESS);
}

/* 
Register all known filters with the library
*/
int
nc_compress_register_all(void)
{
    const NCC_COMPRESSOR* cmp;
    memset(H5Z_INFO,0,sizeof(H5Z_INFO));
    for(cmp=compressors;cmp->nccid;cmp++) {
        H5Z_class2_t* h5info = &H5Z_INFO[cmp->nccid];
	h5info->version = H5Z_CLASS_T_VERS;
	h5info->encoder_present = 1;
	h5info->decoder_present = 1;
	h5info->can_apply = NULL;
	h5info->set_local = NULL;
	h5info->name = cmp->name;
	h5info->id = cmp->h5id;
	if(cmp->_register != NULL) {
	    int stat = cmp->_register(cmp,h5info);
	    if(stat != NC_NOERR)
		return stat;
            if(available(cmp,h5info) != NC_NOERR)
                return THROW(NC_ECOMPRESS);
	}
    }
    return THROW(NC_NOERR);
}

const char*
nc_compress_name_for(int id)
{
    const NCC_COMPRESSOR* cmp;
    for(cmp=compressors;cmp->nccid;cmp++) {
        if(cmp->h5id == id)
            return cmp->name;
    }
    return NULL;
}

#if 0
static H5Z_filter_t
nc_compress_id_for(const char* name)
{
    const NCC_COMPRESSOR* cmp;
    for(cmp=compressors;cmp->nccid;cmp++) {
        if(strcmp(cmp->name,name)==0)
            return cmp->h5info->id;
    }
    return 0;
}
#endif

int
nc_compress_inq_parameters(const char* algorithm,
                          hid_t propid, /* surrogate for the variable */
                          int argc, /*in*/
                          unsigned int* argv, /*in*/
			  char name[NC_COMPRESSION_MAX_NAME], /*out*/
                          int* nparamsp, /*out*/
                          unsigned int* params) /*out*/
{
    const NCC_COMPRESSOR* cmp;
    nc_compression_t* uparams = (nc_compression_t*)params;
    if(name == NULL || nparamsp == NULL || params == NULL)
	return NC_EINVAL;
    for(cmp=compressors;cmp->nccid;cmp++) {
        if(strcmp(cmp->name,algorithm) == 0) {
	    if(!registered[cmp->nccid])
		return NC_ECOMPRESS;
	    if(argc < cmp->nelems)
		return THROW(NC_EINVAL);
	    *nparamsp = argc;
            if(cmp->_inq(cmp,propid,nparamsp,argv,uparams) != NC_NOERR)
                return THROW(NC_ECOMPRESS);
            strncpy(name,cmp->name,NC_COMPRESSION_MAX_NAME);
            return THROW(NC_NOERR);
        }
    }
    return THROW(NC_ECOMPRESS);
}

int
nc_compress_validate(const char* algorithm,
                          int nparams,
                          unsigned int* params)
{
    const NCC_COMPRESSOR* cmp;
    nc_compression_t* uparams = (nc_compression_t*)params;
    if(algorithm == NULL || params == NULL)
	return NC_EINVAL;
    for(cmp=compressors;cmp->nccid;cmp++) {
        if(strcmp(cmp->name,algorithm) == 0) {
	    if(!registered[cmp->nccid])
		return THROW(NC_ECOMPRESS);
	    if(nparams < cmp->nelems)
		return THROW(NC_EINVAL);
            if(cmp->_valid(cmp,uparams) != NC_NOERR)
                return THROW(NC_ECOMPRESS);
            return THROW(NC_NOERR);
        }
    }
    return THROW(NC_ECOMPRESS);
}

/*
* Check if compression is available and can be used for both
* compression and decompression.  Normally we do not perform error
* checking in these examples for the sake of clarity, but in this
* case we will make an exception because this filter is an
* optional part of the hdf5 library.
*/
static int
available(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    htri_t avail;
    unsigned int filter_info;

    if(registered[info->nccid]) {
        avail = H5Zfilter_avail(h5info->id);
        if(!avail) {
            fprintf(stderr,"Filter not available: %s.\n",info->name);
            return THROW(NC_ECOMPRESS);
	}
        if(H5Zget_filter_info(h5info->id, &filter_info))
	    return THROW(NC_ECOMPRESS);
        if(!(filter_info & H5Z_FILTER_CONFIG_ENCODE_ENABLED)
           || !(filter_info & H5Z_FILTER_CONFIG_DECODE_ENABLED) ) {
           fprintf(stderr,"Filter not available for encoding and decoding: %s.\n",info->name);
            return THROW(NC_ECOMPRESS);
	}
    }
    return THROW(NC_NOERR);
}

/**
Generic inquiry function
*/
static int
generic_inq(const NCC_COMPRESSOR* info,
	    hid_t propid,
	    int* argc,
            unsigned int* argv,
	    nc_compression_t* parms)
{
    int i;
    if(!registered[info->nccid])
	return NC_ECOMPRESS;
    if(*argc < info->nelems)
       return THROW(NC_EINVAL);
    *argc = info->nelems;
    for(i=0;i<*argc;i++)
        parms->params[i] = argv[i];
    return THROW(NC_NOERR);
}

/**************************************************/
/*#ifdef ZIP (DEFLATE) compression always defined */

static int
zip_register(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    registered[NC_ZIP] = 1;
    return THROW(NC_NOERR); /* no-op */
}

static int
zip_attach(const NCC_COMPRESSOR* info, nc_compression_t* parms, hid_t plistid)
{
    int status = zip_valid(info,parms);
    if(status == NC_NOERR) {
	if(H5Pset_deflate(plistid, parms->zip.level))
	    status = NC_ECOMPRESS;
    }
    return THROW(status);
}

static int
zip_valid(const NCC_COMPRESSOR* info, nc_compression_t* parms)
{
    int status = NC_NOERR;
    /* validate level */
    if(parms->zip.level < NC_DEFLATE_LEVEL_MIN ||
	parms->zip.level > NC_DEFLATE_LEVEL_MAX)
	status =  NC_EINVAL;
    return THROW(status);
}

/**************************************************/

static int
szip_register(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    herr_t status = NC_NOERR;
#ifdef SZIP_FILTER
    /* See if already in the hdf5 library */
    int avail = H5Zfilter_avail(H5Z_FILTER_SZIP);
    if(avail) {
        registered[info->nccid] = (avail ? 1 : 0);
    } else {
        /* finish the H5Z_class2_t instance */
        h5info->filter = (H5Z_func_t)H5Z_filter_szip;
        status = H5Zregister(h5info);
        if(status == 0) registered[info->nccid] = 1;
    }
#endif
    return THROW((status ? NC_ECOMPRESS : NC_NOERR));
}

static int
szip_attach(const NCC_COMPRESSOR* info, nc_compression_t* parms, hid_t plistid)
{
    int status = NC_NOERR;
    if(!registered[info->nccid])
    status = szip_valid(info,parms);
    if(status != NC_NOERR) goto done;
    /* See if already in the hdf5 library */
    int avail = H5Zfilter_avail(H5Z_FILTER_SZIP);
    if(avail) {
       	if(H5Pset_szip(plistid, parms->szip.options_mask, parms->szip.pixels_per_block))
	    status = NC_ECOMPRESS;
    } else if(H5Pset_filter(plistid, info->h5id, H5Z_FLAG_MANDATORY, (size_t)NC_NELEMS_SZIP,parms->params))
	status = NC_ECOMPRESS;
done:
    return THROW(status);
}

static int
szip_valid(const NCC_COMPRESSOR* info, nc_compression_t* parms)
{
    int status = NC_NOERR;
    /* validate bpp */
    if(parms->szip.pixels_per_block > SZ_MAX_PIXELS_PER_BLOCK)
	status = NC_EINVAL;
    return THROW(status);
}

#if 0
static int
szip_inq(const NCC_COMPRESSOR* info, hid_t propid, int* argc, unsigned int* argv, nc_compression_t* parms)
{
#ifdef SZIP_FILTER
    if(!registered[info->nccid]) return NC_ECOMPRESS;
#endif
   if(*argc < NC_NELEMS_SZIP)
       return THROW(NC_EINVAL);
   *argc = NC_NELEMS_SZIP;
    parms->szip.options_mask = argv[0];
    parms->szip.pixels_per_block = argv[1];
    return THROW(NC_NOERR);
}
#endif

#ifdef SZIP_FILTER
static size_t
H5Z_filter_szip (unsigned flags, size_t cd_nelmts, const unsigned cd_values[],
    size_t nbytes, size_t *buf_size, void **buf)
{
    size_t ret_value = 0;       /* Return value */
    size_t size_out  = 0;       /* Size of output buffer */
    unsigned char *outbuf = NULL;    /* Pointer to new output buffer */
    unsigned char *newbuf = NULL;    /* Pointer to input buffer */
    SZ_com_t sz_param;          /* szip parameter block */

    /* Sanity check to make certain that we haven't drifted out of date with
     * the mask options from the szlib.h header */
    assert(H5_SZIP_ALLOW_K13_OPTION_MASK==SZ_ALLOW_K13_OPTION_MASK);
    assert(H5_SZIP_CHIP_OPTION_MASK==SZ_CHIP_OPTION_MASK);
    assert(H5_SZIP_EC_OPTION_MASK==SZ_EC_OPTION_MASK);
#if 0 /* not defined in our szlib.h */
    assert(H5_SZIP_LSB_OPTION_MASK==SZ_LSB_OPTION_MASK);
    assert(H5_SZIP_MSB_OPTION_MASK==SZ_MSB_OPTION_MASK);
    assert(H5_SZIP_RAW_OPTION_MASK==SZ_RAW_OPTION_MASK);
#endif
    assert(H5_SZIP_NN_OPTION_MASK==SZ_NN_OPTION_MASK);

    /* Check arguments */
    if (cd_nelmts!=4) {
	fprintf(stderr,"szip: invalid deflate aggression level\n");
	ret_value = 0;
	goto done;
    }

    /* Copy the filter parameters into the szip parameter block */
    H5_ASSIGN_OVERFLOW(sz_param.options_mask,cd_values[H5Z_SZIP_PARM_MASK],unsigned,int);
    H5_ASSIGN_OVERFLOW(sz_param.bits_per_pixel,cd_values[H5Z_SZIP_PARM_BPP],unsigned,int);
    H5_ASSIGN_OVERFLOW(sz_param.pixels_per_block,cd_values[H5Z_SZIP_PARM_PPB],unsigned,int);
    H5_ASSIGN_OVERFLOW(sz_param.pixels_per_scanline,cd_values[H5Z_SZIP_PARM_PPS],unsigned,int);

    /* Input; uncompress */
    if (flags & H5Z_FLAG_REVERSE) {
        uint32_t stored_nalloc;  /* Number of bytes the compressed block will expand into */
        size_t nalloc;  /* Number of bytes the compressed block will expand into */

        /* Get the size of the uncompressed buffer */
        newbuf = *buf;
        UINT32DECODE(newbuf,stored_nalloc);
        H5_ASSIGN_OVERFLOW(nalloc,stored_nalloc,uint32_t,size_t);

        /* Allocate space for the uncompressed buffer */
        if(NULL==(outbuf = malloc(nalloc))) {
	    fprintf(stderr,"szip: memory allocation failed for szip decompression\n");
	    ret_value = 0;
	    goto done;
	}
        /* Decompress the buffer */
        size_out=nalloc;
        if(SZ_BufftoBuffDecompress(outbuf, &size_out, newbuf, nbytes-4, &sz_param) != SZ_OK) {
	    fprintf(stderr,"szip: szip_filter: decompression failed\n");
	    ret_value = 0;
	    goto done;
	}
        assert(size_out==nalloc);

        /* Free the input buffer */
        if(*buf) free(*buf);

        /* Set return values */
        *buf = outbuf;
        outbuf = NULL;
        *buf_size = nalloc;
        ret_value = nalloc;
    }
    /* Output; compress */
    else {
        unsigned char *dst = NULL;    /* Temporary pointer to new output buffer */

        /* Allocate space for the compressed buffer & header (assume data won't get bigger) */
        if(NULL==(dst=outbuf = malloc(nbytes+4))) {
	    fprintf(stderr,"szip: unable to allocate szip destination buffer\n");
	    ret_value = 0;
	    goto done;
	}
        /* Encode the uncompressed length */
        H5_CHECK_OVERFLOW(nbytes,size_t,uint32_t);
        UINT32ENCODE(dst,nbytes);

        /* Compress the buffer */
        size_out = nbytes;
        if(SZ_OK!= SZ_BufftoBuffCompress(dst, &size_out, *buf, nbytes, &sz_param)) {
	    fprintf(stderr,"szip: overflow\n");
	    ret_value = 0;
	    goto done;
	}
        assert(size_out<=nbytes);

        /* Free the input buffer */
        if(*buf) free(*buf);

        /* Set return values */
        *buf = outbuf;
        outbuf = NULL;
        *buf_size = size_out+4;
        ret_value = size_out+4;
    }
done:
    if(outbuf)
        free(outbuf);
    return ret_value;
}
#endif /*SZIP_FILTER*/

/**************************************************/

static int
bzip2_register(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    herr_t status = 0;
#ifdef BZIP2_FILTER
    /* finish the H5Z_class2_t instance */
    h5info->filter = (H5Z_func_t)H5Z_filter_bzip2;
    status = H5Zregister(h5info);
    if(status == 0) registered[info->nccid] = 1;
#endif
    return THROW((status ? NC_ECOMPRESS : NC_NOERR));
};

static int
bzip2_attach(const NCC_COMPRESSOR* info, nc_compression_t* parms, hid_t plistid)
{
    int status = NC_NOERR;
    if(!registered[info->nccid]) {status = NC_ECOMPRESS; goto done;}
    if((status = bzip2_valid(info,parms)) != NC_NOERR) goto done;
    if(H5Pset_filter(plistid, info->h5id, H5Z_FLAG_MANDATORY, (size_t)NC_NELEMS_BZIP2,parms->params))
	status = NC_ECOMPRESS;
done:
    return THROW(status);
}

static int
bzip2_valid(const NCC_COMPRESSOR* info, nc_compression_t* parms)
{
    int status = NC_NOERR;
    if(parms->bzip2.level < NC_DEFLATE_LEVEL_MIN ||
       parms->bzip2.level > NC_DEFLATE_LEVEL_MAX)
	status = NC_EINVAL;
    return THROW(status);
}

#ifdef BZIP2_FILTER
static size_t
H5Z_filter_bzip2(unsigned int flags, size_t cd_nelmts,
                     const unsigned int argv[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
    char *outbuf = NULL;
    size_t outbuflen, outdatalen;
    int ret;
  
    if(nbytes == 0) return 0; /* sanity check */

    if(flags & H5Z_FLAG_REVERSE) {
  
	/** Decompress data.
         **
         ** This process is troublesome since the size of uncompressed data
         ** is unknown, so the low-level interface must be used.
         ** Data is decompressed to the output buffer (which is sized
         ** for the average case); if it gets full, its size is doubled
         ** and decompression continues.  This avoids repeatedly trying to
         ** decompress the whole block, which could be really inefficient.
         **/
  
	bz_stream stream;
	char *newbuf = NULL;
	size_t newbuflen;
  
        /* Prepare the output buffer. */
        outbuflen = nbytes * 3 + 1;/* average bzip2 compression ratio is 3:1 */
        outbuf = malloc(outbuflen);
	if(outbuf == NULL) {
	    fprintf(stderr,"memory allocation failed for bzip2 decompression\n");
	    goto cleanupAndFail;
	}
        /* Use standard malloc()/free() for internal memory handling. */
        stream.bzalloc = NULL;
        stream.bzfree = NULL;
        stream.opaque = NULL;

        /* Start decompression. */
        ret = BZ2_bzDecompressInit(&stream, 0, 0);
        if(ret != BZ_OK) {
            fprintf(stderr, "bzip2 decompression start failed with error %d\n", ret);
            goto cleanupAndFail;
	}

        /* Feed data to the decompression process and get decompressed data. */
        stream.next_out = outbuf;
        stream.avail_out = outbuflen;
        stream.next_in = *buf;
        stream.avail_in = nbytes;
        do {
	    ret = BZ2_bzDecompress(&stream);
            if(ret < 0) {
                fprintf(stderr, "BUG: bzip2 decompression failed with error %d\n", ret);
                goto cleanupAndFail;
            }
            if(ret != BZ_STREAM_END && stream.avail_out == 0) {
                /* Grow the output buffer. */
                newbuflen = outbuflen * 2;
                newbuf = realloc(outbuf, newbuflen);
                if(newbuf == NULL) {
                    fprintf(stderr, "memory allocation failed for bzip2 decompression\n");
                    goto cleanupAndFail;
                }
                stream.next_out = newbuf + outbuflen;  /* half the new buffer behind */
                stream.avail_out = outbuflen;  /* half the new buffer ahead */
                outbuf = newbuf;
                outbuflen = newbuflen;
            }
        } while (ret != BZ_STREAM_END);

        /* End compression. */
        outdatalen = stream.total_out_lo32;
        ret = BZ2_bzDecompressEnd(&stream);
        if(ret != BZ_OK) {
            fprintf(stderr, "bzip2 compression end failed with error %d\n", ret);
            goto cleanupAndFail;
        }
    } else {

	/** Compress data.
         **
         ** This is quite simple, since the size of compressed data in the worst
         ** case is known and it is not much bigger than the size of uncompressed
         ** data.  This allows us to use the simplified one-shot interface to
         ** compression.
         **/
   
	unsigned int odatalen;  /* maybe not the same size as outdatalen */
        int blockSize100k = 9;
   
        /* Get compression block size if present. */
	if(cd_nelmts > 0) {
            blockSize100k = argv[0];
	    if(blockSize100k < 1 || blockSize100k > 9) {
		fprintf(stderr, "invalid compression block size: %d\n", blockSize100k);
                goto cleanupAndFail;
	    }
        }
    
        /* Prepare the output buffer. */
        outbuflen = nbytes + nbytes / 100 + 600;  /* worst case (bzip2 docs) */
        outbuf = malloc(outbuflen);
        if(outbuf == NULL) {
	    fprintf(stderr, "memory allocation failed for bzip2 compression\n");
            goto cleanupAndFail;
        }
    
        /* Compress data. */
        odatalen = outbuflen;
        ret = BZ2_bzBuffToBuffCompress(outbuf, &odatalen, *buf, nbytes,
                                       blockSize100k, 0, 0);
        outdatalen = odatalen;
        if(ret != BZ_OK) {
	    fprintf(stderr, "bzip2 compression failed with error %d\n", ret);
            goto cleanupAndFail;
        }
    }

    /* Always replace the input buffer with the output buffer. */
    free(*buf);
    *buf = outbuf;
    *buf_size = outbuflen;
    return outdatalen;
    
cleanupAndFail:
    if(outbuf)
        free(outbuf);
    return 0;
}
#endif    
    
/**************************************************/
    
static int
fpzip_register(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    herr_t status = 0;
#ifdef FPZIP_FILTER
    /* finish the H5Z_class2_t instance */
    h5info->filter = (H5Z_func_t)H5Z_filter_fpzip;
    status = H5Zregister(h5info);
    registered[info->nccid] = (status ? 0 : 1);
#endif
    return THROW((status ? NC_ECOMPRESS : NC_NOERR));
}
    
static int
fpzip_attach(const NCC_COMPRESSOR* info, nc_compression_t* parms, hid_t plistid)
{
    int status = NC_NOERR;
    if(!registered[info->nccid]) {status = NC_ECOMPRESS; goto done;}
    if((status = fpzip_valid(info,parms)) != NC_NOERR) goto done;
    if(H5Pset_filter(plistid,info->h5id,H5Z_FLAG_MANDATORY,NC_NELEMS_ZFP,parms->params))
	status = NC_ECOMPRESS;
done:
    return THROW(status); 
}
    
static int
fpzip_valid(const NCC_COMPRESSOR* info, nc_compression_t* parms)
{
    int status = NC_NOERR;
    char c = (char)parms->fpzip.ndimalg;
    if(parms->fpzip.prec < 0 || parms->fpzip.prec > 64) {status = NC_EINVAL; goto done;}
    if(parms->fpzip.prec > 32 && !parms->fpzip.isdouble) {status = NC_EINVAL; goto done;}
    if(parms->fpzip.rank < 0 || parms->fpzip.rank > NC_COMPRESSION_MAX_DIMS) {status = NC_EINVAL; goto done;}
    if(c!='p' && c != 'c' && c != 's') {status = NC_EINVAL; goto done;}
done:
    return THROW(status);
}

#ifdef FPZIP_FILTER
/**
Assumptions:
1. Each incoming block represents 1 complete chunk
2. If "choose" is enabled, then only 3 chunks can have
   value different from 1 (one).
*/
static size_t
H5Z_filter_fpzip(unsigned int flags, size_t cd_nelmts,
                     const unsigned int argv[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
    int i;
    FPZ* fpz;
    nc_compression_t* params;
    int rank;
    int isdouble;
    int prec;
    size_t outbuflen;
    char *outbuf = NULL;
    size_t inbytes;
    size_t elemsize;
    size_t bufbytes;
    size_t totalsize;
    size_t chunksizes[NC_MAX_VAR_DIMS];
    int nx,ny,nz,nf;
    size_t nzsize;
    int ndimalg;

    if(nbytes == 0) return 0; /* sanity check */

    params = (nc_compression_t*)argv;
    isdouble = params->fpzip.isdouble;
    prec = params->fpzip.prec;
    rank = params->fpzip.rank;
    ndimalg = (int)params->fpzip.ndimalg;	

    for(totalsize=1,i=0;i<rank;i++) {
	chunksizes[i] = params->fpzip.chunksizes[i];
	totalsize *= chunksizes[i];
    }

    switch (ndimalg) {
    case NC_NDIM_CHOOSE:
        nx = ny = nz = nf = 1;
        for(i=0;i<rank;i++) {
	    if(chunksizes[i] > 1) {
  	        if(nx == 1) nx = chunksizes[i];
	        else if(ny == 1) ny = chunksizes[i];
	        else if(nz == 1) nz = chunksizes[i];
		else {
	  	    fprintf(stderr,"At most, 3 fpzip chunksizes can be > 1\n");
		    return NC_ECOMPRESS;
		}
	    }
	}
	break;
    case NC_NDIM_PREFIX:
        /* Do some computations */
        nzsize = 0;
        if(rank > 2) {
            for(nzsize=1,i=2;i<rank;i++)
	        nzsize *= chunksizes[i];
	}
	break;
    case NC_NDIM_SUFFIX:
	break;
    }

    /* Element size (in bytes) */
    elemsize = (isdouble ? sizeof(double) : sizeof(float));

    /* Number of array bytes */
    inbytes = totalsize * elemsize;

    /* Allocated size of the new buffer;
       used for both decompression and compression */
    bufbytes = 1024 + inbytes; /* why the 1024? */

    /* precision */
    if(prec == 0)
        prec = CHAR_BIT * elemsize;

    if(flags & H5Z_FLAG_REVERSE) {
        /** Decompress data.
         **/

	/* Tell fpzip where to get the compressed data */
        fpz = fpzip_read_from_buffer(*buf);
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        fpz->type = isdouble ? FPZIP_TYPE_DOUBLE : FPZIP_TYPE_FLOAT;
	fpz->prec = prec;

	switch (ndimalg) {
	case NC_NDIM_CHOOSE:
	    fpz->nx = nz;
	    fpz->ny = ny;
	    fpz->nz = nz;
	    fpz->nf = nf;
	    break;
	case NC_NDIM_PREFIX:
	    fpz->nx = chunksizes[0];
	    fpz->ny = (rank >= 2 ? chunksizes[1] : 1);
	    fpz->nz = (rank >= 3 ? nzsize : 1);
	    fpz->nf = 1;
	    break;
	case NC_NDIM_SUFFIX:
	    break;
	}

        /* Create the decompressed data buffer */
	outbuf = (char*)malloc(bufbytes);

        /* Decompress into the compressed data buffer */
        outbuflen = fpzip_read(fpz,outbuf);

        if(fpzip_errno == fpzipSuccess && outbuflen == 0)
            fpzip_errno = fpzipErrorReadStream;

        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        fpzip_read_close(fpz);
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        /* Replace the buffer given to us with our decompressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = bufbytes;
        outbuf = NULL;
        return outbuflen; /* # valid bytes */

    } else {
  
        /** Compress data.
         **/

        /* Create the compressed data buffer */
        outbuf = (char*)malloc(bufbytes); /* overkill */

        /* Compress into the decompressed data buffer */
        fpz = fpzip_write_to_buffer(outbuf,bufbytes);
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        fpz->type = isdouble ? FPZIP_TYPE_DOUBLE : FPZIP_TYPE_FLOAT;
	fpz->prec = prec;

	switch (ndimalg) {
	case NC_NDIM_CHOOSE:
	    fpz->nx = nz;
	    fpz->ny = ny;
	    fpz->nz = nz;
	    fpz->nf = nf;
	    break;
	case NC_NDIM_PREFIX:
	    fpz->nx = chunksizes[0];
	    fpz->ny = (rank >= 2 ? chunksizes[1] : 1);
	    fpz->nz = (rank >= 3 ? nzsize : 1);
	    fpz->nf = 1;
	    break;
	case NC_NDIM_SUFFIX:
	    break;
	}

        /* Compress to the compressed data buffer from decompressed data in *buf*/
        outbuflen = fpzip_write(fpz,*buf);

        if(outbuflen == 0 && fpzip_errno  == fpzipSuccess)
            fpzip_errno = fpzipErrorWriteStream;
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        fpzip_write_close(fpz);
        if(fpzip_errno != fpzipSuccess)
	    goto cleanupAndFail;

        /* Replace the buffer given to us with our decompressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = bufbytes;
        outbuf = NULL;
        return outbuflen; /* # valid bytes */
    }

cleanupAndFail:
    if(outbuf)
        free(outbuf);
    if(fpzip_errno != fpzipSuccess) {
	fprintf(stderr,"fpzip error: %s\n",fpzip_errstr[fpzip_errno]);
        fflush(stderr);
    }
    return 0;
}
#endif    

/**************************************************/

static int
zfp_register(const NCC_COMPRESSOR* info, H5Z_class2_t* h5info)
{
    herr_t status = 0;
#ifdef ZFP_FILTER
    /* finish the H5Z_class2_t instance */
    h5info->filter = (H5Z_func_t)H5Z_filter_zfp;
    status = H5Zregister(h5info);
    registered[info->nccid] = (status ? 0 : 1);
#endif
    return THROW((status ? NC_ECOMPRESS : NC_NOERR));
}
    
static int
zfp_attach(const NCC_COMPRESSOR* info, nc_compression_t* parms, hid_t plistid)
{
    int status = NC_NOERR;
    if(!registered[info->nccid]) {status = NC_ECOMPRESS; goto done;}
    if((status = zfp_valid(info,parms)) != NC_NOERR) goto done;
    if(H5Pset_filter(plistid,info->h5id,H5Z_FLAG_MANDATORY,NC_NELEMS_ZFP,parms->params))
	status = NC_ECOMPRESS;
done:
    return THROW(status);
}
    
static int
zfp_valid(const NCC_COMPRESSOR* info, nc_compression_t* parms)
{
    int status = NC_NOERR;
    char c = (char)parms->zfp.ndimalg;
    if(parms->zfp.prec < 0 || parms->zfp.prec > 64) {status = NC_EINVAL; goto done;}
    if(parms->zfp.prec > 32 && !parms->zfp.isdouble) {status = NC_EINVAL; goto done;}
    if(parms->zfp.rank < 0 || parms->zfp.rank > NC_COMPRESSION_MAX_DIMS) {status = NC_EINVAL; goto done;}
    if(c != 'p' && c != 'c' && c != 's') {status = NC_EINVAL; goto done;}
done:
    return THROW(status);
}

#ifdef ZFP_FILTER
/**
Assumptions:
1. Each incoming block represents 1 complete chunk
*/
static size_t
H5Z_filter_zfp(unsigned int flags, size_t cd_nelmts,
                     const unsigned int argv[], size_t nbytes,
                     size_t *buf_size, void **buf)
{
    int i;
    zfp_params zfp;
    nc_compression_t* params;
    int rank;
    int isdouble;
    unsigned int prec;
    double rate;
    double accuracy;
    size_t outbuflen;
    char *outbuf = NULL;
    size_t inbytes;
    size_t bufbytes;
    size_t elemsize;
    size_t totalsize;
    size_t chunksizes[NC_MAX_VAR_DIMS];
    int nx,ny,nz;
    size_t nzsize;
    int ndimalg;
    
    if(nbytes == 0) return 0; /* sanity check */

    params = (nc_compression_t*)argv;
    isdouble = params->zfp.isdouble;
    prec = params->zfp.prec;
    rank = params->zfp.rank;
    rate = params->zfp.rate;
    accuracy = params->zfp.tolerance;
    ndimalg = params->zfp.ndimalg;

    for(totalsize=1,i=0;i<rank;i++) {
	chunksizes[i] = params->zfp.chunksizes[i];
	totalsize *= chunksizes[i];
    }

    switch (ndimalg) {
    case NC_NDIM_CHOOSE:
        nx = ny = nz = 1;
        for(i=0;i<rank;i++) {
            if(chunksizes[i] > 1) {
                if(nx == 1) nx = chunksizes[i];
                else if(ny == 1) ny = chunksizes[i];
                else if(nz == 1) nz = chunksizes[i];
		else {
	  	    fprintf(stderr,"At most, 3 zfp chunksizes can be > 1\n");
		    return NC_ECOMPRESS;
		}
            }
        }
	break;
    case NC_NDIM_PREFIX:
        /* Do some computations */
        nzsize = 0;
        if(rank > 2) {
            for(nzsize=1,i=2;i<rank;i++) {
                nzsize *= chunksizes[i];
            }
        }
	break;
    case NC_NDIM_SUFFIX:
	break;
    }

    /* Element size (in bytes) */
    elemsize = (isdouble ? sizeof(double) : sizeof(float));

    if(flags & H5Z_FLAG_REVERSE) {
        /** Decompress data.
         **/

        /* Number of uncompressed bytes */
        inbytes = totalsize * elemsize;

        /* Allocated size of the target buffer */
        bufbytes = 1024 + inbytes; /* see fpzip? */

	switch (ndimalg) {
	case NC_NDIM_CHOOSE:
	    zfp.nx = nz;
	    zfp.ny = ny;
	    zfp.nz = nz;
	    break;
	case NC_NDIM_PREFIX:
	    zfp.nx = chunksizes[0];
	    zfp.ny = (rank >= 2 ? chunksizes[1] : 1);
	    zfp.nz = (rank >= 3 ? nzsize : 1);
	    break;
	case NC_NDIM_SUFFIX:
	    break;
	}

        zfp.type = isdouble ? ZFP_TYPE_DOUBLE : ZFP_TYPE_FLOAT;

        zfp_set_precision(&zfp,(unsigned int)prec);
        if(rate != 0)
            zfp_set_rate(&zfp,rate);
        if(accuracy != 0)
            zfp_set_accuracy(&zfp,accuracy);

        /* Create the decompressed data buffer */
        outbuf = (char*)malloc(bufbytes);

        /* Decompress into the compressed data buffer */
        outbuflen = zfp_decompress(&zfp,outbuf,*buf,nbytes);
        if(outbuflen == 0)
            goto cleanupAndFail;

        /* Replace the buffer given to us with our decompressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = bufbytes;
        outbuf = NULL;
        return outbuflen; /* # valid bytes */

    } else {
  
        /** Compress data.
         **/

        /* fill in zfp */
	switch (ndimalg) {
	case NC_NDIM_CHOOSE:
	    zfp.nx = nz;
	    zfp.ny = ny;
	    zfp.nz = nz;
	    break;
	case NC_NDIM_PREFIX:
	    zfp.nx = chunksizes[0];
	    zfp.ny = (rank >= 2 ? chunksizes[1] : 1);
	    zfp.nz = (rank >= 3 ? nzsize : 1);
	    break;
	case NC_NDIM_SUFFIX:
	    break;
	}

        zfp.type = isdouble ? ZFP_TYPE_DOUBLE : ZFP_TYPE_FLOAT;

        zfp_set_precision(&zfp,(unsigned int)prec);
        if(rate != 0)
            zfp_set_rate(&zfp,rate);
        if(accuracy != 0)
            zfp_set_accuracy(&zfp,accuracy);

        /* Create the compressed data buffer */
        bufbytes = zfp_estimate_compressed_size(&zfp);
        outbuf = (char*)malloc(bufbytes);

        /* Compress into the compressed data buffer */
        outbuflen = zfp_compress(&zfp,*buf,outbuf,bufbytes);
        if(outbuflen == 0)
            goto cleanupAndFail;

        /* Replace the buffer given to us with our decompressed data buffer */
        free(*buf);
        *buf = outbuf;
        *buf_size = bufbytes;
        outbuf = NULL;
        return outbuflen; /* # valid bytes */
    }

cleanupAndFail:
    if(outbuf)
        free(outbuf);
    return 0;
}
#endif    

/**************************************************/

/* Provide access to all the compressors */
static const NCC_COMPRESSOR compressors[NC_COMPRESSORS+1] = {
    {NC_ZIP, "zip", NC_NELEMS_ZIP, H5Z_FILTER_DEFLATE, zip_register, zip_attach, generic_inq, zip_valid},
    {NC_SZIP, "szip", NC_NELEMS_SZIP, H5Z_FILTER_SZIP, szip_register, szip_attach, generic_inq, szip_valid},
    {NC_BZIP2, "bzip2", NC_NELEMS_BZIP2, H5Z_FILTER_BZIP2, bzip2_register, bzip2_attach, generic_inq, bzip2_valid},
    {NC_FPZIP, "fpzip", NC_NELEMS_FPZIP, H5Z_FILTER_FPZIP, fpzip_register, fpzip_attach, generic_inq, fpzip_valid},
    {NC_ZFP, "zfp", NC_NELEMS_ZFP, H5Z_FILTER_ZFP, zfp_register, zfp_attach, generic_inq, zfp_valid},
    {NC_NOZIP, "\0", 0, 0, NULL, NULL, NULL, NULL} /* must be last */
};