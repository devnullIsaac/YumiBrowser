/*
    Font Bindings for WebAssembly
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "font_bindings.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <fontconfig/fontconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#define B ((FontBindings *)env)

#define FONT_MAX_SIZE        (64 * 1024 * 1024)
#define FONT_MAX_GLYPHS_REQ  1024
#define FONT_MAX_SEGMENTS    (64 * 1024)
#define FONT_BITMAP_DEFAULT_SIZE 64
#define GLYPH_INFO_SIZE  48
#define SEGMENT_SIZE     48
#define GLYPH_BITMAP_INFO_SIZE 24
#define SEG_LINE  1
#define SEG_QUAD  2
#define SEG_CUBIC 3

typedef struct {
    uint8_t  info[GLYPH_INFO_SIZE];
    uint8_t *segments;
    uint32_t segment_count;
    uint8_t *bitmap;
    uint32_t bmp_width;
    uint32_t bmp_height;
    float    bmp_advance_x;
    float    bmp_bearing_x;
    float    bmp_bearing_y;
    int      bmp_pixel_size;
} FontCachedGlyph;

struct FontEntry {
    FT_Face   face;
    uint8_t  *font_data;
    uint32_t  font_data_size;
    FontCachedGlyph **cache;
    uint32_t          cache_size;
};

static FT_Library g_ft_library = NULL;
static int        g_ft_refcount = 0;

static int ft_ensure(void) {
    if (g_ft_library) { g_ft_refcount++; return 1; }
    if (FT_Init_FreeType(&g_ft_library)) { fprintf(stderr, "[font] FreeType init failed\n"); return 0; }
    g_ft_refcount = 1; printf("[font] FreeType initialized\n"); return 1;
}
static void ft_release(void) {
    if (--g_ft_refcount <= 0 && g_ft_library) { FT_Done_FreeType(g_ft_library); g_ft_library = NULL; g_ft_refcount = 0; }
}

typedef struct { float p[4][2]; int type; int contour_index; } RawSegment;
typedef struct { RawSegment *segs; int count; int cap; int current_contour; float last[2]; } DecompCtx;

static void decomp_push(DecompCtx *ctx, int type, float p[4][2]) {
    if (ctx->count >= ctx->cap) { ctx->cap = ctx->cap ? ctx->cap * 2 : 64;
        ctx->segs = realloc(ctx->segs, ctx->cap * sizeof(RawSegment)); }
    RawSegment *s = &ctx->segs[ctx->count++]; s->type = type; s->contour_index = ctx->current_contour;
    memcpy(s->p, p, sizeof(s->p));
}
static int decomp_move_to(const FT_Vector *to, void *user) {
    DecompCtx *ctx = user; if (ctx->count > 0) ctx->current_contour++;
    ctx->last[0] = (float)to->x; ctx->last[1] = (float)to->y; return 0;
}
static int decomp_line_to(const FT_Vector *to, void *user) {
    DecompCtx *ctx = user;
    float p[4][2] = {{ctx->last[0],ctx->last[1]},{(float)to->x,(float)to->y},{0,0},{0,0}};
    decomp_push(ctx, SEG_LINE, p); ctx->last[0]=p[1][0]; ctx->last[1]=p[1][1]; return 0;
}
static int decomp_conic_to(const FT_Vector *ctrl, const FT_Vector *to, void *user) {
    DecompCtx *ctx = user;
    float p[4][2] = {{ctx->last[0],ctx->last[1]},{(float)ctrl->x,(float)ctrl->y},{(float)to->x,(float)to->y},{0,0}};
    decomp_push(ctx, SEG_QUAD, p); ctx->last[0]=p[2][0]; ctx->last[1]=p[2][1]; return 0;
}
static int decomp_cubic_to(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user) {
    DecompCtx *ctx = user;
    float p[4][2] = {{ctx->last[0],ctx->last[1]},{(float)c1->x,(float)c1->y},{(float)c2->x,(float)c2->y},{(float)to->x,(float)to->y}};
    decomp_push(ctx, SEG_CUBIC, p); ctx->last[0]=p[3][0]; ctx->last[1]=p[3][1]; return 0;
}
static const FT_Outline_Funcs g_decomp_funcs = {
    .move_to=decomp_move_to,.line_to=decomp_line_to,.conic_to=decomp_conic_to,.cubic_to=decomp_cubic_to,.shift=0,.delta=0 };

static float vec2_dist(float ax,float ay,float bx,float by) { float dx=bx-ax,dy=by-ay; return sqrtf(dx*dx+dy*dy); }
static float seg_arc_length(int type, float p[4][2]) {
    if (type == SEG_LINE) return vec2_dist(p[0][0],p[0][1],p[1][0],p[1][1]);
    const int N = 16; float len = 0, px = p[0][0], py = p[0][1];
    for (int i = 1; i <= N; i++) { float t=(float)i/N,u=1.0f-t; float x,y;
        if (type==SEG_QUAD){x=u*u*p[0][0]+2*u*t*p[1][0]+t*t*p[2][0];y=u*u*p[0][1]+2*u*t*p[1][1]+t*t*p[2][1];}
        else{x=u*u*u*p[0][0]+3*u*u*t*p[1][0]+3*u*t*t*p[2][0]+t*t*t*p[3][0];
             y=u*u*u*p[0][1]+3*u*u*t*p[1][1]+3*u*t*t*p[2][1]+t*t*t*p[3][1];}
        len+=vec2_dist(px,py,x,y);px=x;py=y;} return len;
}

static void cache_insert(FontEntry *fe, uint32_t glyph_id, FontCachedGlyph *cg) {
    if (glyph_id >= fe->cache_size) {
        uint32_t ns = glyph_id + 256;
        fe->cache = realloc(fe->cache, ns * sizeof(FontCachedGlyph *));
        memset(fe->cache + fe->cache_size, 0, (ns - fe->cache_size) * sizeof(FontCachedGlyph *));
        fe->cache_size = ns;
    }
    fe->cache[glyph_id] = cg;
}

static FontCachedGlyph *extract_glyph_outline(FontEntry *fe, uint32_t glyph_id, FT_GlyphSlot slot) {
    DecompCtx dctx = { 0 };
    if (FT_Outline_Decompose(&slot->outline, &g_decomp_funcs, &dctx)) { free(dctx.segs); return NULL; }
    int nc = dctx.current_contour + (dctx.count > 0 ? 1 : 0);
    FT_BBox bbox; FT_Outline_Get_CBox(&slot->outline, &bbox);
    float adv=(float)slot->metrics.horiAdvance, bx=(float)slot->metrics.horiBearingX, by=(float)slot->metrics.horiBearingY;
    float bmin_x=(float)bbox.xMin,bmin_y=(float)bbox.yMin,bmax_x=(float)bbox.xMax,bmax_y=(float)bbox.yMax;
    float ta=0; for(int i=0;i<dctx.count;i++) ta+=seg_arc_length(dctx.segs[i].type,dctx.segs[i].p);
    FontCachedGlyph *cg = calloc(1, sizeof(FontCachedGlyph));
    { uint8_t *p=cg->info; uint32_t so=0,sc=(uint32_t)dctx.count,cc=(uint32_t)nc;
      memcpy(p+0,&glyph_id,4);memcpy(p+4,&adv,4);memcpy(p+8,&bx,4);memcpy(p+12,&by,4);
      memcpy(p+16,&bmin_x,4);memcpy(p+20,&bmin_y,4);memcpy(p+24,&bmax_x,4);memcpy(p+28,&bmax_y,4);
      memcpy(p+32,&so,4);memcpy(p+36,&sc,4);memcpy(p+40,&cc,4);memcpy(p+44,&ta,4); }
    cg->segment_count = (uint32_t)dctx.count;
    if (dctx.count > 0) {
        cg->segments = malloc((size_t)dctx.count * SEGMENT_SIZE);
        for (int i = 0; i < dctx.count; i++) {
            uint8_t *p = cg->segments + i * SEGMENT_SIZE; RawSegment *rs = &dctx.segs[i];
            uint32_t ty=(uint32_t)rs->type,ci=(uint32_t)rs->contour_index; float arc=seg_arc_length(rs->type,rs->p); uint32_t pad=0;
            memcpy(p+0,&ty,4);memcpy(p+4,&ci,4);
            memcpy(p+8,&rs->p[0][0],4);memcpy(p+12,&rs->p[0][1],4);
            memcpy(p+16,&rs->p[1][0],4);memcpy(p+20,&rs->p[1][1],4);
            memcpy(p+24,&rs->p[2][0],4);memcpy(p+28,&rs->p[2][1],4);
            memcpy(p+32,&rs->p[3][0],4);memcpy(p+36,&rs->p[3][1],4);
            memcpy(p+40,&arc,4);memcpy(p+44,&pad,4);
        }
    }
    free(dctx.segs);
    cache_insert(fe, glyph_id, cg);
    return cg;
}

static FontCachedGlyph *extract_glyph_bitmap(FontEntry *fe, uint32_t glyph_id) {
    int pixel_size = FONT_BITMAP_DEFAULT_SIZE;
    if (fe->face->num_fixed_sizes > 0) {
        int best_idx = 0;
        int best_h = fe->face->available_sizes[0].height;
        for (int i = 1; i < fe->face->num_fixed_sizes; i++) {
            int h = fe->face->available_sizes[i].height;
            if (h <= FONT_BITMAP_DEFAULT_SIZE && h > best_h) { best_h = h; best_idx = i; }
            else if (best_h > FONT_BITMAP_DEFAULT_SIZE && h < best_h) { best_h = h; best_idx = i; }
        }
        pixel_size = best_h;
        if (FT_Select_Size(fe->face, best_idx)) return NULL;
    } else {
        if (FT_Set_Pixel_Sizes(fe->face, 0, (FT_UInt)pixel_size)) return NULL;
    }
    FT_Int32 load_flags = FT_LOAD_COLOR;
    if (FT_Load_Glyph(fe->face, glyph_id, load_flags)) {
        load_flags = FT_LOAD_DEFAULT;
        if (FT_Load_Glyph(fe->face, glyph_id, load_flags)) return NULL;
    }
    FT_GlyphSlot slot = fe->face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_BITMAP)
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL)) return NULL;
    FT_Bitmap *bmp = &slot->bitmap;
    if (bmp->width == 0 || bmp->rows == 0) return NULL;
    uint32_t w = bmp->width, h = bmp->rows;
    uint8_t *rgba = malloc(w * h * 4);
    if (!rgba) return NULL;
    if (bmp->pixel_mode == FT_PIXEL_MODE_BGRA) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src = bmp->buffer + y * (uint32_t)bmp->pitch;
            uint8_t *dst = rgba + y * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                dst[x*4+0]=src[x*4+2]; dst[x*4+1]=src[x*4+1]; dst[x*4+2]=src[x*4+0]; dst[x*4+3]=src[x*4+3];
            }
        }
    } else if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src = bmp->buffer + y * (uint32_t)bmp->pitch;
            uint8_t *dst = rgba + y * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                dst[x*4+0]=255; dst[x*4+1]=255; dst[x*4+2]=255; dst[x*4+3]=src[x];
            }
        }
    } else { free(rgba); return NULL; }
    float upm = (float)fe->face->units_per_EM;
    if (upm <= 0) upm = 2048.0f;
    float stf = upm / (float)pixel_size;
    float advance_x = (float)(slot->advance.x >> 6) * stf;
    float bearing_x = (float)slot->bitmap_left * stf;
    float bearing_y = (float)slot->bitmap_top * stf;
    float gw = (float)w * stf, gh = (float)h * stf;
    FontCachedGlyph *cg = calloc(1, sizeof(FontCachedGlyph));
    { uint8_t *p=cg->info; uint32_t so=0,sc=0,cc=0; float ta=0;
      memcpy(p+0,&glyph_id,4);memcpy(p+4,&advance_x,4);memcpy(p+8,&bearing_x,4);memcpy(p+12,&bearing_y,4);
      float bmin_x=bearing_x,bmin_y=bearing_y-gh,bmax_x=bearing_x+gw,bmax_y=bearing_y;
      memcpy(p+16,&bmin_x,4);memcpy(p+20,&bmin_y,4);memcpy(p+24,&bmax_x,4);memcpy(p+28,&bmax_y,4);
      memcpy(p+32,&so,4);memcpy(p+36,&sc,4);memcpy(p+40,&cc,4);memcpy(p+44,&ta,4); }
    cg->segments=NULL; cg->segment_count=0;
    cg->bitmap=rgba; cg->bmp_width=w; cg->bmp_height=h;
    cg->bmp_advance_x=advance_x; cg->bmp_bearing_x=bearing_x;
    cg->bmp_bearing_y=bearing_y; cg->bmp_pixel_size=pixel_size;
    cache_insert(fe, glyph_id, cg);
    return cg;
}

static FontCachedGlyph *extract_glyph(FontEntry *fe, uint32_t glyph_id) {
    if (glyph_id < fe->cache_size && fe->cache[glyph_id]) return fe->cache[glyph_id];
    if (!FT_Load_Glyph(fe->face, glyph_id, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP)) {
        FT_GlyphSlot slot = fe->face->glyph;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE && slot->outline.n_points > 0) {
            FontCachedGlyph *cg = extract_glyph_outline(fe, glyph_id, slot);
            if (cg) return cg;
        }
    }
    return extract_glyph_bitmap(fe, glyph_id);
}

static int validate_font_magic_fb(const uint8_t *data, uint32_t size) {
    if (size < 12) return 0;
    uint8_t b0=data[0], b1=data[1], b2=data[2], b3=data[3];
    /* TrueType */
    if (b0==0x00 && b1==0x01 && b2==0x00 && b3==0x00) return 1;
    /* OpenType (CFF) */
    if (b0==0x4F && b1==0x54 && b2==0x54 && b3==0x4F) return 1;
    /* TrueType Collection */
    if (b0==0x74 && b1==0x74 && b2==0x63 && b3==0x66) return 1;
    /* WOFF */
    if (b0==0x77 && b1==0x4F && b2==0x46 && b3==0x46) return 1;
    /* WOFF2 */
    if (b0==0x77 && b1==0x4F && b2==0x46 && b3==0x32) return 1;
    return 0;
}

