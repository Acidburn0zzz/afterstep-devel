/*
 * Copyright (c) 2000,2001 Sasha Vasko <sasha at aftercode.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#undef LOCAL_DEBUG
#undef DO_CLOCKING
#ifndef NO_DEBUG_OUTPUT
#undef DEBUG_RECTS
#undef DEBUG_RECTS2
#endif

#ifdef _WIN32
#include "win32/config.h"
#else
#include "config.h"
#endif


#define USE_64BIT_FPU

#include <string.h>
#ifdef DO_CLOCKING
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#ifdef _WIN32
# include "win32/afterbase.h"
#else
# include "afterbase.h"
#endif
#include "asvisual.h"
#include "blender.h"
#include "asimage.h"

static ASVisual __as_dummy_asvisual = {0};
static ASVisual *__as_default_asvisual = &__as_dummy_asvisual ;

/* these are internal library things - user should not mess with it ! */
ASVisual *_set_default_asvisual( ASVisual *new_v )
{
	ASVisual *old_v = __as_default_asvisual ;
	__as_default_asvisual = new_v?new_v:&__as_dummy_asvisual ;
	return old_v;
}

ASVisual *get_default_asvisual()
{
	return __as_default_asvisual?__as_default_asvisual:&__as_dummy_asvisual;
}

/* internal buffer used for compression/decompression */

static CARD8 *__as_compression_buffer = NULL ;
static size_t __as_compression_buffer_len = 0;   /* allocated size */

static inline CARD8* get_compression_buffer( size_t size )
{
	if( size > __as_compression_buffer_len )
 		__as_compression_buffer_len = (size+1023)&(~0x03FF) ;

	return (__as_compression_buffer = realloc( __as_compression_buffer, __as_compression_buffer_len ));
}

static inline void release_compression_buffer( CARD8 *ptr )
{
	/* do nothing so far */
}



#ifdef TRACK_ASIMAGES
static ASHashTable *__as_image_registry = NULL ;
#endif

static void decode_asscanline_native( ASImageDecoder *imdec, unsigned int skip, int y );
static void decode_asscanline_ximage( ASImageDecoder *imdec, unsigned int skip, int y );

void decode_image_scanline_normal( ASImageDecoder *imdec );
void decode_image_scanline_beveled( ASImageDecoder *imdec );
void decode_image_scl_bevel_solid( ASImageDecoder *imdec );


Bool create_image_xim( ASVisual *asv, ASImage *im, ASAltImFormats format );
Bool create_image_argb32( ASVisual *asv, ASImage *im, ASAltImFormats format );

void encode_image_scanline_asim( ASImageOutput *imout, ASScanline *to_store );
void encode_image_scanline_xim( ASImageOutput *imout, ASScanline *to_store );
void encode_image_scanline_mask_xim( ASImageOutput *imout, ASScanline *to_store );
void encode_image_scanline_argb32( ASImageOutput *imout, ASScanline *to_store );

static struct ASImageFormatHandlers
{
	Bool (*check_create_asim_format)( ASVisual *asv, ASImage *im, ASAltImFormats format );
	void (*encode_image_scanline)( ASImageOutput *imout, ASScanline *to_store );
}asimage_format_handlers[ASA_Formats] =
{
	{ NULL, encode_image_scanline_asim },
	{ create_image_xim, encode_image_scanline_xim },
	{ create_image_xim, encode_image_scanline_mask_xim },
	{ create_image_xim, encode_image_scanline_xim },
	{ create_image_xim, encode_image_scanline_mask_xim },
	{ create_image_argb32, encode_image_scanline_argb32 },
	{ NULL, NULL }                             /* vector of doubles */
};



void output_image_line_top( ASImageOutput *, ASScanline *, int );
void output_image_line_fine( ASImageOutput *, ASScanline *, int );
void output_image_line_fast( ASImageOutput *, ASScanline *, int );
void output_image_line_direct( ASImageOutput *, ASScanline *, int );


/* *********************************************************************
 * quality control: we support several levels of quality to allow for
 * smooth work on older computers.
 * *********************************************************************/
static int asimage_quality_level = ASIMAGE_QUALITY_GOOD;
#ifdef HAVE_MMX
Bool asimage_use_mmx = True;
#else
Bool asimage_use_mmx = False;
#endif
/* ********************************************************************/
/* initialization routines 											  */
/* *********************************************************************/
/* ************************** MMX **************************************/
/*inline extern*/
int mmx_init(void)
{
int mmx_available = 0;
#ifdef HAVE_MMX
	asm volatile (
                      /* Get CPU version information */
                      "pushl %%eax\n\t"
                      "pushl %%ebx\n\t"
                      "pushl %%ecx\n\t"
                      "pushl %%edx\n\t"
                      "movl $1, %%eax\n\t"
                      "cpuid\n\t"
                      "andl $0x800000, %%edx \n\t"
					  "movl %%edx, %0\n\t"
                      "popl %%edx\n\t"
                      "popl %%ecx\n\t"
                      "popl %%ebx\n\t"
                      "popl %%eax\n\t"
                      : "=m" (mmx_available)
                      : /* no input */
					  : "ebx", "ecx", "edx"
			  );
#endif
	return mmx_available;
}

int mmx_off(void)
{
#ifdef HAVE_MMX
	__asm__ __volatile__ (
                      /* exit mmx state : */
                      "emms \n\t"
                      : /* no output */
                      : /* no input  */
			  );
#endif
	return 0;
}

/* *********************   ASImage  ************************************/
void
asimage_init (ASImage * im, Bool free_resources)
{
	if (im != NULL)
	{
		if (free_resources)
		{
			register int i ;
			for( i = im->height*4-1 ; i>= 0 ; --i )
				if( im->red[i] )
					free( im->red[i] );
			if( im->red )
				free(im->red);
#ifndef X_DISPLAY_MISSING
			if( im->alt.ximage )
				XDestroyImage( im->alt.ximage );
			if( im->alt.mask_ximage )
				XDestroyImage( im->alt.mask_ximage );
#endif
			if( im->alt.argb32 )
				free( im->alt.argb32 );
			if( im->alt.vector )
				free( im->alt.vector );
		}
		memset (im, 0x00, sizeof (ASImage));
		im->magic = MAGIC_ASIMAGE ;
		im->back_color = ARGB32_DEFAULT_BACK_COLOR ;
	}
}

void
flush_asimage_cache( ASImage *im )
{
#ifndef X_DISPLAY_MISSING
			if( im->alt.ximage )
            {
				XDestroyImage( im->alt.ximage );
                im->alt.ximage = NULL ;
            }
			if( im->alt.mask_ximage )
            {
				XDestroyImage( im->alt.mask_ximage );
                im->alt.mask_ximage = NULL ;
            }
#endif
}

void
asimage_start (ASImage * im, unsigned int width, unsigned int height, unsigned int compression)
{
	if (im)
	{
		register unsigned int i;

		asimage_init (im, True);
		/* we want result to be 32bit aligned and padded */
		im->red = safemalloc (sizeof (CARD8*) * height * 4);
		if( im->red == NULL )
		{
			show_error( "Insufficient memory to create image %dx%d!", width, height );
			if( im->red )
				free( im->red );
			return ;
		}
		im->height = height;
		im->width = width;

		for( i = 0 ; i < height<<2 ; i++ )
			im->red[i] = 0 ;
		im->green = im->red+height;
		im->blue = 	im->red+(height*2);
		im->alpha = im->red+(height*3);
		im->channels[IC_RED] = im->red ;
		im->channels[IC_GREEN] = im->green ;
		im->channels[IC_BLUE] = im->blue ;
		im->channels[IC_ALPHA] = im->alpha ;

		im->max_compressed_width = width*compression/100;
		if( im->max_compressed_width > im->width )
			im->max_compressed_width = im->width ;
	}
}

ASImage *
create_asimage( unsigned int width, unsigned int height, unsigned int compression)
{
	ASImage *im = safecalloc( 1, sizeof(ASImage) );
	asimage_start( im, width, height, compression );
	if( im->width == 0 || im->height == 0 )
	{
		free( im );
		im = NULL ;
#ifdef TRACK_ASIMAGES
        show_error( "failed to create ASImage of size %dx%d with compression %d", width, height, compression );
#endif
    }else
    {
#ifdef TRACK_ASIMAGES
        show_progress( "created ASImage %p of size %dx%d with compression %d", im, width, height, compression );
        if( __as_image_registry == NULL )
            __as_image_registry = create_ashash( 0, pointer_hash_value, NULL, NULL );
        add_hash_item( __as_image_registry, AS_HASHABLE(im), im );
#endif
    }
	return im;
}

void
destroy_asimage( ASImage **im )
{
	if( im )
		if( *im && !AS_ASSERT_NOTVAL((*im)->imageman,NULL))
		{
#ifdef TRACK_ASIMAGES
            show_progress( "destroying ASImage %p of size %dx%d", *im, (*im)->width, (*im)->height );
            remove_hash_item( __as_image_registry, AS_HASHABLE(*im), NULL, False );
#endif
			asimage_init( *im, True );
			(*im)->magic = 0;
			free( *im );
			*im = NULL ;
		}
}

void print_asimage_func (ASHashableValue value)
{
    ASImage *im = (ASImage*)value ;
    if( im && im->magic == MAGIC_ASIMAGE )
    {
        unsigned int k;
        unsigned int red_mem = 0, green_mem = 0, blue_mem = 0, alpha_mem = 0;
        unsigned int red_count = 0, green_count = 0, blue_count = 0, alpha_count = 0;
        fprintf( stderr,"\n\tASImage[%p].size = %dx%d;\n",  im, im->width, im->height );
        fprintf( stderr,"\tASImage[%p].back_color = 0x%lX;\n", im, (long)im->back_color );
        fprintf( stderr,"\tASImage[%p].max_compressed_width = %d;\n", im, im->max_compressed_width);
        fprintf( stderr,"\t\tASImage[%p].alt.ximage = %p;\n", im, im->alt.ximage );
        if( im->alt.ximage )
        {
            fprintf( stderr,"\t\t\tASImage[%p].alt.ximage.bytes_per_line = %d;\n", im, im->alt.ximage->bytes_per_line);
            fprintf( stderr,"\t\t\tASImage[%p].alt.ximage.size = %dx%d;\n", im, im->alt.ximage->width, im->alt.ximage->height);
        }
        fprintf( stderr,"\t\tASImage[%p].alt.mask_ximage = %p;\n", im, im->alt.mask_ximage);
        if( im->alt.mask_ximage )
        {
            fprintf( stderr,"\t\t\tASImage[%p].alt.mask_ximage.bytes_per_line = %d;\n", im, im->alt.mask_ximage->bytes_per_line);
            fprintf( stderr,"\t\t\tASImage[%p].alt.mask_ximage.size = %dx%d;\n", im, im->alt.mask_ximage->width, im->alt.mask_ximage->height);
        }
        fprintf( stderr,"\t\tASImage[%p].alt.argb32 = %p;\n", im, im->alt.argb32 );
        fprintf( stderr,"\t\tASImage[%p].alt.vector = %p;\n", im, im->alt.vector );
        fprintf( stderr,"\tASImage[%p].imageman = %p;\n", im, im->imageman );
        fprintf( stderr,"\tASImage[%p].ref_count = %d;\n", im, im->ref_count );
        fprintf( stderr,"\tASImage[%p].name = \"%s\";\n", im, im->name );
        fprintf( stderr,"\tASImage[%p].flags = 0x%lX;\n", im, im->flags );

        for( k = 0 ; k < im->height ; k++ )
    	{
            unsigned int tmp ;

            tmp = asimage_print_line( im, IC_RED  , k, 0 );
            if( tmp > 0 )
            {
                ++red_count;
                red_mem += tmp ;
            }
            tmp = asimage_print_line( im, IC_GREEN, k, 0 );
            if( tmp > 0 )
            {
                ++green_count;
                green_mem += tmp ;
            }
            tmp = asimage_print_line( im, IC_BLUE , k, 0 );
            if( tmp > 0 )
            {
                ++blue_count;
                blue_mem += tmp ;
            }
            tmp = asimage_print_line( im, IC_ALPHA, k, 0 );
            if( tmp > 0 )
            {
                ++alpha_count;
                alpha_mem += tmp ;
            }
        }

        fprintf( stderr,"\tASImage[%p].uncompressed_size = %d;\n", im, im->width*red_count +
                                                                    im->width*green_count +
                                                                    im->width*blue_count +
                                                                    im->width*alpha_count );
        fprintf( stderr,"\tASImage[%p].compressed_size = %d;\n",   im, red_mem + green_mem +blue_mem + alpha_mem );
        fprintf( stderr,"\t\tASImage[%p].channel[red].lines_count = %d;\n", im, red_count );
        fprintf( stderr,"\t\tASImage[%p].channel[red].memory_used = %d;\n", im, red_mem );
        fprintf( stderr,"\t\tASImage[%p].channel[green].lines_count = %d;\n", im, green_count );
        fprintf( stderr,"\t\tASImage[%p].channel[green].memory_used = %d;\n", im, green_mem );
        fprintf( stderr,"\t\tASImage[%p].channel[blue].lines_count = %d;\n", im, blue_count );
        fprintf( stderr,"\t\tASImage[%p].channel[blue].memory_used = %d;\n", im, blue_mem );
        fprintf( stderr,"\t\tASImage[%p].channel[alpha].lines_count = %d;\n", im, alpha_count );
        fprintf( stderr,"\t\tASImage[%p].channel[alpha].memory_used = %d;\n", im, alpha_mem );
    }
}

void
print_asimage_registry()
{
#ifdef TRACK_ASIMAGES
    print_ashash( __as_image_registry, print_asimage_func );
#endif
}

void
purge_asimage_registry()
{
#ifdef TRACK_ASIMAGES
    if( __as_image_registry )
        destroy_ashash( &__as_image_registry );
#endif
}


