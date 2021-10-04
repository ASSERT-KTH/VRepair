#ifndef NGIFLIB_NO_FILE
#include <stdio.h>
#endif /* NGIFLIB_NO_FILE */

#include "ngiflib.h"

/* decodeur GIF en C portable (pas de pb big/little endian)
 * Thomas BERNARD. janvier 2004.
 * (c) 2004-2017 Thomas Bernard. All rights reserved
 */

/* Fonction de debug */
#ifdef DEBUG
void fprintf_ngiflib_img(FILE * f, struct ngiflib_img * i) {
	fprintf(f, "  * ngiflib_img @ %p\n", i);
	fprintf(f, "    next = %p\n", i->next);
	fprintf(f, "    parent = %p\n", i->parent);
	fprintf(f, "    palette = %p\n", i->palette);
	fprintf(f, "    %3d couleurs", i->ncolors);
	if(i->interlaced) fprintf(f, " interlaced");
	fprintf(f, "\n    taille : %dx%d, pos (%d,%d)\n", i->width, i->height, i->posX, i->posY);
	fprintf(f, "    sort_flag=%x localpalbits=%d\n", i->sort_flag, i->localpalbits);
}
#endif /* DEBUG */

void GifImgDestroy(struct ngiflib_img * i) {
	if(i==NULL) return;
	if(i->next) GifImgDestroy(i->next);
	if(i->palette && (i->palette != i->parent->palette))
	  ngiflib_free(i->palette);
	ngiflib_free(i);
}

/* Fonction de debug */
#ifdef DEBUG
void fprintf_ngiflib_gif(FILE * f, struct ngiflib_gif * g) {
	struct ngiflib_img * i;
	fprintf(f, "* ngiflib_gif @ %p %s\n", g, g->signature);
	fprintf(f, "  %dx%d, %d bits, %d couleurs\n", g->width, g->height, g->imgbits, g->ncolors);
	fprintf(f, "  palette = %p, backgroundcolorindex %d\n", g->palette, g->backgroundindex);
	fprintf(f, "  pixelaspectratio = %d\n", g->pixaspectratio);
	fprintf(f, "  frbuff = %p\n", g->frbuff.p8);
	
	fprintf(f, "  cur_img = %p\n", g->cur_img);
	fprintf(f, "  %d images :\n", g->nimg);
	i = g->first_img;
	while(i) {
		fprintf_ngiflib_img(f, i);
		i = i->next;
	}
}
#endif /* DEBUG */

void GifDestroy(struct ngiflib_gif * g) {
	if(g==NULL) return;
	if(g->palette) ngiflib_free(g->palette);
	if(g->frbuff.p8) ngiflib_free(g->frbuff.p8);
	GifImgDestroy(g->first_img);
	ngiflib_free(g);
}

/* u8 GetByte(struct ngiflib_gif * g);
 * fonction qui renvoie un octet du fichier .gif
 * on pourait optimiser en faisant 2 fonctions.
 */
static u8 GetByte(struct ngiflib_gif * g) {
#ifndef NGIFLIB_NO_FILE
	if(g->mode & NGIFLIB_MODE_FROM_MEM) {
#endif /* NGIFLIB_NO_FILE */
		return *(g->input.bytes++);
#ifndef NGIFLIB_NO_FILE
	} else {
		return (u8)(getc(g->input.file));
	}
#endif /* NGIFLIB_NO_FILE */
}

/* u16 GetWord()
 * Renvoie un mot de 16bits
 * N'est pas influencee par l'endianess du CPU !
 */
static u16 GetWord(struct ngiflib_gif * g) {
	u16 r = (u16)GetByte(g);
	r |= ((u16)GetByte(g) << 8);
	return r;
}

/* int GetByteStr(struct ngiflib_gif * g, u8 * p, int n);
 * prend en argument un pointeur sur la destination
 * et le nombre d'octet a lire.
 * Renvoie 0 si l'operation a reussi, -1 sinon.
 */
static int GetByteStr(struct ngiflib_gif * g, u8 * p, int n) {
	if(!p) return -1;
#ifndef NGIFLIB_NO_FILE
	if(g->mode & NGIFLIB_MODE_FROM_MEM) {
#endif /* NGIFLIB_NO_FILE */
		ngiflib_memcpy(p, g->input.bytes, n);
		g->input.bytes += n;
		return 0;
#ifndef NGIFLIB_NO_FILE
	} else {
		size_t read;
		read = fread(p, 1, n, g->input.file);
		return ((int)read == n) ? 0 : -1;
	}
#endif /* NGIFLIB_NO_FILE */
}