static FontEntry *font_entry_create(const uint8_t *data, uint32_t size) {
    if (!validate_font_magic_fb(data, size)) {
        fprintf(stderr, "[font] Not a font file (bad magic: 0x%02X%02X%02X%02X, %u bytes)\n",
                size >= 4 ? data[0] : 0, size >= 4 ? data[1] : 0,
                size >= 4 ? data[2] : 0, size >= 4 ? data[3] : 0, size);
        return NULL;
    }
    FontEntry *fe = calloc(1, sizeof(FontEntry));
    fe->font_data = malloc(size); memcpy(fe->font_data, data, size); fe->font_data_size = size;
    if (FT_New_Memory_Face(g_ft_library, fe->font_data, (FT_Long)size, 0, &fe->face)) {
        fprintf(stderr, "[font] FT_New_Memory_Face failed (%u bytes)\n", size);
        free(fe->font_data); free(fe); return NULL;
    }
    fe->cache_size = (uint32_t)fe->face->num_glyphs + 1;
    fe->cache = calloc(fe->cache_size, sizeof(FontCachedGlyph *));
    printf("[font] Loaded: %s %s (%ld glyphs, %d UPM)\n",
           fe->face->family_name?fe->face->family_name:"?",
           fe->face->style_name?fe->face->style_name:"",
           fe->face->num_glyphs,(int)fe->face->units_per_EM);
    return fe;
}
static FontEntry *font_entry_create_from_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
    if (len<=0||len>FONT_MAX_SIZE) {
        if (len > FONT_MAX_SIZE)
            fprintf(stderr, "[font] File too large: %s (%ld bytes, max %d)\n", path, len, FONT_MAX_SIZE);
        fclose(f); return NULL;
    }
    uint8_t *data=malloc((size_t)len); if(!data){fclose(f);return NULL;}
    size_t read = fread(data,1,(size_t)len,f); fclose(f);
    if ((long)read != len) {
        fprintf(stderr, "[font] Short read: %s (got %zu of %ld bytes)\n", path, read, len);
        free(data); return NULL;
    }
    FontEntry *fe = font_entry_create(data,(uint32_t)len);
    if (!fe) fprintf(stderr, "[font] FreeType rejected: %s (%ld bytes)\n", path, len);
    free(data); return fe;
}
static void font_cached_glyph_free(FontCachedGlyph *cg) {
    if(!cg)return; free(cg->segments); free(cg->bitmap); free(cg);
}
static void font_entry_destroy(FontEntry *fe) {
    if(!fe)return;
    for(uint32_t i=0;i<fe->cache_size;i++) font_cached_glyph_free(fe->cache[i]);
    free(fe->cache); if(fe->face)FT_Done_Face(fe->face); free(fe->font_data); free(fe);
}