Bool create_image_xim( ASVisual *asv, ASImage *im, ASAltImFormats format )
{
	Bool scratch = False ; 
	XImage **dst ;
	if( format == ASA_ScratchXImage || format == ASA_ScratchMaskXImage ) 
	{	
		scratch = True ;
		format = (format - ASA_ScratchXImage ) + ASA_XImage ;
	}		
	dst = (format == ASA_MaskXImage )? &(im->alt.mask_ximage):&(im->alt.ximage);
	if( *dst == NULL )
	{
		int depth = 0 ;
		if( format == ASA_MaskXImage )
			depth = get_flags(im->flags, ASIM_XIMAGE_8BIT_MASK )? 8: 1;
		if( scratch )
			*dst = create_visual_scratch_ximage( asv, im->width, im->height, depth );
		else
			*dst = create_visual_ximage( asv, im->width, im->height, depth );
		if( *dst == NULL )
			show_error( "Unable to create %sXImage for the visual %d",
				        (format == ASA_MaskXImage )?"mask ":"",
						asv->visual_info.visualid );
	}
	return ( *dst != NULL );
}

Bool create_image_argb32( ASVisual *asv, ASImage *im, ASAltImFormats format )
{
	if( im->alt.argb32 == NULL )
		im->alt.argb32 = safemalloc( im->width*im->height*sizeof(ARGB32) );
	return True;
}

/* ******************** ASImageManager ****************************/
void
asimage_destroy (ASHashableValue value, void *data)
{
	if( data )
	{
		ASImage *im = (ASImage*)data ;
		if( im != NULL )
		{
			if( AS_ASSERT_NOTVAL(im->magic, MAGIC_ASIMAGE) )
				im = NULL ;
			else
				im->imageman = NULL ;
		}
		free( (char*)value );
		destroy_asimage( &im );
	}
}

ASImageManager *create_image_manager( struct ASImageManager *reusable_memory, double gamma, ... )
{
	ASImageManager *imman = reusable_memory ;
	int i ;
	va_list ap;

	if( imman == NULL )
		imman = safecalloc( 1, sizeof(ASImageManager));
	else
		memset( imman, 0x00, sizeof(ASImageManager));

	va_start (ap, gamma);
	for( i = 0 ; i < MAX_SEARCH_PATHS ; i++ )
	{
		char *path = va_arg(ap,char*);
		if( path == NULL )
			break;
		imman->search_path[i] = mystrdup( path );
	}
	va_end (ap);

	imman->search_path[MAX_SEARCH_PATHS] = NULL ;
	imman->gamma = gamma ;

	imman->image_hash = create_ashash( 7, string_hash_value, string_compare, asimage_destroy );

	return imman;
}

void
destroy_image_manager( struct ASImageManager *imman, Bool reusable )
{
	if( imman )
	{
		int i = MAX_SEARCH_PATHS;
		destroy_ashash( &(imman->image_hash) );
		while( --i >= 0 )
			if(imman->search_path[i])
				free( imman->search_path[i] );

		if( !reusable )
			free( imman );
		else
			memset( imman, 0x00, sizeof(ASImageManager));
	}
}

Bool
store_asimage( ASImageManager* imageman, ASImage *im, const char *name )
{
	Bool res = False ;
	if( !AS_ASSERT(im) )
		if( AS_ASSERT_NOTVAL(im->magic, MAGIC_ASIMAGE) )
			im = NULL ;
	if( !AS_ASSERT(imageman) && !AS_ASSERT(im) && !AS_ASSERT((char*)name) )
		if( im->imageman == NULL )
		{
			int hash_res ;
			im->name = mystrdup( name );
			hash_res = add_hash_item( imageman->image_hash, (ASHashableValue)(char*)(im->name), im);
			res = ( hash_res == ASH_Success);
			if( !res )
			{
				free( im->name );
				im->name = NULL ;
			}else
			{
				im->imageman = imageman ;
				im->ref_count = 1 ;
			}
		}
	return res ;
}

inline ASImage *
query_asimage( ASImageManager* imageman, const char *name )
{
	ASImage *im = NULL ;
	if( !AS_ASSERT(imageman) && !AS_ASSERT(name) )
	{
		ASHashData hdata = {0} ;
		if( get_hash_item( imageman->image_hash, AS_HASHABLE((char*)name), &hdata.vptr) == ASH_Success )
		{
			im = hdata.vptr ;
			if( im->magic != MAGIC_ASIMAGE )
				im = NULL ;
        }
	}
	return im;
}

ASImage *
fetch_asimage( ASImageManager* imageman, const char *name )
{
    ASImage *im = query_asimage( imageman, name );
    if( im )
	{
        im->ref_count++ ;
	}
	return im;
}


ASImage *
dup_asimage( ASImage* im )
{
	if( !AS_ASSERT(im) )
		if( AS_ASSERT_NOTVAL(im->magic,MAGIC_ASIMAGE) )
			im = NULL ;

	if( !AS_ASSERT(im) && !AS_ASSERT(im->imageman) )
	{
/*		fprintf( stderr, __FUNCTION__" on image %p ref_count = %d\n", im, im->ref_count ); */
		im->ref_count++ ;
		return im;
	}
	return NULL ;
}

inline int
release_asimage( ASImage *im )
{
	int res = -1 ;
	if( !AS_ASSERT(im) )
	{
		if( im->magic == MAGIC_ASIMAGE )
		{
			if( --(im->ref_count) <= 0 )
			{
				ASImageManager *imman = im->imageman ;
				if( !AS_ASSERT(imman) )
                    if( remove_hash_item(imman->image_hash, (ASHashableValue)(char*)im->name, NULL, True) != ASH_Success )
                        destroy_asimage( &im );
			}else
				res = im->ref_count ;
		}
	}
	return res ;
}

void
forget_asimage( ASImage *im )
{
	if( !AS_ASSERT(im) )
	{
		if( im->magic == MAGIC_ASIMAGE )
		{
			ASImageManager *imman = im->imageman ;
			if( !AS_ASSERT(imman) )
				remove_hash_item(imman->image_hash, (ASHashableValue)(char*)im->name, NULL, False);
/*            im->ref_count = 0;     release_asimage should still correctly work !!!!
 *            im->imageman = NULL;
 */
		}
	}
}

void
forget_asimage_name( ASImageManager *imman, const char *name )
{
    if( !AS_ASSERT(imman) && name != NULL )
	{
        remove_hash_item(imman->image_hash, AS_HASHABLE((char*)name), NULL, False);
    }
}

inline int
safe_asimage_destroy( ASImage *im )
{
	int res = -1 ;
	if( !AS_ASSERT(im) )
	{
		if( im->magic == MAGIC_ASIMAGE )
		{
			ASImageManager *imman = im->imageman ;
			if( imman != NULL )
			{
                res = --(im->ref_count) ;
                if( im->ref_count <= 0 )
					remove_hash_item(imman->image_hash, (ASHashableValue)(char*)im->name, NULL, True);
            }else
			{
				destroy_asimage( &im );
				res = -1 ;
			}
		}
	}
	return res ;
}

int
release_asimage_by_name( ASImageManager *imageman, char *name )
{
	int res = -1 ;
	ASImage *im = NULL ;
	if( !AS_ASSERT(imageman) && !AS_ASSERT(name) )
	{
		ASHashData hdata ;
		if( get_hash_item( imageman->image_hash, AS_HASHABLE((char*)name), &hdata.vptr) == ASH_Success )
		{
			im = hdata.vptr ;
			res = release_asimage( im );
		}
	}
	return res ;
}

/* ******************** ASGradient ****************************/

void
destroy_asgradient( ASGradient **pgrad )
{
	if( pgrad && *pgrad )
	{
		if( (*pgrad)->color )
		{
			free( (*pgrad)->color );
			(*pgrad)->color = NULL ;
		}
		if( (*pgrad)->offset )
		{
			free( (*pgrad)->offset );
			(*pgrad)->offset = NULL ;
		}
		(*pgrad)->npoints = 0 ;
		free( *pgrad );
		*pgrad = NULL ;
	}

}

ASGradient *
flip_gradient( ASGradient *orig, int flip )
{
	ASGradient *grad ;
	int npoints ;
	int type ;
	Bool inverse_points = False ;

	flip &= FLIP_MASK ;
	if( orig == NULL || flip == 0 )
		return orig;

	grad = safecalloc( 1, sizeof(ASGradient));

	grad->npoints = npoints = orig->npoints ;
	type = orig->type ;
    grad->color = safemalloc( npoints*sizeof(ARGB32) );
    grad->offset = safemalloc( npoints*sizeof(double) );

	if( get_flags(flip, FLIP_VERTICAL) )
	{
		Bool upsidedown = get_flags(flip, FLIP_UPSIDEDOWN) ;
		switch(type)
		{
			case GRADIENT_Left2Right  :
				type = GRADIENT_Top2Bottom ; inverse_points = !upsidedown ;
				break;
			case GRADIENT_TopLeft2BottomRight :
				type = GRADIENT_BottomLeft2TopRight ; inverse_points = upsidedown ;
				break;
			case GRADIENT_Top2Bottom  :
				type = GRADIENT_Left2Right ; inverse_points = upsidedown ;
				break;
			case GRADIENT_BottomLeft2TopRight :
				type = GRADIENT_TopLeft2BottomRight ; inverse_points = !upsidedown ;
				break;
		}
	}else if( flip == FLIP_UPSIDEDOWN )
	{
		inverse_points = True ;
	}

	grad->type = type ;
	if( inverse_points )
    {
        register int i = 0, k = npoints;
        while( --k >= 0 )
        {
            grad->color[i] = orig->color[k] ;
            grad->offset[i] = 1.0 - orig->offset[k] ;
			++i ;
        }
    }else
	{
        register int i = npoints ;
        while( --i >= 0 )
        {
            grad->color[i] = orig->color[i] ;
            grad->offset[i] = orig->offset[i] ;
        }
    }
	return grad;
}

/* ******************** ASImageDecoder ****************************/
ASImageDecoder *
start_image_decoding( ASVisual *asv,ASImage *im, ASFlagType filter,
					  int offset_x, int offset_y,
					  unsigned int out_width,
					  unsigned int out_height,
					  ASImageBevel *bevel )
{
	ASImageDecoder *imdec = NULL;

	if( asv == NULL )
		asv = get_default_asvisual();

 	if( AS_ASSERT(filter) || AS_ASSERT(asv))
		return NULL;
	if( im != NULL )
		if( im->magic != MAGIC_ASIMAGE )
		{
#ifdef LOCAL_DEBUG
			ASImage **tmp = NULL;
			*tmp = im ;                        /* segfault !!!!! */
#endif
			im = NULL ;
		}

	if( im == NULL )
	{
		offset_x = offset_y = 0 ;
		if( AS_ASSERT(out_width)|| AS_ASSERT(out_height))
			return NULL ;
	}else
	{
		if( offset_x < 0 )
			offset_x = im->width - (offset_x%im->width);
		else
			offset_x %= im->width ;
		if( offset_y < 0 )
			offset_y = im->height - (offset_y%im->height);
		else
			offset_y %= im->height ;
		if( out_width == 0 )
			out_width = im->width ;
		if( out_height == 0 )
			out_height = im->height ;

	}

	imdec = safecalloc( 1, sizeof(ASImageDecoder));
	imdec->asv = asv ;
	imdec->im = im ;
	imdec->filter = filter ;
	imdec->offset_x = offset_x ;
	imdec->out_width = out_width;
	imdec->offset_y = offset_y ;
	imdec->out_height = out_height;
	imdec->next_line = offset_y ;
	imdec->back_color = (im != NULL)?im->back_color:ARGB32_DEFAULT_BACK_COLOR ;
    imdec->bevel = bevel ;
  	if( bevel )
	{
		if( bevel->left_outline > MAX_BEVEL_OUTLINE )
			bevel->left_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->top_outline > MAX_BEVEL_OUTLINE )
			bevel->top_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->right_outline > MAX_BEVEL_OUTLINE )
			bevel->right_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->bottom_outline > MAX_BEVEL_OUTLINE )
			bevel->bottom_outline = MAX_BEVEL_OUTLINE ;
		if( bevel->left_inline > out_width )
			bevel->left_inline = MAX((int)out_width,0) ;
		if( bevel->top_inline > out_height )
			bevel->top_inline = MAX((int)out_height,0) ;
		if( bevel->left_inline+bevel->right_inline > (int)out_width )
			bevel->right_inline = MAX((int)out_width-(int)bevel->left_inline,0) ;
		if( bevel->top_inline+bevel->bottom_inline > (int)out_height )
			bevel->bottom_inline = MAX((int)out_height-(int)bevel->top_inline,0) ;

		if( bevel->left_outline == 0 && bevel->right_outline == 0 &&
			bevel->top_outline == 0 && bevel->bottom_outline == 0 &&
			bevel->left_inline == 0 && bevel->left_inline == 0 &&
			bevel->top_inline == 0 && bevel->bottom_inline == 0 )
			imdec->bevel = bevel = NULL ;
	}
	if( bevel )
	{
		imdec->bevel_left   = bevel->left_outline ;
		imdec->bevel_top    = bevel->top_outline ;
		imdec->bevel_right  = imdec->bevel_left + (int)out_width ;
		imdec->bevel_bottom = imdec->bevel_top  + (int)out_height;
		imdec->bevel_h_addon  = bevel->left_outline+ bevel->right_outline;
		imdec->bevel_v_addon  = bevel->top_outline + bevel->bottom_outline;

		imdec->decode_image_scanline = decode_image_scanline_beveled ;
	}else
		imdec->decode_image_scanline = decode_image_scanline_normal ;

	prepare_scanline(out_width+imdec->bevel_h_addon, 0, &(imdec->buffer), asv->BGR_mode );
    imdec->buffer.back_color = (im != NULL)?im->back_color:ARGB32_DEFAULT_BACK_COLOR ;

	if( im == NULL )
		imdec->decode_asscanline = decode_asscanline_native;
	else if( get_flags( im->flags, ASIM_DATA_NOT_USEFUL ) && im->alt.ximage != NULL )
	{
		imdec->decode_asscanline = decode_asscanline_ximage;
		imdec->xim_buffer = safecalloc(1, sizeof(ASScanline));
		prepare_scanline(im->alt.ximage->width, 0, imdec->xim_buffer, asv->BGR_mode );
	}else
		imdec->decode_asscanline = decode_asscanline_native;

	return imdec;
}