/* void WritePixel(struct ngiflib_img * i, u8 v);
 * ecrit le pixel de valeur v dans le frame buffer
 */
static void WritePixel(struct ngiflib_img * i, struct ngiflib_decode_context * context, u8 v) {
	struct ngiflib_gif * p = i->parent;

	if(v!=i->gce.transparent_color || !i->gce.transparent_flag) {
#ifndef NGIFLIB_INDEXED_ONLY
		if(p->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
			*context->frbuff_p.p8 = v;
#ifndef NGIFLIB_INDEXED_ONLY
		} else
			*context->frbuff_p.p32 =
			   GifIndexToTrueColor(i->palette, v);
#endif /* NGIFLIB_INDEXED_ONLY */
	}
	if(--(context->Xtogo) <= 0) {
		#ifdef NGIFLIB_ENABLE_CALLBACKS
		if(p->line_cb) p->line_cb(p, context->line_p, context->curY);
		#endif /* NGIFLIB_ENABLE_CALLBACKS */
		context->Xtogo = i->width;
		switch(context->pass) {
		case 0:
			context->curY++;
			break;
		case 1:	/* 1st pass : every eighth row starting from 0 */
			context->curY += 8;
			if(context->curY >= p->height) {
				context->pass++;
				context->curY = i->posY + 4;
			}
			break;
		case 2:	/* 2nd pass : every eighth row starting from 4 */
			context->curY += 8;
			if(context->curY >= p->height) {
				context->pass++;
				context->curY = i->posY + 2;
			}
			break;
		case 3:	/* 3rd pass : every fourth row starting from 2 */
			context->curY += 4;
			if(context->curY >= p->height) {
				context->pass++;
				context->curY = i->posY + 1;
			}
			break;
		case 4:	/* 4th pass : every odd row */
			context->curY += 2;
			break;
		}
#ifndef NGIFLIB_INDEXED_ONLY
		if(p->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
			#ifdef NGIFLIB_ENABLE_CALLBACKS
			context->line_p.p8 = p->frbuff.p8 + (u32)context->curY*p->width;
			context->frbuff_p.p8 = context->line_p.p8 + i->posX;
			#else
			context->frbuff_p.p8 = p->frbuff.p8 + (u32)context->curY*p->width + i->posX;
			#endif /* NGIFLIB_ENABLE_CALLBACKS */
#ifndef NGIFLIB_INDEXED_ONLY
		} else {
			#ifdef NGIFLIB_ENABLE_CALLBACKS
			context->line_p.p32 = p->frbuff.p32 + (u32)context->curY*p->width;
			context->frbuff_p.p32 = context->line_p.p32 + i->posX;
			#else
			context->frbuff_p.p32 = p->frbuff.p32 + (u32)context->curY*p->width + i->posX;
			#endif /* NGIFLIB_ENABLE_CALLBACKS */
		}
#endif /* NGIFLIB_INDEXED_ONLY */
	} else {
#ifndef NGIFLIB_INDEXED_ONLY
		if(p->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
			context->frbuff_p.p8++;
#ifndef NGIFLIB_INDEXED_ONLY
		} else {
			context->frbuff_p.p32++;
		}
#endif /* NGIFLIB_INDEXED_ONLY */
	}
}

/* void WritePixels(struct ngiflib_img * i, const u8 * pixels, u16 n);
 * ecrit les pixels dans le frame buffer
 */
