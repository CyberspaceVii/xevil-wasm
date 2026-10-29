/* xpm.h (cyberspace7 XEvil port): the libXpm subset XEvil uses, implemented
 * in xshim.cpp by parsing the compiled-in XPM C arrays directly. */
#ifndef _X11_XPM_H_
#define _X11_XPM_H_

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long Pixel;

#define XpmSuccess 0
#define XpmColorError 1
#define XpmOpenFailed -1
#define XpmFileInvalid -2
#define XpmNoMemory -3
#define XpmColorFailed -4

#define XpmVisual (1L << 0)
#define XpmColormap (1L << 1)
#define XpmDepth (1L << 2)
#define XpmSize (1L << 3)
#define XpmHotspot (1L << 4)
#define XpmCharsPerPixel (1L << 5)
#define XpmColorSymbols (1L << 6)
#define XpmRgbFilename (1L << 7)
#define XpmInfos (1L << 8)
#define XpmReturnPixels (1L << 9)
#define XpmExtensions (1L << 10)
#define XpmExactColors (1L << 11)
#define XpmCloseness (1L << 12)
#define XpmRGBCloseness (1L << 13)
#define XpmColorKey (1L << 14)
#define XpmReturnAllocPixels (1L << 16)

typedef struct {
  unsigned long valuemask;
  Visual *visual;
  Colormap colormap;
  unsigned int depth;
  unsigned int width;
  unsigned int height;
  unsigned int x_hotspot, y_hotspot;
  unsigned int cpp;
  Pixel *pixels;           /* owned by the shim; XpmFreeAttributes frees */
  unsigned int npixels;
  void *colorsymbols;
  unsigned int numsymbols;
  char *rgb_fname;
  unsigned int nextensions;
  void *extensions;
  unsigned int ncolors;
  void *colorTable;
  char *hints_cmt, *colors_cmt, *pixels_cmt;
  unsigned int mask_pixel;
  Bool exactColors;
  unsigned int closeness;
  unsigned int red_closeness, green_closeness, blue_closeness;
  int color_key;
  Pixel *alloc_pixels;
  int nalloc_pixels;
  Bool alloc_close_colors;
  int bitmap_format;
  void *alloc_color, *free_colors;
  void *color_closure;
} XpmAttributes;

int XpmCreateImageFromData(Display *, char **data, XImage **image,
                           XImage **shapemask, XpmAttributes *);
int XpmCreatePixmapFromData(Display *, Drawable, char **data,
                            Pixmap *pixmap, Pixmap *shapemask,
                            XpmAttributes *);
void XpmFreeAttributes(XpmAttributes *);

#ifdef __cplusplus
}
#endif

#endif