void
set_decoder_bevel_geom( ASImageDecoder *imdec, int x, int y,
                        unsigned int width, unsigned int height )
{
	if( imdec && imdec->bevel )
	{
		ASImageBevel *bevel = imdec->bevel ;
		int tmp ;
		if( imdec->im )
		{
			if( width == 0 )
				width = imdec->im->width ;
			if( height == 0 )
				height= imdec->im->height;
		}else
		{
			if( width == 0 )
				width = MAX( (int)imdec->out_width - x,0) ;
			if( height == 0 )
				height= MAX( (int)imdec->out_height - y,0) ;
		}
		/* Bevel should completely encompas output region */
		x = MIN(x,0);
		y = MIN(y,0);
		if( x+width < imdec->out_width )
			width += (int)imdec->out_width - x ;
		if( y+height < imdec->out_height )
			height += (int)imdec->out_height - y ;

		imdec->bevel_left = x ;
		imdec->bevel_top  = y ;
		imdec->bevel_right = x+(int)width ;
		imdec->bevel_bottom = y+(int)height ;


		imdec->bevel_h_addon  = MAX(imdec->bevel_left+(int)bevel->left_outline, 0) ;
		tmp = MAX(0, (int)imdec->out_width - imdec->bevel_right );
		imdec->bevel_h_addon += MIN( tmp, (int)bevel->right_outline);

		imdec->bevel_v_addon  = MAX(imdec->bevel_top+(int)bevel->top_outline, 0) ;
		tmp = MAX(0, (int)imdec->out_height - imdec->bevel_bottom );
		imdec->bevel_v_addon += MIN( tmp, (int)bevel->bottom_outline);
	}
}

void
set_decoder_shift( ASImageDecoder *imdec, int shift )
{
	if( shift != 0 )
		shift = 8 ;

	if( imdec )
		imdec->buffer.shift = shift ;
}

void set_decoder_back_color( ASImageDecoder *imdec, ARGB32 back_color )
{
	if( imdec )
	{
		imdec->back_color = back_color ;
		imdec->buffer.back_color = back_color ;
	}
}


void
stop_image_decoding( ASImageDecoder **pimdec )
{
	if( pimdec )
		if( *pimdec )
		{
			free_scanline( &((*pimdec)->buffer), True );
			if( (*pimdec)->xim_buffer )
			{
				free_scanline( (*pimdec)->xim_buffer, True );
				free( (*pimdec)->xim_buffer );
			}

			free( *pimdec );
			*pimdec = NULL;
		}
}

/* ******************** ASImageOutput ****************************/

ASImageOutput *
start_image_output( ASVisual *asv, ASImage *im, ASAltImFormats format,
                    int shift, int quality )
{
	register ASImageOutput *imout= NULL;

	if( im != NULL )
		if( im->magic != MAGIC_ASIMAGE )
		{
			im = NULL ;
		}

	if( asv == NULL )
		asv = get_default_asvisual();

	if( AS_ASSERT(im) || AS_ASSERT(asv) )
		return imout;
	if( format < 0 || format == ASA_Vector || format >= ASA_Formats)
		return NULL;
	if( asimage_format_handlers[format].check_create_asim_format )
		if( !asimage_format_handlers[format].check_create_asim_format(asv, im, format) )
			return NULL;

	imout = safecalloc( 1, sizeof(ASImageOutput));
	imout->asv = asv;
	imout->im = im ;

	imout->out_format = format ;
	imout->encode_image_scanline = asimage_format_handlers[format].encode_image_scanline;

	prepare_scanline( im->width, 0, &(imout->buffer[0]), asv->BGR_mode);
	prepare_scanline( im->width, 0, &(imout->buffer[1]), asv->BGR_mode);

	imout->chan_fill[IC_RED]   = ARGB32_RED8(im->back_color);
	imout->chan_fill[IC_GREEN] = ARGB32_GREEN8(im->back_color);
	imout->chan_fill[IC_BLUE]  = ARGB32_BLUE8(im->back_color);
	imout->chan_fill[IC_ALPHA] = ARGB32_ALPHA8(im->back_color);

	imout->available = &(imout->buffer[0]);
	imout->used 	 = NULL;
	imout->buffer_shift = shift;
	imout->next_line = 0 ;
	imout->bottom_to_top = 1 ;
	if( quality > ASIMAGE_QUALITY_TOP || quality < ASIMAGE_QUALITY_POOR )
		quality = asimage_quality_level;

	imout->quality = quality ;
	if( shift > 0 )
	{/* choose what kind of error diffusion we'll use : */
		switch( quality )
		{
			case ASIMAGE_QUALITY_POOR :
			case ASIMAGE_QUALITY_FAST :
				imout->output_image_scanline = output_image_line_fast ;
				break;
			case ASIMAGE_QUALITY_GOOD :
				imout->output_image_scanline = output_image_line_fine ;
				break;
			case ASIMAGE_QUALITY_TOP  :
				imout->output_image_scanline = output_image_line_top ;
				break;
		}
	}else /* no quanitzation - no error diffusion */
		imout->output_image_scanline = output_image_line_direct ;

	return imout;
}

void set_image_output_back_color( ASImageOutput *imout, ARGB32 back_color )
{
	if( imout )
	{
		imout->chan_fill[IC_RED]   = ARGB32_RED8  (back_color);
		imout->chan_fill[IC_GREEN] = ARGB32_GREEN8(back_color);
		imout->chan_fill[IC_BLUE]  = ARGB32_BLUE8 (back_color);
		imout->chan_fill[IC_ALPHA] = ARGB32_ALPHA8(back_color);
	}
}

void toggle_image_output_direction( ASImageOutput *imout )
{
	if( imout )
	{
		if( imout->bottom_to_top < 0 )
		{
			if( imout->next_line >= (int)imout->im->height-1 )
				imout->next_line = 0 ;
			imout->bottom_to_top = 1 ;
		}else if( imout->next_line <= 0 )
		{
		 	imout->next_line = (int)imout->im->height-1 ;
			imout->bottom_to_top = -1 ;
		}
	}
}


void
stop_image_output( ASImageOutput **pimout )
{
	if( pimout )
	{
		register ASImageOutput *imout = *pimout;
		if( imout )
		{
			if( imout->used )
				imout->output_image_scanline( imout, NULL, 1);
			free_scanline(&(imout->buffer[0]), True);
			free_scanline(&(imout->buffer[1]), True);
			free( imout );
			*pimout = NULL;
		}
	}
}

/* ******************** ASImageLayer ****************************/

inline void
init_image_layers( register ASImageLayer *l, int count )
{
	memset( l, 0x00, sizeof(ASImageLayer)*count );
	while( --count >= 0 )
	{
		l[count].merge_scanlines = alphablend_scanlines ;
/*		l[count].solid_color = ARGB32_DEFAULT_BACK_COLOR ; */
	}
}

ASImageLayer *
create_image_layers( int count )
{
	ASImageLayer *l = NULL;

	if( count > 0 )
	{
		l = safecalloc( count, sizeof(ASImageLayer) );
		init_image_layers( l, count );
	}
	return l;
}

void
destroy_image_layers( register ASImageLayer *l, int count, Bool reusable )
{
	if( l )
	{
		register int i = count;
		while( --i >= 0 )
		{
			if( l[i].im )
			{
				if( l[i].im->imageman )
					release_asimage( l[i].im );
				else
					destroy_asimage( &(l[i].im) );
			}
			if( l[i].bevel )
				free( l[i].bevel );
		}
		if( !reusable )
			free( l );
		else
			memset( l, 0x00, sizeof(ASImageLayer)*count );
	}
}



/* **********************************************************************/
/*  Compression/decompression 										   */
/* **********************************************************************/
static void
asimage_apply_buffer (ASImage * im, ColorPart color, unsigned int y, CARD8 *buffer, size_t buf_used)
{
	register int len = (buf_used>>2)+1 ;
	CARD8  **part = im->channels[color];
	register CARD32  *dwdst = safemalloc( sizeof(CARD32)*len);
	register CARD32  *dwsrc = (CARD32*)buffer;
	while( --len >= 0 )
		dwdst[len] = dwsrc[len];

	if (part[y] != NULL)
		free (part[y]);
	part[y] = (CARD8*)dwdst;
}

static void
asimage_dup_line (ASImage * im, ColorPart color, unsigned int y1, unsigned int y2, unsigned int length)
{
	CARD8       **part = im->channels[color];
	if (part[y2] != NULL)
		free (part[y2]);
	if( part[y1] )
	{
		register unsigned int i ;
		register CARD32 *dwsrc = (CARD32*)part[y1];
		register CARD32 *dwdst ;
		length = (length>>2)+1;
		dwdst = safemalloc (sizeof(CARD32)*length);
		/* 32bit copy gives us about 15% performance gain */
		for( i = 0 ; i < length ; ++i )
			dwdst[i] = dwsrc[i];
		part[y2] = (CARD8*)dwdst;
	}else
		part[y2] = NULL;
}

void
asimage_erase_line( ASImage * im, ColorPart color, unsigned int y )
{
	if( !AS_ASSERT(im) )
	{
		if( color < IC_NUM_CHANNELS )
		{
			CARD8       **part = im->channels[color];
			if( part[y] )
			{
				free( part[y] );
				part[y] = NULL;
			}
		}else
		{
			int c ;
			for( c = 0 ; c < IC_NUM_CHANNELS ; c++ )
			{
				CARD8       **part = im->channels[color];
				if( part[y] )
				{
					free( part[y] );
					part[y] = NULL;
				}
			}
		}
	}
}


size_t
asimage_add_line_mono (ASImage * im, ColorPart color, register CARD8 value, unsigned int y)
{
	register CARD8 *dst;
	int rep_count;
	int buf_used = 0;

	if (AS_ASSERT(im) || color <0 || color >= IC_NUM_CHANNELS )
		return 0;

	if( (dst = get_compression_buffer(max(RLE_THRESHOLD+2,4))) == NULL || y >= im->height)
		return 0;

	if( im->width <= RLE_THRESHOLD )
	{
		int i = im->width+1 ;
		dst[0] = (CARD8) RLE_DIRECT_TAIL;
		dst[i] = 0 ;
		while( --i > 0 )
			dst[i] = (CARD8) value;
		buf_used = 1+im->width+1;
	}else
	{
		rep_count = im->width - RLE_THRESHOLD;
		if (rep_count <= RLE_MAX_SIMPLE_LEN)
		{
			dst[0] = (CARD8) rep_count;
			dst[1] = (CARD8) value;
			dst[2] = 0 ;
			buf_used = 3;
		} else
		{
			dst[0] = (CARD8) ((rep_count >> 8) & RLE_LONG_D)|RLE_LONG_B;
			dst[1] = (CARD8) ((rep_count) & 0x00FF);
			dst[2] = (CARD8) value;
			dst[3] = 0 ;
			buf_used = 4;
		}
	}
	asimage_apply_buffer (im, color, y, dst, buf_used);
	release_compression_buffer( dst );
	return buf_used;
}

size_t
asimage_add_line (ASImage * im, ColorPart color, register CARD32 * data, unsigned int y)
{
	int    i = 0, bstart = 0, ccolor = 0;
	unsigned int    width;
	register CARD8 *dst;
	register int 	tail = 0;
	int best_size, best_bstart = 0, best_tail = 0;
	int buf_used = 0 ;

	if (AS_ASSERT(im) || AS_ASSERT(data) || color <0 || color >= IC_NUM_CHANNELS )
		return 0;
	if (y >= im->height)
		return 0;

	best_size = 0 ;
/*	fprintf( stderr, "max = %d, width = %d, %d:%d:%d<%2.2X ", im->max_compressed_width, im->width, y, color, 0, data[0] );*/
	clear_flags( im->flags, ASIM_DATA_NOT_USEFUL );
	if( im->width == 1 )
	{
		if( (dst = get_compression_buffer( 2 )) == NULL )
			return 0;
		dst[0] = RLE_DIRECT_TAIL ;
		dst[1] = data[0] ;
		buf_used = 2;
	}else
	{
		CARD8 *dst_start ;
		if( (dst_start = get_compression_buffer( im->width*2 )) == NULL )
			return 0;
		dst = dst_start ;
/*		width = im->width; */
		width = im->max_compressed_width ;
		while( i < width )
		{
			while( i < width && data[i] == data[ccolor])
			{
/*				fprintf( stderr, "%d<%2.2X ", i, data[i] ); */
				++i ;
			}
			if( i > ccolor + RLE_THRESHOLD )
			{ /* we have to write repetition count and length */
				register unsigned int rep_count = i - ccolor - RLE_THRESHOLD;

				if (rep_count <= RLE_MAX_SIMPLE_LEN)
				{
					dst[tail] = (CARD8) rep_count;
					dst[++tail] = (CARD8) data[ccolor];
/*					fprintf( stderr, "\n%d:%d: >%d: %2.2X %d: %2.2X ", y, color, tail-1, dst[tail-1], tail, dst[tail] ); */
				} else
				{
					dst[tail] = (CARD8) ((rep_count >> 8) & RLE_LONG_D)|RLE_LONG_B;
					dst[++tail] = (CARD8) ((rep_count) & 0x00FF);
					dst[++tail] = (CARD8) data[ccolor];
/*					fprintf( stderr, "\n%d:%d: >%d: %2.2X %d: %2.2X %d: %2.2X ", y, color, tail-2, dst[tail-2], tail-1, dst[tail-1], tail, dst[tail] );*/
				}
				++tail ;
				bstart = ccolor = i;
			}
/*			fprintf( stderr, "\n"); */
			while( i < width )
			{
/*				fprintf( stderr, "%d<%2.2X ", i, data[i] ); */
				if( data[i] != data[ccolor] )
					ccolor = i ;
				else if( i-ccolor > RLE_THRESHOLD )
					break;
				++i ;
			}
			if( i == width )
				ccolor = i ;
			while( ccolor > bstart )
			{/* we have to write direct block */
				unsigned int dist = ccolor-bstart ;

				if( tail-(int)bstart < best_size )
				{
					best_tail = tail ;
					best_bstart = bstart ;
					best_size = tail-bstart ;
				}
				if( dist > RLE_MAX_DIRECT_LEN )
					dist = RLE_MAX_DIRECT_LEN ;
				dst[tail] = RLE_DIRECT_B | ((CARD8)(dist-1));
/*				fprintf( stderr, "\n%d:%d: >%d: %2.2X ", y, color, tail, dst[tail] ); */
				dist += bstart ;
				++tail ;
				while ( bstart < dist )
				{
					dst[tail] = (CARD8) data[bstart];
/*					fprintf( stderr, "%d: %2.2X ", tail, dst[tail]); */
					++tail ;
					++bstart;
				}
			}
/*			fprintf( stderr, "\n"); */
		}
		if( best_size+(int)im->width < tail )
		{
			width = im->width;
/*			LOCAL_DEBUG_OUT( " %d:%d >resetting bytes starting with offset %d(%d) (0x%2.2X) to DIRECT_TAIL( %d bytes total )", y, color, best_tail, best_bstart, dst[best_tail], width-best_bstart ); */
			dst[best_tail] = RLE_DIRECT_TAIL;
			dst += best_tail+1;
			data += best_bstart;
			for( i = width-best_bstart-1 ; i >=0 ; --i )
				dst[i] = data[i] ;
			buf_used = best_tail+1+width-best_bstart ;
		}else
		{
			if( i < im->width )
			{
				dst[tail] = RLE_DIRECT_TAIL;
				dst += tail+1 ;
				data += i;
				buf_used = tail+1+im->width-i ;
				for( i = im->width-(i+1) ; i >=0 ; --i )
					dst[i] = data[i] ;
			}else
			{
				dst[tail] = RLE_EOL;
				buf_used = tail+1;
			}
		}
		dst = dst_start ;
	}
	asimage_apply_buffer (im, color, y, dst, buf_used);
	release_compression_buffer( dst );
	return buf_used;
}