static void WritePixels(struct ngiflib_img * i, struct ngiflib_decode_context * context, const u8 * pixels, u16 n) {
	u16 tocopy;	
	struct ngiflib_gif * p = i->parent;

	while(n > 0) {
		tocopy = (context->Xtogo < n) ? context->Xtogo : n;
		if(!i->gce.transparent_flag) {
#ifndef NGIFLIB_INDEXED_ONLY
			if(p->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
				ngiflib_memcpy(context->frbuff_p.p8, pixels, tocopy);
				pixels += tocopy;
				context->frbuff_p.p8 += tocopy;
#ifndef NGIFLIB_INDEXED_ONLY
			} else {
				int j;
				for(j = (int)tocopy; j > 0; j--) {
					*(context->frbuff_p.p32++) =
					   GifIndexToTrueColor(i->palette, *pixels++);
				}
			}
#endif /* NGIFLIB_INDEXED_ONLY */
		} else {
			int j;
#ifndef NGIFLIB_INDEXED_ONLY
			if(p->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
				for(j = (int)tocopy; j > 0; j--) {
					if(*pixels != i->gce.transparent_color) *context->frbuff_p.p8 = *pixels;
					pixels++;
					context->frbuff_p.p8++;
				}
#ifndef NGIFLIB_INDEXED_ONLY
			} else {
				for(j = (int)tocopy; j > 0; j--) {
					if(*pixels != i->gce.transparent_color) {
						*context->frbuff_p.p32 = GifIndexToTrueColor(i->palette, *pixels);
					}
					pixels++;
					context->frbuff_p.p32++;
				}
			}
#endif /* NGIFLIB_INDEXED_ONLY */
		}
		context->Xtogo -= tocopy;
		if(context->Xtogo == 0) {
			#ifdef NGIFLIB_ENABLE_CALLBACKS
			if(p->line_cb) p->line_cb(p, context->line_p, context->curY);
			#endif /* NGIFLIB_ENABLE_CALLBACKS */
			context->Xtogo = i->width;
			switch(context->pass) {
			case 0:
				context->curY++;
				break;
			case 1:	/* 1st pass : every eighth row starting from 0 */
				context->curY += 8;
				if(context->curY >= p->height) {
					context->pass++;
					context->curY = i->posY + 4;
				}
				break;
			case 2:	/* 2nd pass : every eighth row starting from 4 */
				context->curY += 8;
				if(context->curY >= p->height) {
					context->pass++;
					context->curY = i->posY + 2;
				}
				break;
			case 3:	/* 3rd pass : every fourth row starting from 2 */
				context->curY += 4;
				if(context->curY >= p->height) {
					context->pass++;
					context->curY = i->posY + 1;
				}
				break;
			case 4:	/* 4th pass : every odd row */
				context->curY += 2;
				break;
			}
#ifndef NGIFLIB_INDEXED_ONLY
			if(p->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
				#ifdef NGIFLIB_ENABLE_CALLBACKS
				context->line_p.p8 = p->frbuff.p8 + (u32)context->curY*p->width;
				context->frbuff_p.p8 = context->line_p.p8 + i->posX;
				#else
				context->frbuff_p.p8 = p->frbuff.p8 + (u32)context->curY*p->width + i->posX;
				#endif /* NGIFLIB_ENABLE_CALLBACKS */
#ifndef NGIFLIB_INDEXED_ONLY
			} else {
				#ifdef NGIFLIB_ENABLE_CALLBACKS
				context->line_p.p32 = p->frbuff.p32 + (u32)context->curY*p->width;
				context->frbuff_p.p32 = context->line_p.p32 + i->posX;
				#else
				context->frbuff_p.p32 = p->frbuff.p32 + (u32)context->curY*p->width + i->posX;
				#endif /* NGIFLIB_ENABLE_CALLBACKS */
			}
#endif /* NGIFLIB_INDEXED_ONLY */
		}
		n -= tocopy;
	}
}

/*
 * u16 GetGifWord(struct ngiflib_img * i);
 * Renvoie un code LZW (taille variable)
 */
