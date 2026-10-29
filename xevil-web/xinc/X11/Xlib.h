/*
 * Xlib.h (cyberspace7 XEvil port): a *minimal* Xlib API surface backed by
 * xshim.cpp (a tiny in-process "X server" that rasterises into 32-bit
 * buffers and presents through SDL2). Only what XEvil 2.02's x11/ backend
 * actually calls is declared. Struct layouts follow real Xlib field names so
 * the upstream sources compile unmodified; the opaque parts (Display, GC
 * internals) are ours.
 *
 * Pixel model: depth-24 drawables hold 0x00RRGGBB; depth-1 drawables
 * (masks/bitmaps/stipples) hold 0 or 1 per pixel. XImage data is always one
 * 32-bit word per pixel (bitmap_pad 32), which is exactly what
 * Xvars::new_bytes_for_image() allocates.
 */
#ifndef _X11_XLIB_H_
#define _X11_XLIB_H_

#include <X11/X.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int Bool;
typedef int Status;
#define True 1
#define False 0

#define AllPlanes ((unsigned long)~0L)
#define QueuedAlready 0
#define QueuedAfterReading 1
#define QueuedAfterFlush 2

typedef struct {
  int c_class;
  int bits_per_rgb;
  int map_entries;
} Visual;

struct _XDisplay;

typedef struct {
  struct _XDisplay *display;
  Window root;
  int width, height;
  int root_depth;
  Visual *root_visual;
  Colormap cmap;
  unsigned long white_pixel, black_pixel;
} Screen;

typedef struct _XDisplay {
  Screen screen;
  Visual visual;
  struct _XGC *default_gc;
  void *priv;              /* xshim server state */
} Display;

typedef struct _XGC *GC;

#define DefaultScreenOfDisplay(d) (&(d)->screen)
#define DefaultScreen(d) 0
#define ScreenOfDisplay(d, n) (&(d)->screen)
#define RootWindowOfScreen(s) ((s)->root)
#define RootWindow(d, n) ((d)->screen.root)
#define DefaultRootWindow(d) ((d)->screen.root)
#define DefaultVisual(d, n) (&(d)->visual)
#define DefaultDepthOfScreen(s) ((s)->root_depth)
#define DefaultDepth(d, n) ((d)->screen.root_depth)
#define DefaultColormap(d, n) ((d)->screen.cmap)
#define WhitePixel(d, n) ((d)->screen.white_pixel)
#define BlackPixel(d, n) ((d)->screen.black_pixel)
#define DefaultGC(d, n) ((d)->default_gc)

typedef struct {
  short x, y;
} XPoint;

typedef struct {
  unsigned long pixel;
  unsigned short red, green, blue;
  char flags;
  char pad;
} XColor;

typedef struct {
  int function;
  unsigned long plane_mask;
  unsigned long foreground;
  unsigned long background;
  int line_width;
  int line_style;
  int cap_style;
  int join_style;
  int fill_style;
  int fill_rule;
  int arc_mode;
  Pixmap tile;
  Pixmap stipple;
  int ts_x_origin;
  int ts_y_origin;
  Font font;
  int subwindow_mode;
  Bool graphics_exposures;
  int clip_x_origin;
  int clip_y_origin;
  Pixmap clip_mask;
  int dash_offset;
  char dashes;
} XGCValues;

typedef struct {
  short lbearing, rbearing, width, ascent, descent;
  unsigned short attributes;
} XCharStruct;

typedef struct {
  void *ext_data;
  Font fid;
  unsigned direction;
  unsigned min_char_or_byte2, max_char_or_byte2;
  unsigned min_byte1, max_byte1;
  Bool all_chars_exist;
  unsigned default_char;
  int n_properties;
  void *properties;
  XCharStruct min_bounds, max_bounds;
  XCharStruct *per_char;   /* NULL: every baked font is fixed-cell */
  int ascent, descent;
} XFontStruct;

typedef struct _XImage {
  int width, height;
  int xoffset;
  int format;
  char *data;
  int byte_order;
  int bitmap_unit;
  int bitmap_bit_order;
  int bitmap_pad;
  int depth;
  int bytes_per_line;
  int bits_per_pixel;
} XImage;

/* ---- events ---- */
typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
} XAnyEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root, subwindow;
  Time time;
  int x, y, x_root, y_root;
  unsigned int state;
  unsigned int keycode;
  Bool same_screen;
} XKeyEvent;
typedef XKeyEvent XKeyPressedEvent;
typedef XKeyEvent XKeyReleasedEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Window root, subwindow;
  Time time;
  int x, y, x_root, y_root;
  unsigned int state;
  unsigned int button;
  Bool same_screen;
} XButtonEvent;
typedef XButtonEvent XButtonPressedEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  int x, y, width, height;
  int count;
} XExposeEvent;

typedef struct {
  int type;
  unsigned long serial;
  Bool send_event;
  Display *display;
  Window window;
  Atom message_type;
  int format;
  union {
    char b[20];
    short s[10];
    long l[5];
  } data;
} XClientMessageEvent;

typedef struct {
  int type;
  Display *display;
  XID resourceid;
  unsigned long serial;
  unsigned char error_code;
  unsigned char request_code;
  unsigned char minor_code;
} XErrorEvent;