/* ================================================================== */
/*  System font search (by name — used by font_load_system WASM import) */
/* ================================================================== */

static const char *DEFAULT_FONT_DIRS[] = {"/usr/share/fonts","/usr/local/share/fonts",NULL};
static char *find_font_in_dir(const char *dir, const char *name, int depth) {
    if(depth>5)return NULL; DIR *d=opendir(dir); if(!d)return NULL;
    struct dirent *ent;
    while((ent=readdir(d))!=NULL){
        if(ent->d_name[0]=='.')continue;
        char path[1024]; snprintf(path,sizeof(path),"%s/%s",dir,ent->d_name);
        struct stat st; if(stat(path,&st)!=0)continue;
        if(S_ISDIR(st.st_mode)){char *found=find_font_in_dir(path,name,depth+1);if(found){closedir(d);return found;}continue;}
        const char *fn=ent->d_name; size_t nlen=strlen(name),flen=strlen(fn);
        if(flen>=nlen+4){int match=1;
            for(size_t i=0;i<nlen;i++){char a=name[i],b=fn[i];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b){match=0;break;}}
            if(match&&fn[nlen]=='.'){const char *ext=fn+nlen+1;
                if((ext[0]=='t'||ext[0]=='T')&&(ext[1]=='t'||ext[1]=='T')&&(ext[2]=='f'||ext[2]=='F')&&ext[3]=='\0'){closedir(d);return strdup(path);}
                if((ext[0]=='o'||ext[0]=='O')&&(ext[1]=='t'||ext[1]=='T')&&(ext[2]=='f'||ext[2]=='F')&&ext[3]=='\0'){closedir(d);return strdup(path);}
                if((ext[0]=='t'||ext[0]=='T')&&(ext[1]=='t'||ext[1]=='T')&&(ext[2]=='c'||ext[2]=='C')&&ext[3]=='\0'){closedir(d);return strdup(path);}}}
    } closedir(d); return NULL;
}
static char *find_system_font(const char *name) {
    for(int i=0;DEFAULT_FONT_DIRS[i];i++){char *p=find_font_in_dir(DEFAULT_FONT_DIRS[i],name,0);if(p)return p;}
    const char *home=getenv("HOME");
    if(home){char hd[512];snprintf(hd,sizeof(hd),"%s/.local/share/fonts",home);
        char *p=find_font_in_dir(hd,name,0);if(p)return p;
        snprintf(hd,sizeof(hd),"%s/.fonts",home);p=find_font_in_dir(hd,name,0);if(p)return p;}
    return NULL;
}