static u16 GetGifWord(struct ngiflib_img * i, struct ngiflib_decode_context * context) {
	u16 r;
	int bits_todo;
	u16 newbyte;

	bits_todo = (int)context->nbbit - (int)context->restbits;
	if( bits_todo <= 0) {	/* nbbit <= restbits */
		r = context->lbyte;
		context->restbits -= context->nbbit;
		context->lbyte >>= context->nbbit;
	} else if( bits_todo > 8 ) {	/* nbbit > restbits + 8 */
		if(context->restbyte >= 2) {
			context->restbyte -= 2;
			r = *context->srcbyte++;
		} else {
			if(context->restbyte == 0) {
				context->restbyte = GetByte(i->parent);
#if defined(DEBUG) && !defined(NGIFLIB_NO_FILE)
				if(i->parent->log) fprintf(i->parent->log, "restbyte = %02X\n", context->restbyte);
#endif /* defined(DEBUG) && !defined(NGIFLIB_NO_FILE) */
				GetByteStr(i->parent, context->byte_buffer, context->restbyte);
				context->srcbyte = context->byte_buffer;
			}
			r = *context->srcbyte++;
			if(--context->restbyte == 0) {
				context->restbyte = GetByte(i->parent);
#if defined(DEBUG) && !defined(NGIFLIB_NO_FILE)
				if(i->parent->log) fprintf(i->parent->log, "restbyte = %02X\n", context->restbyte);
#endif /* defined(DEBUG) && !defined(NGIFLIB_NO_FILE) */
				GetByteStr(i->parent, context->byte_buffer, context->restbyte);
				context->srcbyte = context->byte_buffer;
			}
			context->restbyte--;
		}
		newbyte = *context->srcbyte++;
		r |= newbyte << 8;
		r = (r << context->restbits) | context->lbyte;
		context->restbits = 16 - bits_todo;
		context->lbyte = newbyte >> (bits_todo - 8);
	} else /*if( bits_todo > 0 )*/ { /* nbbit > restbits */
		if(context->restbyte == 0) {
			context->restbyte = GetByte(i->parent);
#if defined(DEBUG) && !defined(NGIFLIB_NO_FILE)
			if(i->parent->log) fprintf(i->parent->log, "restbyte = %02X\n", context->restbyte);
#endif /* defined(DEBUG) && !defined(NGIFLIB_NO_FILE) */
			GetByteStr(i->parent, context->byte_buffer, context->restbyte);
			context->srcbyte = context->byte_buffer;
		}
		newbyte = *context->srcbyte++;
		context->restbyte--;
		r = (newbyte << context->restbits) | context->lbyte;
		context->restbits = 8 - bits_todo;
		context->lbyte = newbyte >> bits_todo;
	}
	return (r & context->max);	/* applique le bon masque pour eliminer les bits en trop */
}

/* ------------------------------------------------ */
static void FillGifBackGround(struct ngiflib_gif * g) {
	long n = (long)g->width*g->height;
#ifndef NGIFLIB_INDEXED_ONLY
	u32 bg_truecolor;
#endif /* NGIFLIB_INDEXED_ONLY */

	if((g->frbuff.p8==NULL)||(g->palette==NULL)) return;
#ifndef NGIFLIB_INDEXED_ONLY
	if(g->mode & NGIFLIB_MODE_INDEXED) {
#endif /* NGIFLIB_INDEXED_ONLY */
		ngiflib_memset(g->frbuff.p8, g->backgroundindex, n);
#ifndef NGIFLIB_INDEXED_ONLY
	} else {
		u32 * p = g->frbuff.p32;
		bg_truecolor = GifIndexToTrueColor(g->palette, g->backgroundindex);
		while(n-->0) *p++ = bg_truecolor;
	}
#endif /* NGIFLIB_INDEXED_ONLY */
}

/* ------------------------------------------------ */
int CheckGif(u8 * b) {
	return (b[0]=='G')&&(b[1]=='I')&&(b[2]=='F')&&(b[3]=='8');
}

