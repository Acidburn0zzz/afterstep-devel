/*
 * Copyright (c) 2000,2001 Sasha Vasko <sasha at aftercode.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
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

#include "config.h"

/* #define LOCAL_DEBUG */
/* #define DO_CLOCKING */

#include <malloc.h>
#ifdef DO_CLOCKING
#include <sys/time.h>
#endif
#include <stdarg.h>


#include "afterbase.h"
#include "asvisual.h"
#include "blender.h"
#include "asimage.h"
#include "ximage.h"


/* ***************************************************************************/
/* ASImage->XImage->pixmap->XImage->ASImage conversion						*/
/* ***************************************************************************/

ASImage      *
ximage2asimage (ASVisual *asv, XImage * xim, unsigned int compression)
{
	ASImage      *im = NULL;
	unsigned char *xim_line;
	int           i, height, bpl;
	ASScanline    xim_buf;
#ifdef LOCAL_DEBUG
	CARD32       *tmp ;
#endif

	if( xim == NULL)
		return im;

	height = xim->height;
	bpl 	  = xim->bytes_per_line;
	xim_line = xim->data;

	im = create_asimage( xim->width, xim->height, compression);
#ifdef LOCAL_DEBUG
	tmp = safemalloc( xim->width * sizeof(CARD32));
#endif
	prepare_scanline( xim->width, 0, &xim_buf, asv->BGR_mode );
	for (i = 0; i < height; i++)
	{
		GET_SCANLINE(asv,xim,&xim_buf,i,xim_line);
		asimage_add_line (im, IC_RED,   xim_buf.red, i);
		asimage_add_line (im, IC_GREEN, xim_buf.green, i);
		asimage_add_line (im, IC_BLUE,  xim_buf.blue, i);
#ifdef LOCAL_DEBUG
		if( !asimage_compare_line( im, IC_RED,  xim_buf.red, tmp, i, True ) )
			exit(0);
		if( !asimage_compare_line( im, IC_GREEN,  xim_buf.green, tmp, i, True ) )
			exit(0);
		if( !asimage_compare_line( im, IC_BLUE,  xim_buf.blue, tmp, i, True ) )
			exit(0);
#endif
		xim_line += bpl;
	}
	free_scanline(&xim_buf, True);

	return im;
}

static inline int
xim_set_component( register CARD32 *src, register CARD32 value, int offset, int len )
{
	register int i ;
	for( i = offset ; i < len ; ++i )
		src[i] = value;
	return len-offset;
}


XImage*
asimage2ximage (ASVisual *asv, ASImage *im)
{
	XImage        *xim = NULL;
	int            i;
	ASScanline     xim_buf;
	ASImageOutput *imout ;
#ifdef DO_CLOCKING
	clock_t       started = clock ();
#endif

	if (im == NULL)
		return xim;
	if( (imout = start_image_output( asv, im, ASA_XImage, 0, ASIMAGE_QUALITY_DEFAULT )) == NULL )
		return xim;
	xim = im->alt.ximage ;
	prepare_scanline( im->width, 0, &xim_buf, asv->BGR_mode );
#ifdef DO_CLOCKING
	started = clock ();
#endif
	set_flags( xim_buf.flags, SCL_DO_ALL );
	for (i = 0; i < im->height; i++)
	{
		int count ;
		if( (count = asimage_decode_line (im, IC_RED,   xim_buf.red, i, 0, xim_buf.width)) < xim_buf.width )
			xim_set_component( xim_buf.red, ARGB32_RED8(im->back_color), count, xim_buf.width );
		if( (count = asimage_decode_line (im, IC_GREEN, xim_buf.green, i, 0, xim_buf.width))> 0 )
			xim_set_component( xim_buf.green, ARGB32_GREEN8(im->back_color), count, xim_buf.width );
		if( (count = asimage_decode_line (im, IC_BLUE,  xim_buf.blue, i, 0, xim_buf.width)) > 0 )
			xim_set_component( xim_buf.blue, ARGB32_BLUE8(im->back_color), count, xim_buf.width );

		imout->output_image_scanline( imout, &xim_buf, 1 );
	}
#ifdef DO_CLOCKING
	fprintf (stderr, "asimage->ximage time (clocks): %lu\n", clock () - started);
#endif
	free_scanline(&xim_buf, True);

	stop_image_output(&imout);

	return xim;
}