/* ================================================================== */
/*  Fontconfig — codepoint-based font discovery                        */
/*                                                                     */
/*  Uses FcFontSort to get a RANKED LIST of candidate fonts, then      */
/*  tries each until one actually has the glyph in FreeType.           */
/*  All loaded fonts stay cached — never reload the same file.         */
/* ================================================================== */

/* ---- Path cache: remembers file path → handle (dynamic, no cap) ---- */

typedef struct {
    char    *path;
    uint32_t handle;
    int      load_failed;
} fb_path_entry_t;

static fb_path_entry_t *g_path_cache = NULL;
static int g_path_cache_count = 0;
static int g_path_cache_cap   = 0;

static fb_path_entry_t *path_cache_find(const char *path) {
    for (int i = 0; i < g_path_cache_count; i++)
        if (strcmp(g_path_cache[i].path, path) == 0)
            return &g_path_cache[i];
    return NULL;
}

static fb_path_entry_t *path_cache_insert(const char *path, uint32_t handle, int failed) {
    if (g_path_cache_count >= g_path_cache_cap) {
        int new_cap = g_path_cache_cap ? g_path_cache_cap * 2 : 128;
        g_path_cache = realloc(g_path_cache, new_cap * sizeof(fb_path_entry_t));
        g_path_cache_cap = new_cap;
    }
    fb_path_entry_t *e = &g_path_cache[g_path_cache_count++];
    e->path = strdup(path);
    e->handle = handle;
    e->load_failed = failed;
    return e;
}

static void path_cache_destroy(void) {
    for (int i = 0; i < g_path_cache_count; i++)
        free(g_path_cache[i].path);
    free(g_path_cache);
    g_path_cache = NULL;
    g_path_cache_count = 0;
    g_path_cache_cap = 0;
}

/* ---- Codepoint cache: remembers codepoint → handle ---- */
/*                                                          */
/* Open-addressing hash table with linear probing.          */
/* 65536 slots (power of 2). Fibonacci hash for scatter.    */
/* Max 32 probes before giving up — more than enough at     */
/* typical load factors. Handles ~20k distinct codepoints   */
/* without meaningful collision pressure.                   */

