/*
 * Copyright (c) 2001 Sasha Vasko <sashav@sprintmail.com>
 *
 * This module is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "../configure.h"

#define LOCAL_DEBUG
/*#define DO_CLOCKING*/

#include <unistd.h>
#ifdef HAVE_FREETYPE
#include <freetype/freetype.h>
#include FT_FREETYPE_H
#endif

#define INCLUDE_ASFONT_PRIVATE

#include "../include/aftersteplib.h"
#include <X11/Intrinsic.h>

#include "../include/afterstep.h"
#include "../include/screen.h"
#include "../include/ashash.h"
#include "../include/asimage.h"
#include "../include/asfont.h"

/*********************************************************************************/
/* TrueType and X11 font management functions :   								 */
/*********************************************************************************/

/*********************************************************************************/
/* construction destruction miscelanea:			   								 */
/*********************************************************************************/

void asfont_destroy (ASHashableValue value, void *data);

ASFontManager *
create_font_manager( const char * font_path, ASFontManager *reusable_memory )
{
	ASFontManager *fontman = reusable_memory;
	if( fontman == NULL )
		fontman = safecalloc( 1, sizeof(ASFontManager));
	else
		memset( fontman, 0x00, sizeof(ASFontManager));

	if( font_path )
		fontman->font_path = mystrdup( font_path );

#ifdef HAVE_FREETYPE
	if( !FT_Init_FreeType( &(fontman->ft_library)) )
		fontman->ft_ok = True ;
	else
		show_error( "Failed to initialize FreeType library - TrueType Fonts support will be disabled!");
LOCAL_DEBUG_OUT( "Freetype library is %p", fontman->ft_library );
#endif

	fontman->fonts_hash = create_ashash( 7, string_hash_value, string_compare, asfont_destroy );

	return fontman;
}

void
destroy_font_manager( ASFontManager *fontman, Bool reusable )
{
	if( fontman )
	{

		destroy_ashash( &(fontman->fonts_hash) );

#ifdef HAVE_FREETYPE
		FT_Done_FreeType( fontman->ft_library);
		fontman->ft_ok = False ;
#endif
		if( fontman->font_path )
			free( fontman->font_path );

		if( !reusable )
			free( fontman );
		else
			memset( fontman, 0x00, sizeof(ASFontManager));
	}
}

static int load_freetype_glyphs( ASFont *font );

ASFont*
open_freetype_font( ASFontManager *fontman, const char *font_string, int face_no, int size, Bool verbose)
{
	ASFont *font = NULL ;
#ifdef HAVE_FREETYPE
	if( fontman && fontman->ft_ok )
	{
		char *realfilename;
		FT_Face face ;
LOCAL_DEBUG_OUT( "looking for \"%s\"", font_string );
		if( (realfilename = findIconFile( font_string, fontman->font_path, R_OK )) == NULL )
		{/* we might have face index specifier at the end of the filename */
			char *tmp = mystrdup( font_string );
			register int i = 0;
			while(tmp[i] != '\0' ) ++i ;
			while( --i >= 0 )
				if( !isdigit( tmp[i] ) )
				{
					if( tmp[i] == '.' )
					{
						face_no = atoi( &tmp[i+1] );
						tmp[i] = '\0' ;
					}
					break;
				}
			if( i >= 0 && font_string[i] != '\0' )
				realfilename = findIconFile( tmp, fontman->font_path, R_OK );
			free( tmp );
		}

		if( realfilename )
		{
			face = NULL ;
LOCAL_DEBUG_OUT( "font file found : \"%s\", trying to load face #%d, using library %p", realfilename, face_no, fontman->ft_library );
			if( FT_New_Face( fontman->ft_library, "test.ttf"/*realfilename*/, face_no, &face ) )
			{
LOCAL_DEBUG_OUT( "face load failed.%s", "" );

				if( face_no  > 0  )
				{
					show_warning( "face %d is not available in font \"%s\" - falling back to first available.", face_no, realfilename );
					FT_New_Face( fontman->ft_library, realfilename, 0, &face );
				}
			}
LOCAL_DEBUG_OUT( "face found : %p", face );
			if( face != NULL )
			{
				if( face->num_glyphs >  MAX_GLYPHS_PER_FONT )
					show_error( "Font \"%s\" contains too many glyphs - %d. Max allowed is %d", realfilename, face->num_glyphs, MAX_GLYPHS_PER_FONT );
				else
				{
					font = safecalloc( 1, sizeof(ASFont));
					font->magic = MAGIC_ASFONT ;
					font->fontman = fontman;
					font->type = ASF_Freetype ;
					font->ft_face = face ;
					FT_Set_Pixel_Sizes( font->ft_face, size, size );
	   				font->glyphs = safecalloc( face->num_glyphs, sizeof(ASGlyph));
					font->glyphs_num = face->num_glyphs ;
	   				font->max_height = load_freetype_glyphs( font );
				}
			}else if( verbose )
				show_error( "FreeType library failed to load font \"%s\"", realfilename );

			if( realfilename != font_string )
				free( realfilename );
		}
	}
#endif
	return font;
}

ASFont*
open_X11_font( ASFontManager *fontman, const char *font_string)
{
	ASFont *font = NULL ;
	/* TODO: implement X11 font opening */
	return font;
}