ASFlagType
get_asimage_chanmask( ASImage *im)
{
    ASFlagType mask = 0 ;
	int color ;

	if( !AS_ASSERT(im) )
		for( color = 0; color < IC_NUM_CHANNELS ; color++ )
		{
			register CARD8 **chan = im->channels[color];
			register int y, height = im->height ;
			for( y = 0 ; y < height ; y++ )
				if( chan[y] )
				{
					set_flags( mask, 0x01<<color );
					break;
				}
		}
    return mask ;
}

int
check_asimage_alpha (ASVisual *asv, ASImage *im )
{
	int recomended_depth = 0 ;
	unsigned int            i;
	ASScanline     buf;

	if( asv == NULL )
		asv = get_default_asvisual();

	if (im == NULL)
		return 0;

	prepare_scanline( im->width, 0, &buf, asv->BGR_mode );
	buf.flags = SCL_DO_ALPHA ;
	for (i = 0; i < im->height; i++)
	{
		int count = asimage_decode_line (im, IC_ALPHA, buf.alpha, i, 0, buf.width);
		if( count < (int)buf.width )
		{
			if( ARGB32_ALPHA8(im->back_color) == 0 )
			{
				if( recomended_depth == 0 )
					recomended_depth = 1 ;
			}else if( ARGB32_ALPHA8(im->back_color) != 0xFF )
			{
				recomended_depth = 8 ;
				break ;
			}
		}
		while( --count >= 0 )
			if( buf.alpha[count] == 0  )
			{
				if( recomended_depth == 0 )
					recomended_depth = 1 ;
			}else if( (buf.alpha[count]&0xFF) != 0xFF  )
			{
				recomended_depth = 8 ;
				break ;
			}
		if( recomended_depth == 8 )
			break;
	}
	free_scanline(&buf, True);

	return recomended_depth;
}



unsigned int
asimage_print_line (ASImage * im, ColorPart color, unsigned int y, unsigned long verbosity)
{
	CARD8       **color_ptr;
	register CARD8 *ptr;
	int           to_skip = 0;
	int 		  uncopressed_size = 0 ;

	if (AS_ASSERT(im) || color < 0 || color >= IC_NUM_CHANNELS )
		return 0;
	if (y >= im->height)
		return 0;

	color_ptr = im->channels[color];
	if( AS_ASSERT(color_ptr) )
		return 0;
	ptr = color_ptr[y];
	if( ptr == NULL )
	{
		if(  verbosity != 0 )
			show_error( "no data available for line %d", y );
		return 0;
	}
	while (*ptr != RLE_EOL)
	{
		while (to_skip-- > 0)
		{
			if (get_flags (verbosity, VRB_LINE_CONTENT))
				fprintf (stderr, " %2.2X", *ptr);
			ptr++;
		}

		if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
			fprintf (stderr, "\nControl byte encountered : %2.2X", *ptr);

		if (((*ptr) & RLE_DIRECT_B) != 0)
		{
			if( *ptr == RLE_DIRECT_TAIL )
			{
				if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
					fprintf (stderr, " is RLE_DIRECT_TAIL ( %d bytes ) !", im->width-uncopressed_size);
				if (get_flags (verbosity, VRB_LINE_CONTENT))
				{
					to_skip = im->width-uncopressed_size ;
					while (to_skip-- > 0)
					{
						fprintf (stderr, " %2.2X", *ptr);
						ptr++;
					}
				}else
					ptr += im->width-uncopressed_size ;
				break;
			}
			to_skip = 1 + ((*ptr) & (RLE_DIRECT_D));
			uncopressed_size += to_skip;
			if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
				fprintf (stderr, " is RLE_DIRECT !");
		} else if (((*ptr) & RLE_SIMPLE_B_INV) == 0)
		{
			if( *ptr == RLE_EOL )
			{
				if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
					fprintf (stderr, " is RLE_EOL !");
				break;
			}
			if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
				fprintf (stderr, " is RLE_SIMPLE !");
			uncopressed_size += ((int)ptr[0])+ RLE_THRESHOLD;
			to_skip = 1;
		} else if (((*ptr) & RLE_LONG_B) != 0)
		{
			if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
				fprintf (stderr, " is RLE_LONG !");
			uncopressed_size += ((int)ptr[1])+((((int)ptr[0])&RLE_LONG_D ) << 8)+RLE_THRESHOLD;
			to_skip = 1 + 1;
		}
		to_skip++;
		if (get_flags (verbosity, VRB_CTRL_EXPLAIN))
			fprintf (stderr, " to_skip = %d, uncompressed size = %d\n", to_skip, uncopressed_size);
	}
	if (get_flags (verbosity, VRB_LINE_CONTENT))
		fprintf (stderr, " %2.2X\n", *ptr);

	ptr++;

	if (get_flags (verbosity, VRB_LINE_SUMMARY))
		fprintf (stderr, "Row %d, Component %d, Memory Used %ld\n", y, color, (long)(ptr - color_ptr[y]));
	return ptr - color_ptr[y];
}

void print_asimage( ASImage *im, int flags, char * func, int line )
{
	if( im )
	{
		register unsigned int k ;
		int total_mem = 0 ;
		fprintf( stderr, "%s:%d> printing ASImage %p.\n", func, line, im);
		for( k = 0 ; k < im->height ; k++ )
    	{
 			fprintf( stderr, "%s:%d> ******* %d *******\n", func, line, k );
			total_mem+=asimage_print_line( im, IC_RED  , k, flags );
			total_mem+=asimage_print_line( im, IC_GREEN, k, flags );
			total_mem+=asimage_print_line( im, IC_BLUE , k, flags );
            total_mem+=asimage_print_line( im, IC_ALPHA , k, flags );
        }
    	fprintf( stderr, "%s:%d> Total memory : %u - image size %dx%d ratio %d%%\n", func, line, total_mem, im->width, im->height, (total_mem*100)/(im->width*im->height*3) );
	}else
		fprintf( stderr, "%s:%d> Attempted to print NULL ASImage.\n", func, line);
}



inline static CARD32*
asimage_decode_block32 (register CARD8 *src, CARD32 *to_buf, unsigned int width )
{
	register CARD32 *dst = to_buf;
	while ( *src != RLE_EOL)
	{
		if( src[0] == RLE_DIRECT_TAIL )
		{
			register int i = width - (dst-to_buf) ;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
			break;
		}else if( ((*src)&RLE_DIRECT_B) != 0 )
		{
			register int i = (((int)src[0])&RLE_DIRECT_D) + 1;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
		}else if( ((*src)&RLE_LONG_B) != 0 )
		{
			register int i = ((((int)src[0])&RLE_LONG_D)<<8|src[1]) + RLE_THRESHOLD;
			dst += i ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[2] ;
				++i ;
			}
			src += 3;
		}else
		{
			register int i = (int)src[0] + RLE_THRESHOLD ;
			dst += i ;
			i = -i;
			while( i < 0 )
			{
				dst[i] = src[1] ;
				++i ;
			}
			src += 2;
		}
	}
	return dst;
}

inline static CARD8*
asimage_decode_block8 (register CARD8 *src, CARD8 *to_buf, unsigned int width )
{
	register CARD8 *dst = to_buf;
	while ( *src != RLE_EOL)
	{
		if( src[0] == RLE_DIRECT_TAIL )
		{
			register int i = width - (dst-to_buf) ;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
			break;
		}else if( ((*src)&RLE_DIRECT_B) != 0 )
		{
			register int i = (((int)src[0])&RLE_DIRECT_D) + 1;
			dst += i ;
			src += i+1 ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[i] ;
				++i ;
			}
		}else if( ((*src)&RLE_LONG_B) != 0 )
		{
			register int i = ((((int)src[0])&RLE_LONG_D)<<8|src[1]) + RLE_THRESHOLD;
			dst += i ;
			i = -i ;
			while( i < 0 )
			{
				dst[i] = src[2] ;
				++i ;
			}
			src += 3;
		}else
		{
			register int i = (int)src[0] + RLE_THRESHOLD ;
			dst += i ;
			i = -i;
			while( i < 0 )
			{
				dst[i] = src[1] ;
				++i ;
			}
			src += 2;
		}
	}
	return dst;
}

void print_component( register CARD32 *data, int nonsense, int len );

inline int
asimage_decode_line (ASImage * im, ColorPart color, CARD32 * to_buf, unsigned int y, unsigned int skip, unsigned int out_width)
{
	register CARD8  *src = im->channels[color][y];
	/* that thing below is supposedly highly optimized : */
LOCAL_DEBUG_CALLER_OUT( "im->width = %d, color = %d, y = %d, skip = %d, out_width = %d, src = %p", im->width, color, y, skip, out_width, src );
	if( src )
	{
		register int i = 0;
#if 1
  		if( skip > 0 || out_width+skip < im->width)
		{
			int max_i ;
			CARD8 *buffer = get_compression_buffer( im->width );

			asimage_decode_block8( src, buffer, im->width );
			skip = skip%im->width ;
			max_i = MIN(out_width,im->width-skip);
			src = buffer+skip ;
			while( i < (int)out_width )
			{
				while( i < max_i )
				{
					to_buf[i] = src[i] ;
					++i ;
				}
				src = buffer-i ;
				max_i = MIN(out_width,im->width+i) ;
			}
		}else
		{
#endif
			i = asimage_decode_block32( src, to_buf, im->width ) - to_buf;
            LOCAL_DEBUG_OUT( "decoded %d pixels", i );
#if defined(LOCAL_DEBUG) && !defined(NO_DEBUG_OUTPUT)
            {
                int z = -1 ;
                while( ++z < i )
                    fprintf( stderr, "%X ", src[z] );
                fprintf( stderr, "\n");

            }
#endif
#if 1
	  		while( i < (int)out_width )
			{   /* tiling code : */
				register CARD32 *src = to_buf-i ;
				int max_i = MIN(out_width,im->width+i);
				while( i < max_i )
				{
					to_buf[i] = src[i] ;
					++i ;
				}
			}
#endif
		}
		return i;
	}
	return 0;
}


inline CARD8*
asimage_copy_line (register CARD8 *src, int width)
{
	register int i = 0;

	/* merely copying the data */
	if ( src == NULL )
		return NULL;
	while (src[i] != RLE_EOL && width )
	{
		if ((src[i] & RLE_DIRECT_B) != 0)
		{
			if( src[i] == RLE_DIRECT_TAIL )
			{
				i += width+1 ;
				break;
			}else
			{
				register int to_skip = (src[i] & (RLE_DIRECT_D))+1;
				width -= to_skip ;
				i += to_skip ;
			}
		} else if ((src[i]&RLE_SIMPLE_B_INV) == 0)
		{
			width -= ((int)src[i])+ RLE_THRESHOLD;
			++i ;
		} else if ((src[i] & RLE_LONG_B) != 0)
		{
			register int to_skip = ((((int)src[i])&RLE_LONG_D ) << 8) ;
			width -= ((int)src[++i])+to_skip+RLE_THRESHOLD;
			++i;
		}
		++i;
	}
	if( i > 0 )
	{
		CARD8 *res = safemalloc( i+1 );
		memcpy( res, src, i+1 );
		return res;
	}else
		return NULL ;
}

