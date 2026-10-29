/* Xutil.h (cyberspace7 XEvil port): WM hint types (accepted, ignored) and
 * the keyboard helpers XEvil calls. See Xlib.h for the overall model. */
#ifndef _X11_XUTIL_H_
#define _X11_XUTIL_H_

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USPosition (1L << 0)
#define USSize (1L << 1)
#define PPosition (1L << 2)
#define PSize (1L << 3)
#define PMinSize (1L << 4)
#define PMaxSize (1L << 5)

#define InputHint (1L << 0)
#define StateHint (1L << 1)
#define IconPixmapHint (1L << 2)
#define IconWindowHint (1L << 3)
#define IconPositionHint (1L << 4)
#define IconMaskHint (1L << 5)

#define WithdrawnState 0
#define NormalState 1
#define IconicState 3

typedef struct {
  long flags;
  int x, y, width, height;
  int min_width, min_height;
  int max_width, max_height;
  int width_inc, height_inc;
  struct { int x, y; } min_aspect, max_aspect;
  int base_width, base_height;
  int win_gravity;
} XSizeHints;

typedef struct {
  long flags;
  Bool input;
  int initial_state;
  Pixmap icon_pixmap;
  Window icon_window;
  int icon_x, icon_y;
  Pixmap icon_mask;
  XID window_group;
} XWMHints;

typedef struct {
  char *res_name;
  char *res_class;
} XClassHint;

typedef struct {
  unsigned char *value;
  Atom encoding;
  int format;
  unsigned long nitems;
} XTextProperty;

typedef struct _XComposeStatus {
  char *compose_ptr;
  int chars_matched;
} XComposeStatus;

Status XStringListToTextProperty(char **list, int count, XTextProperty *);
void XSetWMProperties(Display *, Window, XTextProperty *window_name,
                      XTextProperty *icon_name, char **argv, int argc,
                      XSizeHints *, XWMHints *, XClassHint *);
int XLookupString(XKeyEvent *, char *buffer, int nbytes, KeySym *,
                  XComposeStatus *);

#ifdef __cplusplus
}
#endif

#endif