ASFont*
get_asfont( ASFontManager *fontman, const char *font_string, int face_no, int size, ASFontType type )
{
	ASFont *font = NULL ;
	if( fontman && font_string )
	{
		if( get_hash_item( fontman->fonts_hash, (ASHashableValue)((char*)font_string), (void**)&font) != ASH_Success )
		{	/* not loaded just yet - lets do it :*/
			if( type == ASF_Freetype || type == ASF_GuessWho )
				font = open_freetype_font( fontman, font_string, face_no, size, (type == ASF_Freetype));
			if( font == NULL )
				font = open_X11_font( fontman, font_string );
			if( font != NULL )
				add_hash_item( fontman->fonts_hash, (ASHashableValue)(char*)font_string, font);
		}
	}
	return font;
}

void
destroy_font( ASFont *font )
{
	if( font )
	{
#ifdef HAVE_FREETYPE
		if( font->type == ASF_Freetype && font->ft_face )
			FT_Done_Face(font->ft_face);
#endif
		font->magic = 0 ;
		free( font );
	}
}

void
asfont_destroy (ASHashableValue value, void *data)
{
	if( data )
	{
		free( value.string_val );
		if( ((ASMagic*)data)->magic == MAGIC_ASFONT )
			destroy_font( (ASFont*)data );
	}
}

#ifdef HAVE_FREETYPE
static int
load_freetype_glyphs( ASFont *font )
{
	int max_ascend = 0, max_descend = 0;
	int i ;
	register FT_Face face = font->ft_face;
	for( i = 0 ; i < font->glyphs_num ; ++i )
	{
		if( !FT_Load_Glyph( face, i, FT_LOAD_DEFAULT ) )
			if( !FT_Render_Glyph( face->glyph, ft_render_mode_normal ) )
			{
				FT_Bitmap 	*bmap = &(face->glyph->bitmap) ;
				ASGlyph 	*asg = &(font->glyphs[i]);
				int k, src_inc = bmap->pitch, dst_inc = bmap->width;
				register CARD8 *dst, *src ;
				dst = asg->pixmap = safemalloc( bmap->rows*bmap->width );
				src = bmap->buffer ;
				if( bmap->pitch < 0 )
				{
				   src_inc = -bmap->pitch ;
				   dst_inc = -bmap->width ;
				   dst += bmap->width*(bmap->rows-1) ;
				}
				for( k = 0 ; k < bmap->rows ; ++k )
				{
					memcpy( dst, src, bmap->width );
					dst+= dst_inc ;
					src+= src_inc ;
				}
				asg->width   = bmap->width ;
				asg->height  = bmap->rows ;
				asg->descend = bmap->rows-face->glyph->bitmap_top ;
				asg->lead    = (face->glyph->bitmap_left<0)? 0 : face->glyph->bitmap_left ;
LOCAL_DEBUG_OUT( "glyph %d is %dx%d descend = %d, lead = %d, bmap_top = %d", i, asg->width, asg->height, asg->descend, asg->lead, face->glyph->bitmap_top );
				if( face->glyph->bitmap_top > max_ascend )
					max_ascend = face->glyph->bitmap_top;
				if( asg->descend > max_descend )
					max_descend = asg->descend;
			}
	}
	return max_ascend+max_descend;
}
#endif

/* Misc functions : */
void print_asfont( FILE* stream, ASFont* font)
{
	if( font )
	{
		fprintf( stream, "font.type = %d\n", font->type       );
#ifdef HAVE_FREETYPE
		fprintf( stream, "font.ft_face = %p\n", font->ft_face    );              /* free type font handle */
#endif
		fprintf( stream, "font.glyphs_num = %d\n", font->glyphs_num );
		fprintf( stream, "font.max_height = %d\n", font->max_height );
	}
}

void print_asglyph( FILE* stream, ASFont* font, unsigned int glyph_index)
{
	if( font && glyph_index < font->glyphs_num)
	{
		int i, k ;
		ASGlyph *asg = &(font->glyphs[glyph_index]);
		fprintf( stream, "glyph[%d].width = %d\n", glyph_index, asg->width  );
		fprintf( stream, "glyph[%d].height = %d\n", glyph_index, asg->height  );
		for( i = 0 ; i < asg->height ; i++ )
		{
			for( k = 0 ; k < asg->width ; k++ )
				fprintf( stream, "%2.2X ", asg->pixmap[i*asg->width+k]);
			fprintf( stream, "\n" );
		}
	}
}

/*********************************************************************************/
/* actuall rendering code :						   								 */
/*********************************************************************************/
static inline void
get_character_size( char c, ASFont *font, unsigned int *width, unsigned int *height )
{

	if( font->type == ASF_Freetype )
	{
#ifdef HAVE_FREETYPE


#endif
	}
}

Bool
get_text_size( const char *text, ASFont *font, unsigned int *width, unsigned int *height )
{
	unsigned int w = 0, h = 0;
	unsigned int line_height = 0, line_width = 0;
	int i = -1;
	if( text == NULL || font == NULL)
		return False;
	do
	{
		++i ;
		if( text[i] == '\n' || text[i] == '\0' )
		{
			h += (line_height == 0)? font->max_height : line_height;
			line_height = 0 ;
			if( line_width > w )
				w += line_width ;
			line_width = 0 ;
		}else
		{
			int char_width, char_height;
			get_character_size( text[i], font, &char_width, &char_height );
			if( char_height > line_height )
				line_height = char_height ;
			line_width += char_width ;
		}
	}while( text[i] != '\0' );

	if( width )
		*width = w ;
	if( height )
		*height = h ;
	return True;
}



ASImage *
draw_text( const char *text, ASFont *font, ASImage *fore, ARGB32 tint, int flip )
{

	return NULL;
}

/*********************************************************************************/
/* The end !!!! 																 */
/*********************************************************************************/