unsigned int
asimage_threshold_line( CARD8 *src, unsigned int width, unsigned int *runs, unsigned int threshold )
{
	register int i = 0;
	int runs_count = 0 ;
	int start = -1, end = -1, curr_x = 0 ;

	/* merely copying the data */
	if ( src == NULL )
		return 0;
	runs[0] = runs[1] = 0 ;
	while (src[i] != RLE_EOL && curr_x < (int)width )
	{
		if ((src[i] & RLE_DIRECT_B) != 0)
		{
			int stop ;
			if( src[i] == RLE_DIRECT_TAIL )
				stop = i + 1 + (width-curr_x) ;
			else
				stop = i + 1 + (src[i] & (RLE_DIRECT_D))+1;
#ifdef DEBUG_RECTS2			
			fprintf( stderr, "%d: i = %d, stop = %d\n", __LINE__, i, stop ); 
#endif				
			while( ++i < stop  )
			{
				if( src[i] >= threshold )
				{
					if( start < 0 )
						start = curr_x ;
					if( end < 0 )
						end = curr_x ;
					else
						++end ;
				}else
				{
					if( start >= 0 && end >= start )
					{
						runs[runs_count] = start ;
						++runs_count;
						runs[runs_count] = end ;
						++runs_count ;
					}
					start = end = -1 ;
				}
				++curr_x ;
#ifdef DEBUG_RECTS2			     				
				fprintf( stderr, "%d: src[%d] = %d, curr_x = %d, start = %d, end = %d, runs_count = %d\n",
  					     __LINE__, i, (int)src[i], curr_x, start, end, runs_count );
#endif
  
			}
		} else
		{
			int len = 0 ;

			if ((src[i]&RLE_SIMPLE_B_INV) == 0)
			{
#ifdef DEBUG_RECTS2			   
			fprintf( stderr, "%d: SimpleRLE: i = %d, src[i] = %d(%2.2X)\n", __LINE__, i, src[i], src[i] ); 
#endif
				len = ((int)src[i])+ RLE_THRESHOLD;
				++i ;
			} else if ((src[i] & RLE_LONG_B) != 0)
			{
#ifdef DEBUG_RECTS2			   
			fprintf( stderr, "%d: LongRLE: i = %d, src[i] = %d(%2.2X), src[i] = %d(%2.2X)\n", __LINE__, i, src[i], src[i], src[i+1], src[i+1] ); 
#endif
				len = ((((int)src[i])&RLE_LONG_D ) << 8) ;
				++i;
				len += ((int)src[i])+RLE_THRESHOLD;
				++i;
			}
#ifdef DEBUG_RECTS2			   
			fprintf( stderr, "%d: i = %d, len = %d, src[i] = %d\n", __LINE__, i, len, src[i] ); 
#endif

			if( src[i] >= threshold )
			{
				if( start < 0 )
					start = curr_x ;
				if( end < 0 )
					end = curr_x-1 ;
				end += len ;
			}else
			{
				if( start >= 0 && end >= start )
				{
					runs[runs_count] = start ;
					++runs_count;
					runs[runs_count] = end ;
					++runs_count ;
				}
				start = end = -1 ;
			}
			curr_x += len ;
#ifdef DEBUG_RECTS2			   
			fprintf( stderr, "%d: src[%d] = %d, curr_x = %d, start = %d, end = %d, runs_count = %d\n",
 					    __LINE__, i, (int)src[i], curr_x, start, end, runs_count );
#endif
			++i;
		}
	}
	if( start >= 0 && end >= start )
	{
		runs[runs_count] = start ;
		++runs_count;
		runs[runs_count] = end ;
		++runs_count ;
	}

#ifdef DEBUG_RECTS2			   
	for( i = 0 ; i < runs_count ; ++i, ++i )
		fprintf( stderr, "\trun[%d] = %d ... %d\n", i, runs[i], runs[i+1] );
#endif

	return runs_count;
}

void
move_asimage_channel( ASImage *dst, int channel_dst, ASImage *src, int channel_src )
{
	if( !AS_ASSERT(dst) && !AS_ASSERT(src) && channel_src >= 0 && channel_src < IC_NUM_CHANNELS &&
		channel_dst >= 0 && channel_dst < IC_NUM_CHANNELS )
	{
		if( dst->width == src->width )
		{
			register int i = MIN(dst->height, src->height);
			register CARD8 **dst_rows = dst->channels[channel_dst] ;
			register CARD8 **src_rows = src->channels[channel_src] ;
			while( --i >= 0 )
			{
				if( dst_rows[i] )
					free( dst_rows[i] );
				dst_rows[i] = src_rows[i] ;
				src_rows[i] = NULL ;
			}
		}else
			show_debug( __FILE__,"move_asimage_channel",__LINE__, "images size differ : %d and %d", src->width, dst->width );
	}
}


void
copy_asimage_channel( ASImage *dst, int channel_dst, ASImage *src, int channel_src )
{
	if( !AS_ASSERT(dst) && !AS_ASSERT(src) && channel_src >= 0 && channel_src < IC_NUM_CHANNELS &&
		channel_dst >= 0 && channel_dst < IC_NUM_CHANNELS )
		if( dst->width == src->width )
		{
			register int i = MIN(dst->height, src->height);
			register CARD8 **dst_rows = dst->channels[channel_dst] ;
			register CARD8 **src_rows = src->channels[channel_src] ;
			while( --i >= 0 )
			{
				if( dst_rows[i] )
					free( dst_rows[i] );
				dst_rows[i] = asimage_copy_line( src_rows[i], dst->width );
			}
		}
}

void
copy_asimage_lines( ASImage *dst, unsigned int offset_dst,
                    ASImage *src, unsigned int offset_src,
					unsigned int nlines, ASFlagType filter )
{
	if( !AS_ASSERT(dst) && !AS_ASSERT(src) &&
		offset_src < src->height && offset_dst < dst->height &&
		dst->width == src->width )
	{
		int chan;

		if( offset_src+nlines > src->height )
			nlines = src->height - offset_src ;
		if( offset_dst+nlines > dst->height )
			nlines = dst->height - offset_dst ;

		for( chan = 0 ; chan < IC_NUM_CHANNELS ; ++chan )
			if( get_flags( filter, 0x01<<chan ) )
			{
				register int i = -1;
				register CARD8 **dst_rows = &(dst->channels[chan][offset_dst]) ;
				register CARD8 **src_rows = &(src->channels[chan][offset_src]) ;
LOCAL_DEBUG_OUT( "copying %d lines of channel %d...", nlines, chan );
				while( ++i < (int)nlines )
				{
					if( dst_rows[i] )
						free( dst_rows[i] );
					dst_rows[i] = asimage_copy_line( src_rows[i], dst->width );
				}
			}
#if 0
		for( i = 0 ; i < nlines ; ++i )
		{
			asimage_print_line( src, IC_ALPHA, i, (i==4)?VRB_EVERYTHING:VRB_LINE_SUMMARY );
			asimage_print_line( dst, IC_ALPHA, i, (i==4)?VRB_EVERYTHING:VRB_LINE_SUMMARY );
		}
#endif
	}
}

Bool
asimage_compare_line (ASImage *im, ColorPart color, CARD32 *to_buf, CARD32 *tmp, unsigned int y, Bool verbose)
{
	register unsigned int i;
	asimage_decode_line( im, color, tmp, y, 0, im->width );
	for( i = 0 ; i < im->width ; i++ )
		if( tmp[i] != to_buf[i] )
		{
			if( verbose )
				show_error( "line %d, component %d differ at offset %d ( 0x%lX(compresed) != 0x%lX(orig) )\n", y, color, i, tmp[i], to_buf[i] );
			return False ;
		}
	return True;
}

/* for consistency sake : */
inline void
copy_component( register CARD32 *src, register CARD32 *dst, int *unused, int len )
{
#if 1
#ifdef CARD64
	CARD64 *dsrc = (CARD64*)src;
	CARD64 *ddst = (CARD64*)dst;
#else
	double *dsrc = (double*)src;
	double *ddst = (double*)dst;
#endif
	register int i = 0;

	len += len&0x01;
	len = len>>1 ;
	do
	{
		ddst[i] = dsrc[i];
	}while(++i < len );
#else
	register int i = 0;

	len += len&0x01;
	do
	{
		dst[i] = src[i];
	}while(++i < len );
#endif
}

static inline int
set_component( register CARD32 *src, register CARD32 value, int offset, int len )
{
	register int i ;
	for( i = offset ; i < len ; ++i )
		src[i] = value;
	return len-offset;
}



static inline void
divide_component( register CARD32 *src, register CARD32 *dst, CARD16 ratio, int len )
{
	register int i = 0;
	len += len&0x00000001;                     /* we are 8byte aligned/padded anyways */
	if( ratio == 2 )
	{
#ifdef HAVE_MMX
		if( asimage_use_mmx )
		{
			double *ddst = (double*)&(dst[0]);
			double *dsrc = (double*)&(src[0]);
			len = len>>1;
			do{
				asm volatile
       		    (
            		"movq %1, %%mm0  \n\t" // load 8 bytes from src[i] into MM0
            		"psrld $1, %%mm0 \n\t" // MM0=src[i]>>1
            		"movq %%mm0, %0  \n\t" // store the result in dest
					: "=m" (ddst[i]) // %0
					: "m"  (dsrc[i]) // %1
	            );
			}while( ++i < len );
		}else
#endif
			do{
				dst[i] = src[i] >> 1;
				dst[i+1] = src[i+1]>> 1;
				i += 2 ;
			}while( i < len );
	}else
	{
		do{
			register int c1 = src[i];
			register int c2 = src[i+1];
			dst[i] = c1/ratio;
			dst[i+1] = c2/ratio;
			i+=2;
		}while( i < len );
	}
}

/* diffusingly combine src onto self and dst, and rightbitshift src by quantization shift */
static inline void
best_output_filter( register CARD32 *line1, register CARD32 *line2, int unused, int len )
{/* we carry half of the quantization error onto the surrounding pixels : */
 /*        X    7/16 */
 /* 3/16  5/16  1/16 */
	register int i ;
	register CARD32 errp = 0, err = 0, c;
	c = line1[0];
	if( (c&0xFFFF0000)!= 0 )
		c = ( c&0x7F000000 )?0:0x0000FFFF;
	errp = c&QUANT_ERR_MASK;
	line1[0] = c>>QUANT_ERR_BITS ;
	line2[0] += (errp*5)>>4 ;

	for( i = 1 ; i < len ; ++i )
	{
		c = line1[i];
		if( (c&0xFFFF0000)!= 0 )
			c = (c&0x7F000000)?0:0x0000FFFF;
		c += ((errp*7)>>4) ;
		err = c&QUANT_ERR_MASK;
		line1[i] = (c&0x7FFF0000)?0x000000FF:(c>>QUANT_ERR_BITS);
		line2[i-1] += (err*3)>>4 ;
		line2[i] += ((err*5)>>4)+(errp>>4);
		errp = err ;
	}
}

static inline void
fine_output_filter( register CARD32 *src, register CARD32 *dst, short ratio, int len )
{/* we carry half of the quantization error onto the following pixel and store it in dst: */
	register int i = 0;
	if( ratio <= 1 )
	{
		register int c = src[0];
  	    do
		{
			if( (c&0xFFFF0000)!= 0 )
				c = ( c&0x7F000000 )?0:0x0000FFFF;
			dst[i] = c>>(QUANT_ERR_BITS) ;
			if( ++i >= len )
				break;
			c = ((c&QUANT_ERR_MASK)>>1)+src[i];
		}while(1);
	}else if( ratio == 2 )
	{
		register CARD32 c = src[0];
  	    do
		{
			c = c>>1 ;
			if( (c&0xFFFF0000) != 0 )
				c = ( c&0x7F000000 )?0:0x0000FFFF;
			dst[i] = c>>(QUANT_ERR_BITS) ;
			if( ++i >= len )
				break;
			c = ((c&QUANT_ERR_MASK)>>1)+src[i];
		}while( 1 );
	}else
	{
		register CARD32 c = src[0];
  	    do
		{
			c = c/ratio ;
			if( c&0xFFFF0000 )
				c = ( c&0x7F000000 )?0:0x0000FFFF;
			dst[i] = c>>(QUANT_ERR_BITS) ;
			if( ++i >= len )
				break;
			c = ((c&QUANT_ERR_MASK)>>1)+src[i];
		}while(1);
	}
}

static inline void
fast_output_filter( register CARD32 *src, register CARD32 *dst, short ratio, int len )
{/*  no error diffusion whatsoever: */
	register int i = 0;
	if( ratio <= 1 )
	{
		for( ; i < len ; ++i )
		{
			register CARD32 c = src[i];
			if( (c&0xFFFF0000) != 0 )
				dst[i] = ( c&0x7F000000 )?0:0x000000FF;
			else
				dst[i] = c>>(QUANT_ERR_BITS) ;
		}
	}else if( ratio == 2 )
	{
		for( ; i < len ; ++i )
		{
			register CARD32 c = src[i]>>1;
			if( (c&0xFFFF0000) != 0 )
				dst[i] = ( c&0x7F000000 )?0:0x000000FF;
			else
				dst[i] = c>>(QUANT_ERR_BITS) ;
		}
	}else
	{
		for( ; i < len ; ++i )
		{
			register CARD32 c = src[i]/ratio;
			if( (c&0xFFFF0000) != 0 )
				dst[i] = ( c&0x7F000000 )?0:0x000000FF;
			else
				dst[i] = c>>(QUANT_ERR_BITS) ;
		}
	}
}

static inline void
fine_output_filter_mod( register CARD32 *data, int unused, int len )
{/* we carry half of the quantization error onto the following pixel : */
	register int i ;
	register CARD32 err = 0, c;
	for( i = 0 ; i < len ; ++i )
	{
		c = data[i];
		if( (c&0xFFFF0000) != 0 )
			c = ( c&0x7E000000 )?0:0x0000FFFF;
		c += err;
		err = (c&QUANT_ERR_MASK)>>1 ;
		data[i] = (c&0x00FF0000)?0x000000FF:c>>QUANT_ERR_BITS ;
	}
}

/* *********************************************************************/
/*					    	 DECODER : 	   							  */
/* Low level drivers :                                                */
static void
decode_asscanline_native( ASImageDecoder *imdec, unsigned int skip, int y )
{
	int i ;
	ASScanline *scl = &(imdec->buffer);
	int count, width = scl->width-skip ;
	for( i = 0 ; i < IC_NUM_CHANNELS ; i++ )
		if( get_flags(imdec->filter, 0x01<<i) )
		{
			register CARD32 *chan = scl->channels[i]+skip;
			if( imdec->im )
				count = asimage_decode_line(imdec->im, i, chan, y, imdec->offset_x, width);
			else
				count = 0 ;
			if( scl->shift )
			{
				register int k  = 0;
				for(; k < count ; k++ )
					chan[k] = chan[k]<<8;
			}
			if( count < width )
				set_component( chan, ARGB32_CHAN8(imdec->back_color, i)<<scl->shift, count, width );
		}

	clear_flags( scl->flags, SCL_DO_ALL);
	set_flags( scl->flags, imdec->filter);
}