/* ------------------------------------------------ */
static int DecodeGifImg(struct ngiflib_img * i) {
	struct ngiflib_decode_context context;
	long npix;
	u8 * stackp;
	u8 * stack_top;
	u16 clr;
	u16 eof;
	u16 free;
	u16 act_code = 0;
	u16 old_code = 0;
	u16 read_byt;
	u16 ab_prfx[4096];
	u8 ab_suffx[4096];
	u8 ab_stack[4096];
	u8 flags;
	u8 casspecial = 0;

	if(!i) return -1;

	i->posX = GetWord(i->parent);	/* offsetX */
	i->posY = GetWord(i->parent);	/* offsetY */
	i->width = GetWord(i->parent);	/* SizeX   */
	i->height = GetWord(i->parent);	/* SizeY   */

	if((i->width > i->parent->width) || (i->height > i->parent->height)) {
#if !defined(NGIFLIB_NO_FILE)
		if(i->parent->log) fprintf(i->parent->log, "*** ERROR *** Image bigger than global GIF canvas !\n");
#endif
		return -1;
	}
	if((i->posX + i->width) > i->parent->width) {
#if !defined(NGIFLIB_NO_FILE)
		if(i->parent->log) fprintf(i->parent->log, "*** WARNING *** Adjusting X position\n");
#endif
		i->posX = i->parent->width - i->width;
	}
	if((i->posY + i->height) > i->parent->height) {
#if !defined(NGIFLIB_NO_FILE)
		if(i->parent->log) fprintf(i->parent->log, "*** WARNING *** Adjusting Y position\n");
#endif
		i->posY = i->parent->height - i->height;
	}
	context.Xtogo = i->width;
	context.curY = i->posY;
#ifdef NGIFLIB_INDEXED_ONLY
	#ifdef NGIFLIB_ENABLE_CALLBACKS
	context.line_p.p8 = i->parent->frbuff.p8 + (u32)i->posY*i->parent->width;
	context.frbuff_p.p8 = context.line_p.p8 + i->posX;
	#else
	context.frbuff_p.p8 = i->parent->frbuff.p8 + (u32)i->posY*i->parent->width + i->posX;
	#endif /* NGIFLIB_ENABLE_CALLBACKS */
#else
	if(i->parent->mode & NGIFLIB_MODE_INDEXED) {
		#ifdef NGIFLIB_ENABLE_CALLBACKS
		context.line_p.p8 = i->parent->frbuff.p8 + (u32)i->posY*i->parent->width;
		context.frbuff_p.p8 = context.line_p.p8 + i->posX;
		#else
		context.frbuff_p.p8 = i->parent->frbuff.p8 + (u32)i->posY*i->parent->width + i->posX;
		#endif /* NGIFLIB_ENABLE_CALLBACKS */
	} else {
		#ifdef NGIFLIB_ENABLE_CALLBACKS
		context.line_p.p32 = i->parent->frbuff.p32 + (u32)i->posY*i->parent->width;
		context.frbuff_p.p32 = context.line_p.p32 + i->posX;
		#else
		context.frbuff_p.p32 = i->parent->frbuff.p32 + (u32)i->posY*i->parent->width + i->posX;
		#endif /* NGIFLIB_ENABLE_CALLBACKS */
	}
#endif /* NGIFLIB_INDEXED_ONLY */

	npix = (long)i->width * i->height;
	flags = GetByte(i->parent);
	i->interlaced = (flags & 64) >> 6;
	context.pass = i->interlaced ? 1 : 0;
	i->sort_flag = (flags & 32) >> 5;	/* is local palette sorted by color frequency ? */
	i->localpalbits = (flags & 7) + 1;
	if(flags&128) { /* palette locale */
		int k;
		int localpalsize = 1 << i->localpalbits;
#if !defined(NGIFLIB_NO_FILE)
		if(i->parent && i->parent->log) fprintf(i->parent->log, "Local palette\n");
#endif /* !defined(NGIFLIB_NO_FILE) */
		i->palette = (struct ngiflib_rgb *)ngiflib_malloc(sizeof(struct ngiflib_rgb)*localpalsize);
		for(k=0; k<localpalsize; k++) {
			i->palette[k].r = GetByte(i->parent);
			i->palette[k].g = GetByte(i->parent);
			i->palette[k].b = GetByte(i->parent);
		}
#ifdef NGIFLIB_ENABLE_CALLBACKS
		if(i->parent->palette_cb) i->parent->palette_cb(i->parent, i->palette, localpalsize);
#endif /* NGIFLIB_ENABLE_CALLBACKS */
	} else {
		i->palette = i->parent->palette;
		i->localpalbits = i->parent->imgbits;
	}
	i->ncolors = 1 << i->localpalbits;
	
	i->imgbits = GetByte(i->parent);	/* LZW Minimum Code Size */

#if !defined(NGIFLIB_NO_FILE)
	if(i->parent && i->parent->log) {
		if(i->interlaced) fprintf(i->parent->log, "interlaced ");
		fprintf(i->parent->log, "img pos(%hu,%hu) size %hux%hu palbits=%hhu imgbits=%hhu ncolors=%hu\n",
	       i->posX, i->posY, i->width, i->height, i->localpalbits, i->imgbits, i->ncolors);
	}
#endif /* !defined(NGIFLIB_NO_FILE) */

	if(i->imgbits==1) {	/* fix for 1bit images ? */
		i->imgbits = 2;
	}
	clr = 1 << i->imgbits;
	eof = clr + 1;
	free = clr + 2;
	context.nbbit = i->imgbits + 1;
	context.max = clr + clr - 1; /* (1 << context.nbbit) - 1 */
	stackp = stack_top = ab_stack + 4096;
	
	context.restbits = 0;	/* initialise le "buffer" de lecture */
	context.restbyte = 0;	/* des codes LZW */
	context.lbyte = 0;
	for(;;) {
		act_code = GetGifWord(i, &context);
		if(act_code==eof) {
#if !defined(NGIFLIB_NO_FILE)
			if(i->parent && i->parent->log) fprintf(i->parent->log, "End of image code\n");
#endif /* !defined(NGIFLIB_NO_FILE) */
			return 0;
		}
		if(npix==0) {
#if !defined(NGIFLIB_NO_FILE)
			if(i->parent && i->parent->log) fprintf(i->parent->log, "assez de pixels, On se casse !\n");
#endif /* !defined(NGIFLIB_NO_FILE) */
			return 1;
		}	
		if(act_code==clr) {
#if !defined(NGIFLIB_NO_FILE)
			if(i->parent && i->parent->log) fprintf(i->parent->log, "Code clear (free=%hu) npix=%ld\n", free, npix);
#endif /* !defined(NGIFLIB_NO_FILE) */
			/* clear */
			free = clr + 2;
			context.nbbit = i->imgbits + 1;
			context.max = clr + clr - 1; /* (1 << context.nbbit) - 1 */
			act_code = GetGifWord(i, &context);
			casspecial = (u8)act_code;
			old_code = act_code;
			if(npix > 0) WritePixel(i, &context, casspecial);
			npix--;
		} else {
			read_byt = act_code;
			if(act_code >= free) {	/* code pas encore dans alphabet */
/*				printf("Code pas dans alphabet : %d>=%d push %d\n", act_code, free, casspecial); */
				*(--stackp) = casspecial; /* dernier debut de chaine ! */
				act_code = old_code;
			}
/*			printf("actcode=%d\n", act_code); */
			while(act_code > clr) { /* code non concret */
				/* fillstackloop empile les suffixes ! */
				*(--stackp) = ab_suffx[act_code];
				act_code = ab_prfx[act_code];	/* prefixe */
			}
			/* act_code est concret */
			casspecial = (u8)act_code;	/* dernier debut de chaine ! */
			*(--stackp) = casspecial;	/* push on stack */
			if(npix >= (stack_top - stackp)) {
				WritePixels(i, &context, stackp, stack_top - stackp);	/* unstack all pixels at once */
			} else if(npix > 0) {	/* "pixel overflow" */
				WritePixels(i, &context, stackp, npix);
			}
			npix -= (stack_top - stackp);
			stackp = stack_top;
/*			putchar('\n'); */
			if(free < 4096) { /* la taille du dico est 4096 max ! */
				ab_prfx[free] = old_code;
				ab_suffx[free] = (u8)act_code;
				free++;
				if((free > context.max) && (context.nbbit < 12)) {
					context.nbbit++;	/* 1 bit de plus pour les codes LZW */
					context.max += context.max + 1;
				}
			}
			old_code = read_byt;
		}
			
	}
	return 0;
}