typedef union _XEvent {
  int type;
  XAnyEvent xany;
  XKeyEvent xkey;
  XButtonEvent xbutton;
  XExposeEvent xexpose;
  XClientMessageEvent xclient;
  XErrorEvent xerror;
  long pad[24];
} XEvent;

typedef int (*XErrorHandler)(Display *, XErrorEvent *);

/* ---- display / misc ---- */
Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *);
int XSynchronize(Display *, Bool);
int XSync(Display *, Bool);
int XFlush(Display *);
XErrorHandler XSetErrorHandler(XErrorHandler);
int XGetErrorText(Display *, int code, char *buf, int len);
Atom XInternAtom(Display *, const char *name, Bool only_if_exists);
char *XGetDefault(Display *, const char *program, const char *option);
int XFree(void *);

/* ---- windows ---- */
Window XCreateSimpleWindow(Display *, Window parent, int x, int y,
                           unsigned int w, unsigned int h,
                           unsigned int border_width,
                           unsigned long border, unsigned long background);
int XDestroyWindow(Display *, Window);
int XMapWindow(Display *, Window);
int XUnmapWindow(Display *, Window);
int XClearWindow(Display *, Window);
int XSelectInput(Display *, Window, long event_mask);
int XChangeProperty(Display *, Window, Atom property, Atom type, int format,
                    int mode, const unsigned char *data, int nelements);
int XDefineCursor(Display *, Window, Cursor);
Cursor XCreatePixmapCursor(Display *, Pixmap source, Pixmap mask,
                           XColor *fg, XColor *bg,
                           unsigned int x, unsigned int y);

/* ---- events ---- */
int XEventsQueued(Display *, int mode);
int XPending(Display *);
int XNextEvent(Display *, XEvent *);

/* ---- colours ---- */
Status XAllocNamedColor(Display *, Colormap, const char *name,
                        XColor *screen_def, XColor *exact_def);
int XQueryColors(Display *, Colormap, XColor *defs, int ncolors);

/* ---- GC ---- */
GC XCreateGC(Display *, Drawable, unsigned long valuemask, XGCValues *);
int XFreeGC(Display *, GC);
Status XGetGCValues(Display *, GC, unsigned long valuemask, XGCValues *);
int XSetForeground(Display *, GC, unsigned long);
int XSetBackground(Display *, GC, unsigned long);
int XSetFillStyle(Display *, GC, int);
int XSetTile(Display *, GC, Pixmap);
int XSetStipple(Display *, GC, Pixmap);
int XSetTSOrigin(Display *, GC, int, int);
int XSetClipMask(Display *, GC, Pixmap);
int XSetClipOrigin(Display *, GC, int, int);
int XSetFont(Display *, GC, Font);
int XSetFunction(Display *, GC, int);

/* ---- drawing ---- */
int XCopyArea(Display *, Drawable src, Drawable dest, GC,
              int src_x, int src_y, unsigned int w, unsigned int h,
              int dest_x, int dest_y);
int XFillRectangle(Display *, Drawable, GC, int x, int y,
                   unsigned int w, unsigned int h);
int XDrawRectangle(Display *, Drawable, GC, int x, int y,
                   unsigned int w, unsigned int h);
int XDrawLine(Display *, Drawable, GC, int x1, int y1, int x2, int y2);
int XDrawPoints(Display *, Drawable, GC, XPoint *, int n, int mode);
int XFillArc(Display *, Drawable, GC, int x, int y,
             unsigned int w, unsigned int h, int angle1, int angle2);
int XDrawString(Display *, Drawable, GC, int x, int y,
                const char *s, int len);

/* ---- fonts ---- */
XFontStruct *XLoadQueryFont(Display *, const char *name);
int XFreeFont(Display *, XFontStruct *);
int XTextWidth(XFontStruct *, const char *, int);

/* ---- pixmaps / images ---- */
Pixmap XCreatePixmap(Display *, Drawable, unsigned int w, unsigned int h,
                     unsigned int depth);
int XFreePixmap(Display *, Pixmap);
Pixmap XCreateBitmapFromData(Display *, Drawable, const char *data,
                             unsigned int w, unsigned int h);
XImage *XCreateImage(Display *, Visual *, unsigned int depth, int format,
                     int offset, char *data, unsigned int w, unsigned int h,
                     int bitmap_pad, int bytes_per_line);
int XDestroyImage(XImage *);
unsigned long XGetPixel(XImage *, int x, int y);
int XPutPixel(XImage *, int x, int y, unsigned long pixel);
XImage *XGetImage(Display *, Drawable, int x, int y,
                  unsigned int w, unsigned int h,
                  unsigned long plane_mask, int format);
int XPutImage(Display *, Drawable, GC, XImage *, int src_x, int src_y,
              int dest_x, int dest_y, unsigned int w, unsigned int h);

/* ---- keyboard ---- */
KeyCode XKeysymToKeycode(Display *, KeySym);
KeySym XKeycodeToKeysym(Display *, unsigned int keycode, int index);
KeySym XStringToKeysym(const char *);
char *XKeysymToString(KeySym);

#ifdef __cplusplus
}
#endif

#endif