static void
decode_asscanline_ximage( ASImageDecoder *imdec, unsigned int skip, int y )
{
	int i ;
	ASScanline *scl = &(imdec->buffer);
	XImage *xim = imdec->im->alt.ximage ;
	int count, width = scl->width-skip, xim_width = xim->width ;
	ASFlagType filter = imdec->filter ;
#if 1
	if( width > xim_width || imdec->offset_x > 0 )
	{/* need to tile :( */
		ASScanline *xim_scl = imdec->xim_buffer;
		int offset_x = imdec->offset_x%xim_width ;
/*fprintf( stderr, __FILE__ ":" __FUNCTION__ ": width=%d, xim_width=%d, skip = %d, offset_x = %d - tiling\n", width, xim->width, skip, imdec->offset_x );	*/

		GET_SCANLINE(imdec->asv,xim,xim_scl,y,xim->data+xim->bytes_per_line*y);
		/* We also need to decode mask if we have one :*/
		if( (xim = imdec->im->alt.mask_ximage ) != NULL )
		{
#ifndef X_DISPLAY_MISSING
			CARD32 *dst = xim_scl->alpha ;
			register int x = MIN((int)xim_scl->width,xim->width);
			if( xim->depth == 8 )
			{
				CARD8  *src = xim->data+xim->bytes_per_line*y ;
				while(--x >= 0 ) dst[x] = (CARD32)(src[x]);
			}else
			{
				while(--x >= 0 ) dst[x] = (XGetPixel(xim, x, y) == 0)?0x00:0xFF;
			}
#endif
		}
		for( i = 0 ; i < IC_NUM_CHANNELS ; i++ )
			if( get_flags(filter, 0x01<<i) )
			{
				register CARD32 *src = xim_scl->channels[i]+offset_x ;
				register CARD32 *dst = scl->channels[i]+skip;
				register int k  = 0;
				count = xim_width-offset_x ;
				if( count > width )
					count = width ;

#define COPY_TILE_CHAN(op) \
		for(; k < count ; k++ )	dst[k] = op; \
		while( k < width ) \
		{	src = xim_scl->channels[i]-k ; \
			count = MIN(xim_width+k,width); \
			for(; k < count ; k++ ) dst[k] = op; \
		}

				if( scl->shift )
				{
					COPY_TILE_CHAN(src[k]<<8)
				}else
				{
					COPY_TILE_CHAN(src[k])
				}
				count += k ;
				if( count < width )
					set_component( src, ARGB32_CHAN8(imdec->back_color, i)<<scl->shift, count, width );
			}
	}else
#endif
	{/* cool we can put data directly into buffer : */
/*fprintf( stderr, __FILE__ ":" __FUNCTION__ ":direct\n" );	*/
		int old_offset = scl->offset_x ;
		scl->offset_x = skip ;
		GET_SCANLINE(imdec->asv,xim,scl,y,xim->data+xim->bytes_per_line*y);
		/* We also need to decode mask if we have one :*/
		if( (xim = imdec->im->alt.mask_ximage ) != NULL )
		{
#ifndef X_DISPLAY_MISSING
			CARD32 *dst = scl->alpha+skip ;
			register int x = MIN(width,xim_width);
			if( xim->depth == 8 )
			{
				CARD8  *src = xim->data+xim->bytes_per_line*y ;
				while(--x >= 0 ) dst[x] = (CARD32)(src[x]);
			}else
			{
				while(--x >= 0 ) dst[x] = (XGetPixel(xim, x, y) == 0)?0x00:0xFF;
			}
#endif
		}
		count = MIN(width,xim_width);
		scl->offset_x = old_offset ;
		for( i = 0 ; i < IC_NUM_CHANNELS ; i++ )
			if( get_flags(filter, 0x01<<i) )
			{
				register CARD32 *chan = scl->channels[i]+skip;
				if( scl->shift )
				{
					register int k  = 0;
					for(; k < count ; k++ )
						chan[k] = chan[k]<<8;
				}
				if( count < width )
					set_component( chan, ARGB32_CHAN8(imdec->back_color, i)<<scl->shift, count, width );
			}
	}
	clear_flags( scl->flags,SCL_DO_ALL);
	set_flags( scl->flags,imdec->filter);
}

/***********************************************************************/
/* High level drivers :                                                */
void                                           /* normal (unbeveled) */
decode_image_scanline_normal( ASImageDecoder *imdec )
{
	int 	 			 y = imdec->next_line;

	if( y - imdec->offset_y >= imdec->out_height )
	{
		imdec->buffer.flags = 0 ;
		imdec->buffer.back_color = imdec->back_color ;
		return ;
	}

	if( imdec->im )
		y %= imdec->im->height;
	imdec->decode_asscanline( imdec, 0, y );
	++(imdec->next_line);
}

static inline void
draw_solid_bevel_line( register ASScanline *scl, int alt_left, int hi_end, int lo_start, int alt_right,
					   ARGB32 bevel_color, ARGB32 shade_color, ARGB32 hi_corner, ARGB32 lo_corner )
{
	int channel ;
	for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
		if( get_flags(scl->flags, (0x01<<channel)) )
		{
			if( hi_end > 0 )
			{
				set_component( scl->channels[channel],
						        ARGB32_CHAN8(bevel_color,channel)<<scl->shift,
								0, hi_end );
				if( alt_left > 0 )
					scl->channels[channel][alt_left-1] =
						        ARGB32_CHAN8(hi_corner,channel)<<scl->shift ;
			}
			if( lo_start < (int)scl->width )
			{
				set_component( scl->channels[channel],
						        ARGB32_CHAN8(shade_color,channel)<<scl->shift,
						        lo_start, scl->width );
				if( alt_right < (int)scl->width && alt_right > 0 )
					scl->channels[channel][scl->width - alt_right] =
					            ARGB32_CHAN8(lo_corner,channel)<<scl->shift ;
			}
		}
}
static inline void
draw_fading_bevel_sides( ASImageDecoder *imdec,
					     int left_margin, int left_delta,
					     int right_delta, int right_margin )
{
	register ASScanline *scl = &(imdec->buffer);
	ASImageBevel *bevel = imdec->bevel ;
	CARD32 ha_bevel = ARGB32_ALPHA8(bevel->hi_color);
	CARD32 ha_shade = ARGB32_ALPHA8(bevel->lo_color);
    CARD32 hda_bevel = (ha_bevel<<8)/(bevel->left_inline+1) ;
    CARD32 hda_shade = (ha_shade<<8)/(bevel->right_inline+1);
	int channel ;

	for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
		if( get_flags(scl->flags, (0x01<<channel)) )
		{
			CARD32 chan_col = ARGB32_CHAN8(bevel->hi_color,channel)<<scl->shift ;
			register CARD32 ca = hda_bevel*(left_delta+1) ;
			register int i = MIN((int)scl->width, imdec->bevel_left+(int)bevel->left_inline-left_delta);
			CARD32 *chan_img_start = scl->channels[channel] ;

			while( --i >= left_margin )
			{
				chan_img_start[i] = (chan_img_start[i]*(255-(ca>>8))+chan_col*(ca>>8))>>8 ;
				ca += hda_bevel ;
			}
			ca = hda_shade*(right_delta+1) ;
			i =  MAX( left_margin, imdec->bevel_right + right_delta - (int)bevel->right_inline);
			chan_col = ARGB32_CHAN8(bevel->lo_color,channel)<<scl->shift ;
			while( ++i < right_margin )
			{
				chan_img_start[i] = (chan_img_start[i]*(255-(ca>>8))+chan_col*(ca>>8))>>8 ;
				ca += hda_shade ;
			}
		}
}

static inline void
draw_transp_bevel_sides( ASImageDecoder *imdec,
					     int left_margin, int left_delta,
					     int right_delta, int right_margin )
{
	register ASScanline *scl = &(imdec->buffer);
	ASImageBevel *bevel = imdec->bevel ;
	CARD32 ha_bevel = ARGB32_ALPHA8(bevel->hi_color)>>1;
	CARD32 ha_shade = ARGB32_ALPHA8(bevel->lo_color)>>1;
	int channel ;

	for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
		if( get_flags(scl->flags, (0x01<<channel)) )
		{
			CARD32 chan_col = (ARGB32_CHAN8(bevel->hi_color,channel)<<scl->shift)*ha_bevel ;
			register CARD32 ca = 255-ha_bevel ;
			register int i = imdec->bevel_left+(int)bevel->left_inline-left_delta;
			CARD32 *chan_img_start = scl->channels[channel] ;

			while( --i >= left_margin )
				chan_img_start[i] = (chan_img_start[i]*ca+chan_col)>>8 ;

			ca = 255-ha_shade ;
			i =  MAX( left_margin, imdec->bevel_right + right_delta - (int)bevel->right_inline);
			chan_col = (ARGB32_CHAN8(bevel->lo_color,channel)<<scl->shift)*ha_shade ;
			while( ++i < right_margin )
				chan_img_start[i] = (chan_img_start[i]*ca+chan_col)>>8 ;
		}
}


static inline void
draw_transp_bevel_line ( ASImageDecoder *imdec,
					     int left_delta, int right_delta,
						 CARD32 ca,
						 ARGB32 left_color, ARGB32 color, ARGB32 right_color )
{
	register ASScanline *scl = &(imdec->buffer);
	ASImageBevel *bevel = imdec->bevel ;
	int start_point = imdec->bevel_left+(int)bevel->left_inline-left_delta;
	int end_point   = imdec->bevel_right + right_delta - (int)bevel->right_inline;
	int channel ;
	CARD32 rev_ca = (255-(ca>>8));

	if( start_point < (int)scl->width && end_point > 0 )
	{
		for( channel = 0 ; channel < ARGB32_CHANNELS ; ++channel )
			if( get_flags(scl->flags, (0x01<<channel)) )
			{
				CARD32 chan_col = (ARGB32_CHAN8(color,channel)<<scl->shift)*(ca>>8) ;
				CARD32 *chan_img_start = scl->channels[channel] ;
				register int i ;
				int end_i;

				if( start_point < 0 )
					i = -1 ;
				else
				{
					i = start_point-1 ;
					if( i < (int)scl->width )
						chan_img_start[i] = (chan_img_start[i]*rev_ca + ARGB32_CHAN8(left_color,channel)*(ca>>8))>>8 ;
				}
				if( end_point >= (int)scl->width )
					end_i = scl->width ;
				else
				{
					end_i = end_point ;
					chan_img_start[end_i] = (chan_img_start[end_i]*rev_ca + ARGB32_CHAN8(right_color,channel)*(ca>>8))>>8 ;
				}
				while( ++i < end_i )
					chan_img_start[i] = (chan_img_start[i]*rev_ca+chan_col)>>8;
			}
	}
}