/* ------------------------------------------------ 
 * int LoadGif(struct ngiflib_gif *);
 * s'assurer que nimg=0 au depart !
 * retourne : 
 *    0 si GIF termin
 *    un nombre negatif si ERREUR
 *    1 si image Decode
 * rappeler pour decoder les images suivantes
 * ------------------------------------------------ */
int LoadGif(struct ngiflib_gif * g) {
	struct ngiflib_gce gce;
	u8 sign;
	u8 tmp;
	int i;

	if(!g) return -1;
	gce.gce_present = 0;
	
	if(g->nimg==0) {
		GetByteStr(g, g->signature, 6);
		g->signature[6] = '\0';
		if(   g->signature[0] != 'G'
		   || g->signature[1] != 'I'
		   || g->signature[2] != 'F'
		   || g->signature[3] != '8') {
			return -1;
		}
#if !defined(NGIFLIB_NO_FILE)
		if(g->log) fprintf(g->log, "%s\n", g->signature);
#endif /* !defined(NGIFLIB_NO_FILE) */

		g->width = GetWord(g);
		g->height = GetWord(g);
		/* allocate frame buffer */
#ifndef NGIFLIB_INDEXED_ONLY
		if((g->mode & NGIFLIB_MODE_INDEXED)==0)
			g->frbuff.p32 = ngiflib_malloc(4*(long)g->height*(long)g->width);
		else
#endif /* NGIFLIB_INDEXED_ONLY */
			g->frbuff.p8 = ngiflib_malloc((long)g->height*(long)g->width);

		tmp = GetByte(g);/* <Packed Fields> = Global Color Table Flag       1 Bit
		                                      Color Resolution              3 Bits
		                                      Sort Flag                     1 Bit
		                                      Size of Global Color Table    3 Bits */
		g->colorresolution = ((tmp & 0x70) >> 4) + 1;
		g->sort_flag = (tmp & 8) >> 3;
		g->imgbits = (tmp & 7) + 1;	/* Global Palette color resolution */
		g->ncolors = 1 << g->imgbits;

		g->backgroundindex = GetByte(g);

#if !defined(NGIFLIB_NO_FILE)
		if(g->log) fprintf(g->log, "%hux%hu %hhubits %hu couleurs  bg=%hhu\n",
		                   g->width, g->height, g->imgbits, g->ncolors, g->backgroundindex);
#endif /* NGIFLIB_INDEXED_ONLY */

		g->pixaspectratio = GetByte(g);	/* pixel aspect ratio (0 : unspecified) */

		if(tmp&0x80) {
			/* la palette globale suit. */
			g->palette = (struct ngiflib_rgb *)ngiflib_malloc(sizeof(struct ngiflib_rgb)*g->ncolors);
			for(i=0; i<g->ncolors; i++) {
				g->palette[i].r = GetByte(g);
				g->palette[i].g = GetByte(g);
				g->palette[i].b = GetByte(g);
#if defined(DEBUG) && !defined(NGIFLIB_NO_FILE)
				if(g->log) fprintf(g->log, "%3d %02X %02X %02X\n", i, g->palette[i].r,g->palette[i].g,g->palette[i].b);
#endif /* defined(DEBUG) && !defined(NGIFLIB_NO_FILE) */
			}
#ifdef NGIFLIB_ENABLE_CALLBACKS
			if(g->palette_cb) g->palette_cb(g, g->palette, g->ncolors);
#endif /* NGIFLIB_ENABLE_CALLBACKS */
		} else {
			g->palette = NULL;
		}
		g->netscape_loop_count = -1;
	}

	for(;;) {
		char appid_auth[11];
		u8 id,size;
		int blockindex;

		sign = GetByte(g);	/* signature du prochain bloc */
#if !defined(NGIFLIB_NO_FILE)
		if(g->log) fprintf(g->log, "BLOCK SIGNATURE 0x%02X '%c'\n", sign, (sign >= 32) ? sign : '.');
#endif /* NGIFLIB_INDEXED_ONLY */
		switch(sign) {
		case 0x3B:	/* END OF GIF */
			return 0;
		case '!':	/* Extension introducer 0x21 */
			id = GetByte(g);
			blockindex = 0;
#if !defined(NGIFLIB_NO_FILE)
			if(g->log) fprintf(g->log, "extension (id=0x%02hhx)\n", id);
#endif /* NGIFLIB_NO_FILE */
			while( (size = GetByte(g)) ) {
				u8 ext[256];

				GetByteStr(g, ext, size);

				switch(id) {
				case 0xF9:	/* Graphic Control Extension */
					/* The scope of this extension is the first graphic
					 * rendering block to follow. */
					gce.gce_present = 1;
					gce.disposal_method = (ext[0] >> 2) & 7;
					gce.transparent_flag = ext[0] & 1;
					gce.user_input_flag = (ext[0] >> 1) & 1;
					gce.delay_time = ext[1] | (ext[2]<<8);
					gce.transparent_color = ext[3];
#if !defined(NGIFLIB_NO_FILE)
					if(g->log) fprintf(g->log, "disposal_method=%hhu delay_time=%hu (transp=%hhu)transparent_color=0x%02hhX\n",
					       gce.disposal_method, gce.delay_time, gce.transparent_flag, gce.transparent_color);
#endif /* NGIFLIB_INDEXED_ONLY */
					/* this propably should be adjusted depending on the disposal_method
					 * of the _previous_ image. */
					if(gce.transparent_flag && ((g->nimg == 0) || gce.disposal_method == 2)) {
						FillGifBackGround(g);
					}
					break;
				case 0xFE:	/* Comment Extension. */
#if !defined(NGIFLIB_NO_FILE)
					if(g->log) {
						if(blockindex==0) fprintf(g->log, "-------------------- Comment extension --------------------\n");
						ext[size] = '\0';
						fputs((char *)ext, g->log);
					}
#endif /* NGIFLIB_NO_FILE */
					break;
				case 0xFF:	/* application extension */
					/* NETSCAPE2.0 extension :
					 * http://www.vurdalakov.net/misc/gif/netscape-looping-application-extension */
					if(blockindex==0) {
						ngiflib_memcpy(appid_auth, ext, 11);
#if !defined(NGIFLIB_NO_FILE)
						if(g->log) {
							fprintf(g->log, "---------------- Application extension ---------------\n");
							fprintf(g->log, "Application identifier : '%.8s', auth code : %02X %02X %02X (",
							        appid_auth, ext[8], ext[9], ext[10]);
							fputc((ext[8]<32)?' ':ext[8], g->log);
							fputc((ext[9]<32)?' ':ext[9], g->log);
							fputc((ext[10]<32)?' ':ext[10], g->log);
							fprintf(g->log, ")\n");
						}
#endif /* NGIFLIB_INDEXED_ONLY */
					} else {
#if !defined(NGIFLIB_NO_FILE)
						if(g->log) {
							fprintf(g->log, "Datas (as hex) : ");
							for(i=0; i<size; i++) {
								fprintf(g->log, "%02x ", ext[i]);
							}
							fprintf(g->log, "\nDatas (as text) : '");
							for(i=0; i<size; i++) {
								putc((ext[i]<32)?' ':ext[i], g->log);
							}
							fprintf(g->log, "'\n");
						}
#endif /* NGIFLIB_INDEXED_ONLY */
						if(0 == ngiflib_memcmp(appid_auth, "NETSCAPE2.0", 11)) {
							/* ext[0] : Sub-block ID */
							if(ext[0] == 1) {
								/* 1 : Netscape Looping Extension. */
								g->netscape_loop_count = (int)ext[1] | ((int)ext[2] << 8);
#if !defined(NGIFLIB_NO_FILE)
								if(g->log) {
									fprintf(g->log, "NETSCAPE loop_count = %d\n", g->netscape_loop_count);
								}
#endif /* NGIFLIB_NO_FILE */
							}
						}
					}
					break;
				case 0x01:	/* plain text extension */
#if !defined(NGIFLIB_NO_FILE)
					if(g->log) {
						fprintf(g->log, "Plain text extension   blockindex=%d\n", blockindex);
						for(i=0; i<size; i++) {
							putc((ext[i]<32)?' ':ext[i], g->log);
						}
						putc('\n', g->log);
					}
#endif /* NGIFLIB_INDEXED_ONLY */
					break;
				}
				blockindex++;
			}
			switch(id) {
			case 0x01:	/* plain text extension */
			case 0xFE:	/* Comment Extension. */
			case 0xFF:	/* application extension */
#if !defined(NGIFLIB_NO_FILE)
				if(g->log) {
					fprintf(g->log, "-----------------------------------------------------------\n");
				}
#endif /* NGIFLIB_NO_FILE */
				break;
			}
			break;
		case 0x2C:	/* Image separator */
			if(g->nimg==0) {
				g->cur_img = ngiflib_malloc(sizeof(struct ngiflib_img));
				g->first_img = g->cur_img;
			} else {
				g->cur_img->next = ngiflib_malloc(sizeof(struct ngiflib_img));
				g->cur_img = g->cur_img->next;
			}
			g->cur_img->next = NULL;
			g->cur_img->parent = g;
			if(gce.gce_present) {
				ngiflib_memcpy(&g->cur_img->gce, &gce, sizeof(struct ngiflib_gce));
			} else {
				ngiflib_memset(&g->cur_img->gce, 0,  sizeof(struct ngiflib_gce));
			}
			DecodeGifImg(g->cur_img);
			g->nimg++;

			tmp = GetByte(g);/* 0 final */
#if !defined(NGIFLIB_NO_FILE)
			if(g->log) fprintf(g->log, "ZERO TERMINATOR 0x%02X\n", tmp);
#endif /* NGIFLIB_INDEXED_ONLY */
			return 1;	/* image decode */
		default:
			/* unexpected byte */
#if !defined(NGIFLIB_NO_FILE)
			if(g->log) fprintf(g->log, "unexpected signature 0x%02X\n", sign);
#endif /* NGIFLIB_INDEXED_ONLY */
			return -1;
		}
	}
}

u32 GifIndexToTrueColor(struct ngiflib_rgb * palette, u8 v) {
	return palette[v].b | (palette[v].g << 8) | (palette[v].r << 16);
}