XImage*
asimage2mask_ximage (ASVisual *asv, ASImage *im)
{
	XImage        *xim = NULL;
	int            i;
	ASScanline     xim_buf;
	ASImageOutput *imout ;

	if (im == NULL)
		return xim;

	if( (imout = start_image_output( asv, im, ASA_MaskXImage, 0, ASIMAGE_QUALITY_POOR )) == NULL )
		return xim;
	xim = im->alt.mask_ximage ;
	prepare_scanline( xim->width, 0, &xim_buf, asv->BGR_mode );
	xim_buf.flags = SCL_DO_ALPHA ;
	for (i = 0; i < im->height; i++)
	{
		int count = asimage_decode_line (im, IC_ALPHA, xim_buf.alpha, i, 0, xim_buf.width);
		if( count < xim_buf.width )
			xim_set_component( xim_buf.blue, ARGB32_ALPHA8(im->back_color), count, xim_buf.width );
		imout->output_image_scanline( imout, &xim_buf, 127 );
	}
	free_scanline(&xim_buf, True);

	stop_image_output(&imout);

	return xim;
}

ASImage      *
pixmap2asimage(ASVisual *asv, Pixmap p, int x, int y, unsigned int width, unsigned int height, unsigned long plane_mask, Bool keep_cache, unsigned int compression)
{
#ifndef X_DISPLAY_MISSING
	XImage       *xim = XGetImage (asv->dpy, p, x, y, width, height, plane_mask, ZPixmap);
	ASImage      *im = NULL;

	if (xim)
	{
		im = ximage2asimage (asv, xim, compression);
		if( keep_cache )
			im->alt.ximage = xim ;
		else
			XDestroyImage (xim);
	}
	return im;
#else
    return NULL ;
#endif
}

Pixmap
asimage2pixmap(ASVisual *asv, Window root, ASImage *im, GC gc, Bool use_cached)
{
#ifndef X_DISPLAY_MISSING
	XImage       *xim ;
	Pixmap        p = None;
	GC 			  my_gc = gc ;

	if ( !use_cached || im->alt.ximage == NULL )
	{
		if( (xim = asimage2ximage( asv, im )) == NULL )
		{
			show_error("cannot export image into XImage.");
			return None ;
		}
	}else
		xim = im->alt.ximage ;
	if (xim != NULL )
	{
		p = create_visual_pixmap( asv, root, xim->width, xim->height, 0 );
		if( my_gc == NULL )
		{
			XGCValues gcv ;
			my_gc = XCreateGC( asv->dpy, p, 0, &gcv );
		}
		XPutImage( asv->dpy, p, my_gc, xim, 0, 0, 0, 0, xim->width, xim->height );
		if( my_gc != gc )
			XFreeGC( asv->dpy, my_gc );
		if( xim != im->alt.ximage )
			XDestroyImage (xim);
	}
	return p;
#else
	return None ;
#endif
}

Pixmap
asimage2mask(ASVisual *asv, Window root, ASImage *im, GC gc, Bool use_cached)
{
#ifndef X_DISPLAY_MISSING
	XImage       *xim ;
	Pixmap        mask = None;
	GC 			  my_gc = gc ;

	if ( !use_cached || im->alt.mask_ximage == NULL )
	{
		if( (xim = asimage2mask_ximage( asv, im )) == NULL )
		{
			show_error("cannot export image's mask into XImage.");
			return None ;
		}
	}else
		xim = im->alt.mask_ximage ;
	mask = create_visual_pixmap( asv, root, xim->width, xim->height, 1 );
	if( my_gc == NULL )
	{
		XGCValues gcv ;
		my_gc = XCreateGC( asv->dpy, mask, 0, &gcv );
	}
	XPutImage( asv->dpy, mask, my_gc, xim, 0, 0, 0, 0, xim->width, xim->height );
	if( my_gc != gc )
		XFreeGC( asv->dpy, my_gc );
	if( xim != im->alt.mask_ximage )
		XDestroyImage (xim);
	return mask;
#else
	return None ;
#endif
}


/* ********************************************************************************/
/* The end !!!! 																 */
/* ********************************************************************************/