void
decode_image_scanline_beveled( ASImageDecoder *imdec )
{
	register ASScanline *scl = &(imdec->buffer);
	int 	 			 y_out = imdec->next_line- (int)imdec->offset_y;
	register ASImageBevel *bevel = imdec->bevel ;
	ARGB32 bevel_color = bevel->hi_color, shade_color = bevel->lo_color;
	int offset_shade = 0;

	scl->flags = 0 ;
	if( y_out < 0 || y_out > (int)imdec->out_height+imdec->bevel_v_addon )
	{
		scl->back_color = imdec->back_color ;
		return ;
	}

	set_flags( scl->flags, imdec->filter );
	if( y_out < imdec->bevel_top )
	{
		if( bevel->top_outline > 0 )
		{
			register int line = y_out - (imdec->bevel_top - (int)bevel->top_outline);
			int alt_left  = (line*bevel->left_outline/bevel->top_outline)+1 ;
			int alt_right = (line*bevel->right_outline/bevel->top_outline)+1 ;

			alt_left += MAX(imdec->bevel_left-(int)bevel->left_outline,0) ;
			offset_shade = MAX(imdec->bevel_right+(int)bevel->right_outline-alt_right,0);

/*		fprintf( stderr, __FUNCTION__ " %d: y_out = %d, alt_left = %d, offset_shade = %d, alt_right = %d, scl->width = %d, out_width = %d\n",
					 	__LINE__, y_out, alt_left, offset_shade, alt_right, scl->width, imdec->out_width );
  */
			if( (int)scl->width < imdec->bevel_right )
				alt_right -= imdec->bevel_right-(int)scl->width ;
			if( offset_shade > (int)scl->width )
				offset_shade = scl->width ;
			draw_solid_bevel_line( scl, alt_left, offset_shade, offset_shade, alt_right,
							   	bevel->hi_color, bevel->lo_color, bevel->hihi_color, bevel->hilo_color );
		}
	}else if( y_out >= imdec->bevel_bottom )
	{
		if( bevel->bottom_outline > 0 )
		{
			register int line = bevel->bottom_outline - (y_out - imdec->bevel_bottom);
			int alt_left  = (line*bevel->left_outline/bevel->bottom_outline)+1 ;
			int alt_right = (line*bevel->right_outline/bevel->bottom_outline)+1 ;

			alt_left += MAX(imdec->bevel_left-(int)bevel->left_outline,0) ;
			offset_shade = MIN(alt_left, (int)scl->width );

			if( (int)scl->width < imdec->bevel_right )
				alt_right -= imdec->bevel_right-(int)scl->width ;

/*	fprintf( stderr, __FUNCTION__ " %d: y_out = %d, alt_left = %d, offset_shade = %d, alt_right = %d, scl->width = %d, out_width = %d\n",
					 __LINE__, y_out, alt_left, offset_shade, alt_right, scl->width, imdec->out_width );
  */
			set_flags( scl->flags, imdec->filter );
			draw_solid_bevel_line( scl, alt_left, alt_left, alt_left, alt_right,
							   	bevel->hi_color, bevel->lo_color,
							   	bevel->hilo_color, bevel->lolo_color );
		}
	}else
	{
		int left_margin = MAX(0, imdec->bevel_left);
		int right_margin = MIN((int)scl->width, imdec->bevel_right);
		int y = imdec->next_line-bevel->top_outline ;
		if( imdec->im )
			y %= imdec->im->height ;

		if( left_margin < (int)scl->width )
			imdec->decode_asscanline( imdec, left_margin, y );

		draw_solid_bevel_line( scl, -1, left_margin, right_margin, scl->width,
							   bevel->hi_color, bevel->lo_color,
							   bevel->hilo_color, bevel->lolo_color );

		if( left_margin < (int)scl->width )
		{
			if( get_flags( bevel->type, BEVEL_SOLID_INLINE ) )
			{
				if( y_out < imdec->bevel_top+(int)bevel->top_inline)
				{
					register int line = y_out - imdec->bevel_top;
					int left_delta  = bevel->left_inline-((line*bevel->left_inline/bevel->top_inline)) ;
					int right_delta = bevel->right_inline-((line*bevel->right_inline/bevel->top_inline)-1) ;

					draw_transp_bevel_sides( imdec, left_margin, left_delta,
									 	 	right_delta, right_margin );
					draw_transp_bevel_line ( imdec, left_delta-1, right_delta-1,
						 			 		ARGB32_ALPHA8(bevel_color)<<7,
									 		bevel->hihi_color, bevel->hi_color, bevel->hilo_color );

				}else if( y_out >= imdec->bevel_bottom - bevel->bottom_inline)
				{
					register int line = y_out - (imdec->bevel_bottom - bevel->bottom_inline);
					int left_delta  = (line*bevel->left_inline/bevel->bottom_inline)+1 ;
					int right_delta = (line*bevel->right_inline/bevel->bottom_inline)-1 ;

					draw_transp_bevel_sides( imdec,	left_margin, left_delta,
									 		right_delta, right_margin );
					draw_transp_bevel_line ( imdec, left_delta-1, right_delta,
						 			 		ARGB32_ALPHA8(shade_color)<<7,
									 		bevel->hilo_color, bevel->lo_color, bevel->lolo_color );

				}else
				{
					draw_transp_bevel_sides( imdec, left_margin, 0, 0, right_margin );
				}

			}else
			{
/*fprintf( stderr, __FUNCTION__ ":%d: y_out = %d, imdec->bevel_top = %d, bevel->top_inline = %d\n",
				__LINE__,  y_out, imdec->bevel_top, bevel->top_inline);
 */
				if( y_out < imdec->bevel_top+bevel->top_inline)
				{
					register int line = y_out - imdec->bevel_top;
					int left_delta  = bevel->left_inline-((line*bevel->left_inline/bevel->top_inline)) ;
					int right_delta = bevel->right_inline-((line*bevel->right_inline/bevel->top_inline)-1) ;
	    			CARD32 hda_bevel = (ARGB32_ALPHA8(bevel_color)<<8)/(bevel->left_inline+1) ;

					draw_fading_bevel_sides( imdec,	left_margin, left_delta,
									 	 	right_delta, right_margin );
/*fprintf( stderr, __FUNCTION__ ":%d: left_delta = %d, right_delta = %d, left_inline = %d, right_inline = %d, bevel_left = %d, bevel_right = %d\n",
				__LINE__,  left_delta, right_delta, bevel->left_inline, bevel->right_inline, imdec->bevel_left, imdec->bevel_right);
*/
					draw_transp_bevel_line ( imdec, left_delta-1, right_delta-1,
						 			 	 	hda_bevel*(left_delta+1),
									 	 	bevel->hihi_color, bevel->hi_color, bevel->hilo_color );

				}else if( y_out >= imdec->bevel_bottom - bevel->bottom_inline)
				{
					register int line = y_out - (imdec->bevel_bottom - bevel->bottom_inline);
					int left_delta  = (line*bevel->left_inline/bevel->bottom_inline)+1 ;
					int right_delta = (line*bevel->right_inline/bevel->bottom_inline)-1 ;
	    			CARD32 hda_shade = (ARGB32_ALPHA8(shade_color)<<8)/(bevel->right_inline+1) ;

					draw_fading_bevel_sides( imdec, left_margin, left_delta,
									 	 	right_delta, right_margin );
					draw_transp_bevel_line ( imdec, left_delta-1, right_delta,
						 			 	 	hda_shade*(right_delta+1),
									 	 	bevel->hilo_color, bevel->lo_color, bevel->lolo_color );
				}else
				{
					draw_fading_bevel_sides( imdec, left_margin, 0, 0, right_margin );
				}
			}
		}
	}
	++(imdec->next_line);
}

/* *********************************************************************/
/*						  ENCODER : 								  */
inline static void
tile_ximage_line( XImage *xim, unsigned int line, int step, int range )
{
	register int i ;
	int xim_step = step*xim->bytes_per_line ;
	char *src_line = xim->data+xim->bytes_per_line*line ;
	char *dst_line = src_line+xim_step ;
	int max_i = MIN((int)xim->height,(int)line+range), min_i = MAX(0,(int)line-range) ;
	for( i = line+step ; i < max_i && i >= min_i ; i+=step )
	{
		memcpy( dst_line, src_line, xim->bytes_per_line );
		dst_line += xim_step ;
	}
}

void
encode_image_scanline_mask_xim( ASImageOutput *imout, ASScanline *to_store )
{
#ifndef X_DISPLAY_MISSING
	ASImage *im = imout->im ;
	register XImage *xim = im->alt.mask_ximage ;
	if( imout->next_line < xim->height && imout->next_line >= 0 )
	{
		if( get_flags(to_store->flags, SCL_DO_ALPHA) )
		{
			CARD32 *a = to_store->alpha ;
			register int x = MIN((unsigned int)(xim->width), to_store->width);
			if( xim->depth == 8 )
			{
				CARD8 *dst = xim->data+xim->bytes_per_line*imout->next_line ;
				while( --x >= 0 )
					dst[x] = (CARD8)(a[x]);
			}else
			{
				unsigned int nl = imout->next_line ;
				while( --x >= 0 )
					XPutPixel( xim, x, nl, (a[x] >= 0x7F)?1:0 );
			}
		}
		if( imout->tiling_step > 0 )
			tile_ximage_line( xim, imout->next_line,
			                  imout->bottom_to_top*imout->tiling_step,
							  (imout->tiling_range ? imout->tiling_range:imout->im->height) );
		imout->next_line += imout->bottom_to_top;
	}
#endif
}

void
encode_image_scanline_xim( ASImageOutput *imout, ASScanline *to_store )
{
	register XImage *xim = imout->im->alt.ximage ;
	if( imout->next_line < xim->height && imout->next_line >= 0 )
	{
		if( !get_flags(to_store->flags, SCL_DO_RED) )
			set_component( to_store->red, ARGB32_RED8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_GREEN) )
			set_component( to_store->green, ARGB32_GREEN8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_BLUE) )
			set_component( to_store->blue , ARGB32_BLUE8(to_store->back_color), 0, to_store->width );
		PUT_SCANLINE(imout->asv, xim,to_store,imout->next_line,
					 (unsigned char*)xim->data+imout->next_line*xim->bytes_per_line);

		if( imout->tiling_step > 0 )
			tile_ximage_line( imout->im->alt.ximage, imout->next_line,
			                  imout->bottom_to_top*imout->tiling_step,
							  (imout->tiling_range ? imout->tiling_range:imout->im->height) );
		imout->next_line += imout->bottom_to_top;
	}
}

void
encode_image_scanline_asim( ASImageOutput *imout, ASScanline *to_store )
{
LOCAL_DEBUG_CALLER_OUT( "imout->next_line = %d, imout->im->height = %d", imout->next_line, imout->im->height );
	if( imout->next_line < (int)imout->im->height && imout->next_line >= 0 )
	{
		CARD8 chan_fill[4];
		chan_fill[IC_RED]   = ARGB32_RED8  (to_store->back_color);
		chan_fill[IC_GREEN] = ARGB32_GREEN8(to_store->back_color);
		chan_fill[IC_BLUE]  = ARGB32_BLUE8 (to_store->back_color);
		chan_fill[IC_ALPHA] = ARGB32_ALPHA8(to_store->back_color);
		if( imout->tiling_step > 0 )
		{
			int bytes_count ;
			register int i, color ;
			int line = imout->next_line ;
			int range = (imout->tiling_range ? imout->tiling_range:imout->im->height);
			int max_i = MIN((int)imout->im->height,line+range), min_i = MAX(0,line-range) ;
			int step =  imout->bottom_to_top*imout->tiling_step;

			for( color = 0 ; color < IC_NUM_CHANNELS ; color++ )
			{
				if( get_flags(to_store->flags,0x01<<color))
					bytes_count = asimage_add_line(imout->im, color, to_store->channels[color]+to_store->offset_x, line);
				else if( chan_fill[color] != imout->chan_fill[color] )
					bytes_count = asimage_add_line_mono( imout->im, color, (CARD8)chan_fill[color], line);
				else
				{
					asimage_erase_line( imout->im, color, line );
					for( i = line+step ; i < max_i && i >= min_i ; i+=step )
						asimage_erase_line( imout->im, color, i );
					continue;
				}
				for( i = line+step ; i < max_i && i >= min_i ; i+=step )
				{
/*						fprintf( stderr, "copy-encoding color %d, from lline %d to %d, %d bytes\n", color, imout->next_line, i, bytes_count );*/
					asimage_dup_line( imout->im, color, line, i, bytes_count );
				}
			}
		}else
		{
			register int color ;
			for( color = 0 ; color < IC_NUM_CHANNELS ; color++ )
			{
				if( get_flags(to_store->flags,0x01<<color))
				{
					LOCAL_DEBUG_OUT( "encoding line %d for component %d offset = %d ", imout->next_line, color, to_store->offset_x );
					asimage_add_line(imout->im, color, to_store->channels[color]+to_store->offset_x, imout->next_line);
				}else if( chan_fill[color] != imout->chan_fill[color] )
				{
					LOCAL_DEBUG_OUT( "filling line %d for component %d with value %X", imout->next_line, color, chan_fill[color] );
					asimage_add_line_mono( imout->im, color, chan_fill[color], imout->next_line);
				}else
				{
					LOCAL_DEBUG_OUT( "erasing line %d for component %d", imout->next_line, color );
					asimage_erase_line( imout->im, color, imout->next_line );
				}
			}
		}
	}
	imout->next_line += imout->bottom_to_top;
}

inline static void
tile_argb32_line( ARGB32 *data, unsigned int line, int step, unsigned int width, unsigned int height, int range )
{
	register int i ;
	ARGB32 *src_line = data+width*line ;
	ARGB32 *dst_line = src_line+width ;
	int max_i = MIN((int)height,(int)line+range), min_i = MAX(0,(int)line-range) ;

	for( i = line+step ; i < max_i && i >= min_i ; i+=step )
	{
		memcpy( dst_line, src_line, width*sizeof(ARGB32));
		dst_line += step;
	}
}

void
encode_image_scanline_argb32( ASImageOutput *imout, ASScanline *to_store )
{
	register ARGB32 *data = imout->im->alt.argb32 ;
	if( imout->next_line < (int)imout->im->height && imout->next_line >= 0 )
	{
		register int x = imout->im->width;
		register CARD32 *alpha = to_store->alpha ;
		register CARD32 *red = to_store->red ;
		register CARD32 *green = to_store->green ;
		register CARD32 *blue = to_store->blue ;
		if( !get_flags(to_store->flags, SCL_DO_RED) )
			set_component( red, ARGB32_RED8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_GREEN) )
			set_component( green, ARGB32_GREEN8(to_store->back_color), 0, to_store->width );
		if( !get_flags(to_store->flags, SCL_DO_BLUE) )
			set_component( blue , ARGB32_BLUE8(to_store->back_color), 0, to_store->width );

		data += x*imout->next_line ;
		if( !get_flags(to_store->flags, SCL_DO_ALPHA) )
			while( --x >= 0 )
				data[x] = MAKE_ARGB32( 0xFF, red[x], green[x], blue[x] );
		else
			while( --x >= 0 )
				data[x] = MAKE_ARGB32( alpha[x], red[x], green[x], blue[x] );

		if( imout->tiling_step > 0 )
			tile_argb32_line( imout->im->alt.argb32, imout->next_line,
			                  imout->bottom_to_top*imout->tiling_step,
							  imout->im->width, imout->im->height,
							  (imout->tiling_range ? imout->tiling_range:imout->im->height));
		imout->next_line += imout->bottom_to_top;
	}
}


void
output_image_line_top( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	ASScanline *to_store = NULL ;
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
		if( ratio > 1 )
            SCANLINE_FUNC_FILTERED(divide_component,*(new_line),*(imout->available),(CARD8)ratio,imout->available->width);
		else
            SCANLINE_FUNC_FILTERED(copy_component,*(new_line),*(imout->available),NULL,imout->available->width);
		imout->available->flags = new_line->flags ;
		imout->available->back_color = new_line->back_color ;
	}
	/* copying/encoding previously cahced line into destination image : */
	if( imout->used != NULL )
	{
		if( new_line != NULL )
            SCANLINE_FUNC_FILTERED(best_output_filter,*(imout->used),*(imout->available),0,imout->available->width);
		else
            SCANLINE_MOD_FILTERED(fine_output_filter_mod,*(imout->used),0,imout->used->width);
		to_store = imout->used ;
	}
	if( to_store )
    {
		imout->encode_image_scanline( imout, to_store );
    }
	/* rotating the buffers : */
	if( imout->buffer_shift > 0 )
	{
		if( new_line == NULL )
			imout->used = NULL ;
		else
			imout->used = imout->available ;
		imout->available = &(imout->buffer[0]);
		if( imout->available == imout->used )
			imout->available = &(imout->buffer[1]);
	}
}

void
output_image_line_fine( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	/* caching and preprocessing line into our buffer : */
    if( new_line )
	{
        SCANLINE_FUNC_FILTERED(fine_output_filter, *(new_line),*(imout->available),(CARD8)ratio,imout->available->width);
		imout->available->flags = new_line->flags ;
		imout->available->back_color = new_line->back_color ;
/*      SCANLINE_MOD(print_component,*(imout->available),0, new_line->width ); */
		/* copying/encoding previously cached line into destination image : */
		imout->encode_image_scanline( imout, imout->available );
	}
}