#define CP_CACHE_SIZE  65536u
#define CP_CACHE_MASK  (CP_CACHE_SIZE - 1u)
#define CP_CACHE_MAX_PROBE 32

typedef struct {
    uint32_t codepoint;
    uint32_t handle;    /* 0 = searched and found nothing */
    uint8_t  occupied;
} cp_cache_entry_t;

static cp_cache_entry_t g_cp_cache[CP_CACHE_SIZE];

static void cp_cache_clear(void) {
    memset(g_cp_cache, 0, sizeof(g_cp_cache));
}

static uint32_t cp_cache_hash(uint32_t cp) {
    return (cp * 2654435761u) & CP_CACHE_MASK;
}

static cp_cache_entry_t *cp_cache_find(uint32_t cp) {
    uint32_t idx = cp_cache_hash(cp);
    for (int probe = 0; probe < CP_CACHE_MAX_PROBE; probe++) {
        uint32_t i = (idx + (uint32_t)probe) & CP_CACHE_MASK;
        if (!g_cp_cache[i].occupied)
            return NULL; /* empty slot = not in table */
        if (g_cp_cache[i].codepoint == cp)
            return &g_cp_cache[i];
    }
    return NULL;
}

static void cp_cache_insert(uint32_t cp, uint32_t handle) {
    uint32_t idx = cp_cache_hash(cp);
    uint32_t insert_at = idx; /* fallback: overwrite home slot */
    for (int probe = 0; probe < CP_CACHE_MAX_PROBE; probe++) {
        uint32_t i = (idx + (uint32_t)probe) & CP_CACHE_MASK;
        if (!g_cp_cache[i].occupied || g_cp_cache[i].codepoint == cp) {
            insert_at = i;
            goto do_insert;
        }
    }
    /* All probe slots occupied — evict the home position.
       At 65536 slots this should essentially never happen. */
do_insert:
    g_cp_cache[insert_at].codepoint = cp;
    g_cp_cache[insert_at].handle    = handle;
    g_cp_cache[insert_at].occupied  = 1;
}

/* ---- Load a font file, returning its handle. Uses path cache. ---- */

static uint32_t font_load_cached(FontBindings *b, const char *path) {
    fb_path_entry_t *cached = path_cache_find(path);
    if (cached) {
        if (cached->load_failed) return 0;
        return cached->handle;
    }

    FontEntry *fe = font_entry_create_from_file(path);
    if (!fe) {
        path_cache_insert(path, 0, 1);
        return 0;
    }

    uint32_t h = htable_insert(&b->fonts, fe);
    path_cache_insert(path, h, 0);
    return h;
}

uint32_t font_bindings_find_font_for_codepoint(FontBindings *b, uint32_t codepoint) {
    cp_cache_entry_t *cp_cached = cp_cache_find(codepoint);
    if (cp_cached)
        return cp_cached->handle;

    FcCharSet *cs = FcCharSetCreate();
    if (!cs) return 0;
    FcCharSetAddChar(cs, codepoint);

    FcPattern *pat = FcPatternCreate();
    if (!pat) { FcCharSetDestroy(cs); return 0; }
    FcPatternAddCharSet(pat, FC_CHARSET, cs);

    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcFontSet *matches = FcFontSort(NULL, pat, FcTrue, NULL, &result);

    uint32_t found_handle = 0;

    if (matches) {
        for (int i = 0; i < matches->nfont && !found_handle; i++) {
            /* Check fontconfig's charset BEFORE loading the font */
            FcCharSet *font_cs = NULL;
            if (FcPatternGetCharSet(matches->fonts[i], FC_CHARSET, 0, &font_cs) != FcResultMatch
                || !font_cs)
                continue;
            if (!FcCharSetHasChar(font_cs, codepoint))
                continue;

            FcChar8 *file = NULL;
            if (FcPatternGetString(matches->fonts[i], FC_FILE, 0, &file) != FcResultMatch || !file)
                continue;

            const char *path = (const char *)file;
            uint32_t h = font_load_cached(b, path);
            if (!h) continue;

            FontEntry *fe = htable_get(&b->fonts, h);
            if (!fe) continue;

            if (FT_Get_Char_Index(fe->face, (FT_ULong)codepoint) != 0) {
                found_handle = h;
            }
        }
        FcFontSetDestroy(matches);
    }

    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);

    cp_cache_insert(codepoint, found_handle);
    return found_handle;
}

/* ================================================================== */
/*  WASM memory helpers                                                */
/* ================================================================== */

static uint8_t *wasm_mem_base(FontBindings *b){return b->memory?(uint8_t*)wasm_memory_data(b->memory):NULL;}
static size_t wasm_mem_size(FontBindings *b){return b->memory?wasm_memory_data_size(b->memory):0;}
#define ARG_I32(n) (args->data[(n)].of.i32)
#define RET_I32(v) do{res->data[0]=(wasm_val_t){.kind=WASM_I32,.of.i32=(v)};}while(0)

/* ================================================================== */
/*  WASM import functions                                              */
/* ================================================================== */

static wasm_trap_t *fn_font_load(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t dp=(uint32_t)ARG_I32(0);int32_t dl=ARG_I32(1);
    uint8_t *mem=wasm_mem_base(B);size_t msz=wasm_mem_size(B);
    if(!mem||dl<=0||dl>FONT_MAX_SIZE||(size_t)dp+(size_t)dl>msz){RET_I32(0);return NULL;}
    FontEntry *fe=font_entry_create(mem+dp,(uint32_t)dl);
    if(!fe){RET_I32(0);return NULL;}
    uint32_t h=htable_insert(&B->fonts,fe);printf("[font] Handle %u assigned\n",h);RET_I32((int32_t)h);return NULL;
}
static wasm_trap_t *fn_font_load_system(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t np=(uint32_t)ARG_I32(0);int32_t nl=ARG_I32(1);
    uint8_t *mem=wasm_mem_base(B);size_t msz=wasm_mem_size(B);
    if(!mem||nl<=0||nl>256||(size_t)np+(size_t)nl>msz){RET_I32(0);return NULL;}
    char name[257];memcpy(name,mem+np,(size_t)nl);name[nl]='\0';
    printf("[font] System font request: '%s'\n",name);
    char *path=find_system_font(name);
    if(!path){fprintf(stderr,"[font] System font '%s' not found\n",name);RET_I32(0);return NULL;}
    printf("[font] Found: %s\n",path);
    uint32_t h=font_load_cached(B,path);free(path);
    if(!h){RET_I32(0);return NULL;}
    printf("[font] Handle %u assigned (system)\n",h);RET_I32((int32_t)h);return NULL;
}
static wasm_trap_t *fn_font_unload(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    (void)res;uint32_t h=(uint32_t)ARG_I32(0);FontEntry *fe=htable_get(&B->fonts,h);
    if(fe){printf("[font] Unloading handle %u\n",h);font_entry_destroy(fe);htable_remove(&B->fonts,h);}return NULL;
}
static wasm_trap_t *fn_font_get_metrics(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t h=(uint32_t)ARG_I32(0),op=(uint32_t)ARG_I32(1);
    uint8_t *mem=wasm_mem_base(B);size_t msz=wasm_mem_size(B);
    FontEntry *fe=htable_get(&B->fonts,h);
    if(!fe||!mem||(size_t)op+16>msz){RET_I32(0);return NULL;}
    float upm=(float)fe->face->units_per_EM,asc=(float)fe->face->ascender,
          dsc=(float)fe->face->descender,lh=(float)fe->face->height;
    memcpy(mem+op+0,&upm,4);memcpy(mem+op+4,&asc,4);memcpy(mem+op+8,&dsc,4);memcpy(mem+op+12,&lh,4);
    RET_I32(1);return NULL;
}
static wasm_trap_t *fn_font_glyph_count(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    FontEntry *fe=htable_get(&B->fonts,(uint32_t)ARG_I32(0));RET_I32(fe?(int32_t)fe->face->num_glyphs:0);return NULL;
}
static wasm_trap_t *fn_font_has_glyph(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    FontEntry *fe=htable_get(&B->fonts,(uint32_t)ARG_I32(0));
    if(!fe){RET_I32(0);return NULL;}
    FT_UInt gi=FT_Get_Char_Index(fe->face,(uint32_t)ARG_I32(1));RET_I32(gi!=0?1:0);return NULL;
}
static wasm_trap_t *fn_font_get_glyph(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t h=(uint32_t)ARG_I32(0),gid=(uint32_t)ARG_I32(1),oip=(uint32_t)ARG_I32(2),osp=(uint32_t)ARG_I32(3);
    int32_t ms=ARG_I32(4);
    uint8_t *mem=wasm_mem_base(B);size_t msz=wasm_mem_size(B);FontEntry *fe=htable_get(&B->fonts,h);
    if(!fe||!mem||ms<0||ms>FONT_MAX_SEGMENTS||(size_t)oip+GLYPH_INFO_SIZE>msz||(size_t)osp+(size_t)ms*SEGMENT_SIZE>msz)
        {RET_I32(0);return NULL;}
    FontCachedGlyph *cg=extract_glyph(fe,gid);
    if(!cg){memset(mem+oip,0,GLYPH_INFO_SIZE);RET_I32(0);return NULL;}
    memcpy(mem+oip,cg->info,GLYPH_INFO_SIZE);
    uint32_t n=cg->segment_count;if(n>(uint32_t)ms)n=(uint32_t)ms;
    if(n>0&&cg->segments)memcpy(mem+osp,cg->segments,(size_t)n*SEGMENT_SIZE);
    if(n<cg->segment_count)memcpy(mem+oip+36,&n,4);
    RET_I32((int32_t)n);return NULL;
}
static wasm_trap_t *fn_font_get_codepoint_glyph_id(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    FontEntry *fe=htable_get(&B->fonts,(uint32_t)ARG_I32(0));
    if(!fe){RET_I32(0);return NULL;}
    FT_UInt gi=FT_Get_Char_Index(fe->face,(FT_ULong)(uint32_t)ARG_I32(1));RET_I32((int32_t)gi);return NULL;
}