void
output_image_line_fast( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
        SCANLINE_FUNC_FILTERED(fast_output_filter,*(new_line),*(imout->available),(CARD8)ratio,imout->available->width);
		imout->available->flags = new_line->flags ;
		imout->available->back_color = new_line->back_color ;
		imout->encode_image_scanline( imout, imout->available );
	}
}

void
output_image_line_direct( ASImageOutput *imout, ASScanline *new_line, int ratio )
{
	/* caching and preprocessing line into our buffer : */
	if( new_line )
	{
        if( ratio > 1)
		{
            SCANLINE_FUNC_FILTERED(divide_component,*(new_line),*(imout->available),(CARD8)ratio,imout->available->width);
			imout->available->flags = new_line->flags ;
			imout->available->back_color = new_line->back_color ;
			imout->encode_image_scanline( imout, imout->available );
		}else
			imout->encode_image_scanline( imout, new_line );
	}
}
/* ********************************************************************************/
/* Vector -> ASImage functions :                                                  */
/* ********************************************************************************/
Bool
set_asimage_vector( ASImage *im, register double *vector )
{
	if( vector == NULL || im == NULL )
		return False;

	if( im->alt.vector == NULL )
		im->alt.vector = safemalloc( im->width*im->height*sizeof(double));

	{
		register double *dst = im->alt.vector ;
		register int i = im->width*im->height;
		while( --i >= 0 )
			dst[i] = vector[i] ;
	}

	return True;
}

/* ********************************************************************************/
/* Convinience function - very fast image cloning :                               */
/* ********************************************************************************/
ASImage*
clone_asimage( ASImage *src, ASFlagType filter )
{
	ASImage *dst = NULL ;
	START_TIME(started);

	if( !AS_ASSERT(src) )
	{
		int chan ;
		dst = create_asimage(src->width, src->height, (src->max_compressed_width*100)/src->width);
		if( get_flags( src->flags, ASIM_DATA_NOT_USEFUL ) )
			set_flags( dst->flags, ASIM_DATA_NOT_USEFUL );
		dst->back_color = src->back_color ;
		for( chan = 0 ; chan < IC_NUM_CHANNELS;  chan++ )
			if( get_flags( filter, 0x01<<chan) )
			{
				register int i = dst->height;
				register CARD8 **dst_rows = dst->channels[chan] ;
				register CARD8 **src_rows = src->channels[chan] ;
				while( --i >= 0 )
					dst_rows[i] = asimage_copy_line( src_rows[i], dst->width );
			}
	}
	SHOW_TIME("", started);
	return dst;
}

/* ********************************************************************************/
/* Convinience function
 * 		- generate rectangles list for channel values exceeding threshold:        */
/* ********************************************************************************/
XRectangle*
get_asimage_channel_rects( ASImage *src, int channel, unsigned int threshold, unsigned int *rects_count_ret )
{
	XRectangle *rects = NULL ;
	int rects_count = 0, rects_allocated = 0 ;

	START_TIME(started);

	if( !AS_ASSERT(src) && channel < IC_NUM_CHANNELS )
	{
		int i = src->height;
		CARD8 **src_rows = src->channels[channel] ;
		unsigned int *height = safemalloc( (src->width+1)*2 * sizeof(unsigned int) );
		unsigned int *prev_runs = NULL ;
		int prev_runs_count = 0 ;
		unsigned int *runs = safemalloc( (src->width+1)*2 * sizeof(unsigned int) );
		unsigned int *tmp_runs = safemalloc( (src->width+1)*2 * sizeof(unsigned int) );
		unsigned int *tmp_height = safemalloc( (src->width+1)*2 * sizeof(unsigned int) );
		Bool count_empty = (ARGB32_CHAN8(src->back_color,channel)>= threshold);

#ifdef DEBUG_RECTS
		fprintf( stderr, "%d:back_color = #%8.8lX,  count_empty = %d, thershold = %d\n", __LINE__, src->back_color, count_empty, threshold );
#endif
		while( --i >= -1 )
		{
			int runs_count = 0 ;
#ifdef DEBUG_RECTS
			fprintf( stderr, "%d: LINE %d **********************\n", __LINE__, i );
#ifdef DEBUG_RECTS2
			asimage_print_line (src, channel, i, 0xFFFFFFFF);
#else
			asimage_print_line (src, channel, i, VRB_LINE_CONTENT);
#endif
#endif
			if( i >= 0 )
			{
				if( src_rows[i] )
				{
					runs_count = asimage_threshold_line( src_rows[i], src->width, runs, threshold );
				}else if( count_empty )
				{
					runs_count = 2 ;
					runs[0] = 0 ;
					runs[1] = src->width ;
				}
			}

			if( runs_count > 0 && (runs_count &0x0001) != 0 )
			{                                  /* allways wants to have even number of runs */
				runs[runs_count] = 0 ;
				++runs_count ;
			}

			if( prev_runs_count > 0 )
			{ /* here we need to merge runs and add all the detached rectangles to the rects list */
				int k = 0, l = 0, last_k = 0 ;
				int tmp_count = 0 ;
				unsigned int *tmp ;
				if( runs_count == 0 )
				{
					runs[0] = src->width ;
					runs[1] = src->width ;
					runs_count = 2 ;
				}
				tmp_runs[0] = 0 ;
				tmp_runs[1] = src->width ;
				/* two passes : in first pass we go through old runs and try and see if they are continued
				 * in this line. If not - we add them to the list of rectangles. At
				 * the same time we subtract them from new line's runs : */
				for( l = 0 ; l < prev_runs_count ; ++l, ++l )
				{
					int start = prev_runs[l], end = prev_runs[l+1] ;
					int matching_runs = 0 ;
#ifdef DEBUG_RECTS
					fprintf( stderr, "%d: prev run %d : start = %d, end = %d, last_k = %d, height = %d\n", __LINE__, l, start, end, last_k, height[l] );
#endif
					for( k = last_k ; k < runs_count ; ++k, ++k )
					{
#ifdef DEBUG_RECTS
						fprintf( stderr, "*%d: new run %d : start = %d, end = %d\n", __LINE__, k, runs[k], runs[k+1] );
#endif
						if( (int)runs[k] > end )
						{	/* add entire run to rectangles list */
							if( rects_count >= rects_allocated )
							{
								rects_allocated = rects_count + 8 + (rects_count>>3);
								rects = realloc( rects, rects_allocated*sizeof(XRectangle));
							}
							rects[rects_count].x = start ;
							rects[rects_count].y = i+1 ;
							rects[rects_count].width = (end-start)+1 ;
							rects[rects_count].height = height[l] ;
#ifdef DEBUG_RECTS
							fprintf( stderr, "*%d: added rectangle at y = %d\n", __LINE__, rects[rects_count].y );
#endif
							++rects_count ;
							++matching_runs;
							break;
						}else if( (int)runs[k+1] >= start  )
						{
							if( start < (int)runs[k] )
							{	/* add rectangle start, , runs[k]-start, height[l] */
								if( rects_count >= rects_allocated )
								{
									rects_allocated = rects_count + 8 + (rects_count>>3);
									rects = realloc( rects, rects_allocated*sizeof(XRectangle));
								}
								rects[rects_count].x = start ;
								rects[rects_count].y = i+1 ;
								rects[rects_count].width = runs[k]-start ;
								rects[rects_count].height = height[l] ;
#ifdef DEBUG_RECTS
								fprintf( stderr, "*%d: added rectangle at y = %d\n", __LINE__, rects[rects_count].y );
#endif
								++rects_count ;
								start = runs[k] ;
							}else if( start > (int)runs[k] )
							{
								tmp_runs[tmp_count] = runs[k] ;
								tmp_runs[tmp_count+1] = start-1 ;
								tmp_height[tmp_count] = 1 ;
#ifdef DEBUG_RECTS
								fprintf( stderr, "*%d: tmp_run %d added : %d ... %d, height = %d\n", __LINE__, tmp_count, runs[k], start-1, 1 );
#endif
								++tmp_count ; ++tmp_count ;
								runs[k] = start ;
							}
							/* at that point both runs start at the same point */
							if( end < (int)runs[k+1] )
							{
								runs[k] = end+1 ;
							}else 
							{   
								if( end > (int)runs[k+1] )
								{	
									/* add rectangle runs[k+1]+1, , end - runs[k+1], height[l] */
									if( rects_count >= rects_allocated )
									{
										rects_allocated = rects_count + 8 + (rects_count>>3);
										rects = realloc( rects, rects_allocated*sizeof(XRectangle));
									}
									rects[rects_count].x = runs[k+1]+1 ;
									rects[rects_count].y = i+1 ;
									rects[rects_count].width = end - runs[k+1] ;
									rects[rects_count].height = height[l] ;
#ifdef DEBUG_RECTS
									fprintf( stderr, "*%d: added rectangle at y = %d\n", __LINE__, rects[rects_count].y );
#endif
									++rects_count ;
									end = runs[k+1] ;
								
								} 
								/* eliminating new run - it was all used up :) */
								runs[k] = src->width ;
								runs[k+1] = src->width ;
#ifdef DEBUG_RECTS
								fprintf( stderr, "*%d: eliminating new run %d\n", __LINE__, k );
#endif
								++k ; ++k ;
							}
							tmp_runs[tmp_count] = start ;
							tmp_runs[tmp_count+1] = end ;
							tmp_height[tmp_count] = height[l]+1 ;
#ifdef DEBUG_RECTS
							fprintf( stderr, "*%d: tmp_run %d added : %d ... %d, height = %d\n", __LINE__, tmp_count, start, end, height[l]+1 );
#endif
							++tmp_count ; ++tmp_count ;
							last_k = k ;
							++matching_runs;
							break;
						}
					}
					if( matching_runs == 0 ) 
					{  /* no new runs for this prev run - add rectangle */
#ifdef DEBUG_RECTS
						fprintf( stderr, "%d: NO MATCHING NEW RUNS : start = %d, end = %d, height = %d\n", __LINE__, start, end, height[l] );
#endif
						if( rects_count >= rects_allocated )
						{
							rects_allocated = rects_count + 8 + (rects_count>>3);
							rects = realloc( rects, rects_allocated*sizeof(XRectangle));
						}
						rects[rects_count].x = start ;
						rects[rects_count].y = i+1 ;
						rects[rects_count].width = (end-start)+1 ;
						rects[rects_count].height = height[l] ;
#ifdef DEBUG_RECTS
						fprintf( stderr, "*%d: added rectangle at y = %d\n", __LINE__, rects[rects_count].y );
#endif
						++rects_count ;
					}	 
				}
				/* second pass: we need to pick up remaining new runs */
				/* I think these should be inserted in oredrly manner so that we have runs list arranged in ascending order */
				for( k = 0 ; k < runs_count ; ++k, ++k )
					if( runs[k] < src->width )
					{
						int ii = tmp_count ; 
						while( ii > 0 && tmp_runs[ii-1] > runs[k] )
						{
							tmp_runs[ii] = tmp_runs[ii-2] ;
							tmp_runs[ii+1] = tmp_runs[ii-1] ;
							tmp_height[ii] = tmp_height[ii-2] ;
							--ii ; --ii ;
						}
						tmp_runs[ii] = runs[k] ;
						tmp_runs[ii+1] = runs[k+1] ;
						tmp_height[ii] = 1 ;
#ifdef DEBUG_RECTS
						fprintf( stderr, "*%d: tmp_run %d added : %d ... %d, height = %d\n", __LINE__, ii, runs[k], runs[k+1], 1 );
#endif
						++tmp_count, ++tmp_count;
					}
				tmp = prev_runs ;
				prev_runs = tmp_runs ;
				tmp_runs = tmp ;
				tmp = height ;
				height = tmp_height ;
				tmp_height = tmp ;
				prev_runs_count = tmp_count ;
			}else if( runs_count > 0 )
			{
				int k = runs_count;
				prev_runs_count = runs_count ;
				prev_runs = runs ;
				runs = safemalloc( (src->width+1)*2 * sizeof(unsigned int) );
				while( --k >= 0 )
					height[k] = 1 ;
			}
		}
		free( runs );
		if( prev_runs )
			free( prev_runs );
		free( tmp_runs );
		free( tmp_height );
		free( height );
	}
	SHOW_TIME("", started);

	if( rects_count_ret )
		*rects_count_ret = rects_count ;

	return rects;
}

/***********************************************************************************/
void
raw2scanline( register CARD8 *row, ASScanline *buf, CARD8 *gamma_table, unsigned int width, Bool grayscale, Bool do_alpha )
{
	register int x = width;

	if( grayscale )
		row += do_alpha? width<<1 : width ;
	else
		row += width*(do_alpha?4:3) ;

	if( gamma_table )
	{
		if( !grayscale )
		{
			while ( --x >= 0 )
			{
				row -= 3 ;
				if( do_alpha )
				{
					--row;
					buf->alpha[x] = row[3];
				}
				buf->xc1[x]  = gamma_table[row[0]];
				buf->xc2[x]= gamma_table[row[1]];
				buf->xc3[x] = gamma_table[row[2]];
			}
		}else /* greyscale */
			while ( --x >= 0 )
			{
				if( do_alpha )
					buf->alpha[x] = *(--row);
				buf->xc1 [x] = buf->xc2[x] = buf->xc3[x]  = gamma_table[*(--row)];
			}
	}else
	{
		if( !grayscale )
		{
			while ( --x >= 0 )
			{
				row -= 3 ;
				if( do_alpha )
				{
					--row;
					buf->alpha[x] = row[3];
				}
				buf->xc1[x]  = row[0];
				buf->xc2[x]= row[1];
				buf->xc3[x] = row[2];
			}
		}else /* greyscale */
			while ( --x >= 0 )
			{
				if( do_alpha )
					buf->alpha[x] = *(--row);
				buf->xc1 [x] = buf->xc2[x] = buf->xc3[x]  = *(--row);
			}
	}
}

/* ********************************************************************************/
/* The end !!!! 																 */
/* ********************************************************************************/