static int rasterize_glyph_at_size(FontEntry *fe, uint32_t glyph_id,
                                   int pixel_size,
                                   uint32_t *out_w, uint32_t *out_h,
                                   float *out_adv, float *out_bx,
                                   float *out_by, int *out_ps,
                                   uint8_t **out_rgba) {
    if (FT_Set_Pixel_Sizes(fe->face, 0, (FT_UInt)pixel_size))
        return 0;
    if (FT_Load_Glyph(fe->face, glyph_id, FT_LOAD_DEFAULT))
        return 0;
    FT_GlyphSlot slot = fe->face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_BITMAP)
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
            return 0;
    FT_Bitmap *bmp = &slot->bitmap;
    if (bmp->width == 0 || bmp->rows == 0)
        return 0;
    uint32_t w = bmp->width, h = bmp->rows;
    uint8_t *rgba = malloc(w * h * 4);
    if (!rgba) return 0;
    if (bmp->pixel_mode == FT_PIXEL_MODE_BGRA) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src = bmp->buffer + y * (uint32_t)bmp->pitch;
            uint8_t *dst = rgba + y * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                dst[x*4+0]=src[x*4+2]; dst[x*4+1]=src[x*4+1];
                dst[x*4+2]=src[x*4+0]; dst[x*4+3]=src[x*4+3];
            }
        }
    } else if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *src = bmp->buffer + y * (uint32_t)bmp->pitch;
            uint8_t *dst = rgba + y * w * 4;
            for (uint32_t x = 0; x < w; x++) {
                dst[x*4+0]=255; dst[x*4+1]=255; dst[x*4+2]=255; dst[x*4+3]=src[x];
            }
        }
    } else {
        free(rgba); return 0;
    }
    float upm = (float)fe->face->units_per_EM;
    if (upm <= 0) upm = 2048.0f;
    float stf = upm / (float)pixel_size;
    *out_w   = w;
    *out_h   = h;
    *out_adv = (float)(slot->advance.x >> 6) * stf;
    *out_bx  = (float)slot->bitmap_left * stf;
    *out_by  = (float)slot->bitmap_top * stf;
    *out_ps  = pixel_size;
    *out_rgba = rgba;
    return 1;
}
static wasm_trap_t *fn_font_get_glyph_bitmap(void *env, const wasm_val_vec_t *args, wasm_val_vec_t *res) {
    uint32_t h=(uint32_t)ARG_I32(0),gid=(uint32_t)ARG_I32(1);
    int32_t ps=ARG_I32(2);
    uint32_t oip=(uint32_t)ARG_I32(3),opp=(uint32_t)ARG_I32(4);int32_t mb=ARG_I32(5);
    uint8_t *mem=wasm_mem_base(B);size_t msz=wasm_mem_size(B);FontEntry *fe=htable_get(&B->fonts,h);
    if(!fe||!mem||mb<0||(size_t)oip+GLYPH_BITMAP_INFO_SIZE>msz||(size_t)opp+(size_t)mb>msz)
        {RET_I32(0);return NULL;}

    /* ---- Sized rasterization: ps > 0 means "rasterize at this pixel size" ---- */
    if (ps > 0) {
        uint32_t w, hr; float adv, bx, by; int actual_ps; uint8_t *rgba;
        if (!rasterize_glyph_at_size(fe, gid, ps, &w, &hr, &adv, &bx, &by,
                                     &actual_ps, &rgba)) {
            memset(mem+oip, 0, GLYPH_BITMAP_INFO_SIZE);
            RET_I32(0); return NULL;
        }
        uint32_t rsz = w * hr * 4;
        if ((int32_t)rsz > mb) {
            fprintf(stderr, "[font] Bitmap glyph %u @%dpx too large: %u bytes (max %d)\n",
                    gid, ps, rsz, mb);
            free(rgba); RET_I32(0); return NULL;
        }
        uint8_t *info = mem + oip; uint32_t psu = (uint32_t)actual_ps;
        memcpy(info+0, &w, 4); memcpy(info+4, &hr, 4);
        memcpy(info+8, &adv, 4); memcpy(info+12, &bx, 4);
        memcpy(info+16, &by, 4); memcpy(info+20, &psu, 4);
        memcpy(mem+opp, rgba, rsz);
        free(rgba);
        RET_I32((int32_t)rsz); return NULL;
    }

    /* ---- ps == 0: original path — use cached glyph's native bitmap ---- */
    FontCachedGlyph *cg=extract_glyph(fe,gid);
    if(!cg||!cg->bitmap){memset(mem+oip,0,GLYPH_BITMAP_INFO_SIZE);RET_I32(0);return NULL;}
    uint32_t rsz=cg->bmp_width*cg->bmp_height*4;
    if((int32_t)rsz>mb){fprintf(stderr,"[font] Bitmap glyph %u too large: %u bytes (max %d)\n",gid,rsz,mb);RET_I32(0);return NULL;}
    uint8_t *info=mem+oip; uint32_t psu=(uint32_t)cg->bmp_pixel_size;
    memcpy(info+0,&cg->bmp_width,4);memcpy(info+4,&cg->bmp_height,4);
    memcpy(info+8,&cg->bmp_advance_x,4);memcpy(info+12,&cg->bmp_bearing_x,4);
    memcpy(info+16,&cg->bmp_bearing_y,4);memcpy(info+20,&psu,4);
    memcpy(mem+opp,cg->bitmap,rsz);
    RET_I32((int32_t)rsz);return NULL;
}

/* ================================================================== */
/*  Public API (host-internal, not WASM imports)                       */
/* ================================================================== */

const uint8_t *font_bindings_get_data(FontBindings *b, uint32_t handle, uint32_t *out_size) {
    FontEntry *fe=htable_get(&b->fonts,handle);if(!fe)return NULL;
    *out_size=fe->font_data_size;return fe->font_data;
}

uint32_t font_bindings_load_system_direct(FontBindings *b, const char *name) {
    char *path = find_system_font(name);
    if (!path) return 0;
    uint32_t h = font_load_cached(b, path);
    free(path);
    return h;
}

int font_bindings_has_glyph_direct(FontBindings *b, uint32_t handle, uint32_t codepoint) {
    FontEntry *fe = htable_get(&b->fonts, handle);
    if (!fe) return 0;
    return FT_Get_Char_Index(fe->face, (FT_ULong)codepoint) != 0;
}

/* ================================================================== */
/*  Binding table                                                      */
/* ================================================================== */

typedef struct {
    const char *name; wasm_func_callback_with_env_t cb;
    uint32_t np; wasm_valkind_t params[8]; uint32_t nr; wasm_valkind_t results[1];
} FontBindingEntry;
#define I WASM_I32
static const FontBindingEntry FONT_BINDINGS[] = {
    {"font_load",fn_font_load,2,{I,I},1,{I}},
    {"font_load_system",fn_font_load_system,2,{I,I},1,{I}},
    {"font_unload",fn_font_unload,1,{I},0,{0}},
    {"font_get_metrics",fn_font_get_metrics,2,{I,I},1,{I}},
    {"font_glyph_count",fn_font_glyph_count,1,{I},1,{I}},
    {"font_has_glyph",fn_font_has_glyph,2,{I,I},1,{I}},
    {"font_get_glyph",fn_font_get_glyph,5,{I,I,I,I,I},1,{I}},
    {"font_get_codepoint_glyph_id",fn_font_get_codepoint_glyph_id,2,{I,I},1,{I}},
    {"font_get_glyph_bitmap",fn_font_get_glyph_bitmap,6,{I,I,I,I,I,I},1,{I}},
};
#undef I
#define NUM_FONT_BINDINGS (sizeof(FONT_BINDINGS)/sizeof(FONT_BINDINGS[0]))

static wasm_functype_t *make_ft_type(uint32_t np,const wasm_valkind_t p[],uint32_t nr,const wasm_valkind_t r[]) {
    wasm_valtype_vec_t params,results;
    if(np>0){wasm_valtype_t *pt[8];for(uint32_t i=0;i<np;i++)pt[i]=wasm_valtype_new(p[i]);wasm_valtype_vec_new(&params,np,pt);}
    else wasm_valtype_vec_new_empty(&params);
    if(nr>0){wasm_valtype_t *rt[1];rt[0]=wasm_valtype_new(r[0]);wasm_valtype_vec_new(&results,nr,rt);}
    else wasm_valtype_vec_new_empty(&results);
    return wasm_functype_new(&params,&results);
}

void font_bindings_init(FontBindings *b) {
    memset(b,0,sizeof(*b)); ft_ensure(); htable_init(&b->fonts,8);
    FcInit();
    cp_cache_clear();
    printf("[font] Font bindings ready (%zu imports)\n",NUM_FONT_BINDINGS);
}
void font_bindings_set_memory(FontBindings *b, wasm_memory_t *mem){b->memory=mem;}
size_t font_bindings_get_imports(FontBindings *b, wasm_store_t *store,
                                 const char ***out_names, wasm_func_t ***out_funcs) {
    static const char *names[NUM_FONT_BINDINGS]; static wasm_func_t *funcs[NUM_FONT_BINDINGS];
    for(size_t i=0;i<NUM_FONT_BINDINGS;i++){
        names[i]=FONT_BINDINGS[i].name;
        wasm_functype_t *ft=make_ft_type(FONT_BINDINGS[i].np,FONT_BINDINGS[i].params,FONT_BINDINGS[i].nr,FONT_BINDINGS[i].results);
        funcs[i]=wasm_func_new_with_env(store,ft,FONT_BINDINGS[i].cb,b,NULL);wasm_functype_delete(ft);}
    *out_names=names;*out_funcs=funcs;return NUM_FONT_BINDINGS;
}

void font_bindings_dump_stats(FontBindings *b) {
    int total_glyphs = 0, total_segments = 0, total_bitmaps = 0;
    long bitmap_bytes = 0;
    for (uint32_t h = 1; h <= b->fonts.capacity; h++) {
        FontEntry *fe = htable_get(&b->fonts, h);
        if (!fe) continue;
        int cached = 0;
        for (uint32_t i = 0; i < fe->cache_size; i++) {
            if (!fe->cache[i]) continue;
            cached++;
            total_segments += fe->cache[i]->segment_count;
            if (fe->cache[i]->bitmap) {
                total_bitmaps++;
                bitmap_bytes += fe->cache[i]->bmp_width * fe->cache[i]->bmp_height * 4;
            }
        }
        printf("[font-stats] handle %u: %s — %d/%u glyphs cached, %u cache slots (%zu KB)\n",
               h, fe->face->family_name ? fe->face->family_name : "?",
               cached, (unsigned)fe->face->num_glyphs,
               fe->cache_size, fe->cache_size * sizeof(void*) / 1024);
        total_glyphs += cached;
    }
    printf("[font-stats] Total: %d glyphs, %d segments (%ld KB), %d bitmaps (%ld KB)\n",
           total_glyphs, total_segments,
           (long)total_segments * SEGMENT_SIZE / 1024,
           total_bitmaps, bitmap_bytes / 1024);
    printf("[font-stats] Fonts loaded: %d, path cache: %d entries\n",
           (int)b->fonts.capacity, g_path_cache_count);
}
void font_bindings_destroy(FontBindings *b) {
    font_bindings_dump_stats(b);
    for(uint32_t i=1;i<=b->fonts.capacity;i++){FontEntry *fe=htable_get(&b->fonts,i);if(fe)font_entry_destroy(fe);}
    htable_destroy(&b->fonts);path_cache_destroy();cp_cache_clear();FcFini();ft_release();memset(b,0,sizeof(*b));printf("[font] Font bindings destroyed\n");
}
