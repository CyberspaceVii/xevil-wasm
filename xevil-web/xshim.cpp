/*
 * xshim.cpp (cyberspace7 XEvil port): the in-process "X server" behind
 * the xinc/X11 headers. XEvil's x11/ backend is compiled unmodified against
 * those headers; everything it asks of Xlib lands here.
 *
 * Model
 *   - Every drawable (window or pixmap) owns a plain 32-bit buffer.
 *     depth 24 → 0x00RRGGBB, depth 1 → 0/1 per pixel. All rasterising is
 *     ours (no SDL drawing), so semantics match X exactly where XEvil
 *     depends on them: GC clip masks (transparent sprites), FillTiled
 *     (scrolling backgrounds), FillStippled (insensitive panels).
 *   - Windows keep their own backing buffer; children are composited over
 *     parents at present time, which gives the same picture as X's
 *     ClipByChildren drawing without per-op clip lists.
 *   - Top-level windows are centred on one SDL canvas sized to the largest
 *     mapped top-level (the game viewport); dialogs stack over it in map
 *     order. Input is routed like X with a pointer-root focus policy.
 *   - xshim_present() is called by the frame driver (main loop) once per
 *     game turn, and by XNextEvent while it blocks.
 *
 * Fonts: X.org misc-misc 6x13 / 9x15B, public domain, baked by
 * bake-fonts.py into xfonts.h.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/xpm.h>

#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "xfonts.h"
#include "xshim.h"

namespace {

enum Kind { K_PIXMAP, K_WINDOW };

struct Draw {
  Kind kind;
  int w, h, depth;
  std::vector<uint32_t> px;
  // window-only
  Window parent;
  int x, y, bw;
  bool mapped;
  uint32_t bg, border;
  long mask;
  std::vector<Window> kids;
};

struct Gc {
  uint32_t fg, bg;
  int fill;
  Pixmap tile, stipple;
  int tsx, tsy;
  Pixmap clip;
  int cx, cy;
  Font font;
};

struct TopPos {
  Window w;
  int sx, sy;
};

const Window ROOT = 1;
const Font FONT_SMALL = 1, FONT_BIG = 2;
const int ROOT_W = 1600, ROOT_H = 1200;

Display g_dpy;
bool g_open = false;
std::vector<Draw *> g_obj;            // XID → object; 0 unused, 1 = root
std::vector<Window> g_stack;          // mapped top-levels, bottom → top
std::vector<TopPos> g_tops;           // screen placement from last present
std::deque<XEvent> g_q;
unsigned long g_serial = 0;
XErrorHandler g_errh = nullptr;
int g_mouse_x = -1, g_mouse_y = -1;   // canvas coords

SDL_Window *g_win = nullptr;
SDL_Renderer *g_ren = nullptr;
SDL_Texture *g_tex = nullptr;
int g_cw = 0, g_ch = 0;
std::vector<uint32_t> g_frame;

XFontStruct g_font_small, g_font_big;

Draw *get(XID id) {
  return (id > 0 && id < g_obj.size()) ? g_obj[id] : nullptr;
}

XID add(Draw *d) {
  g_obj.push_back(d);
  return g_obj.size() - 1;
}

bool is_top(const Draw *d) { return d && d->kind == K_WINDOW && d->parent == ROOT; }

// --------------------------------------------------------------- colours
struct NamedColor {
  const char *name;
  uint32_t rgb;
};
const NamedColor NAMED[] = {
    {"black", 0x000000},     {"white", 0xffffff},     {"red", 0xff0000},
    {"red2", 0xee0000},      {"green", 0x00ff00},     {"green1", 0x00ff00},
    {"green4", 0x008b00},    {"blue", 0x0000ff},      {"brown", 0xa52a2a},
    {"purple", 0xa020f0},    {"pink3", 0xcd919e},     {"steelblue", 0x4682b4},
    {"yellow", 0xffff00},    {"lightgrey", 0xd3d3d3}, {"lightgray", 0xd3d3d3},
    {"darkgrey", 0xa9a9a9},  {"darkgray", 0xa9a9a9},  {"orange", 0xffa500},
    {"cyan", 0x00ffff},      {"magenta", 0xff00ff},   {"gold", 0xffd700},
    {"navy", 0x000080},      {"gray", 0xbebebe},      {"grey", 0xbebebe},
};

std::string squash(const char *s) {
  std::string o;
  for (; *s; s++)
    if (!isspace((unsigned char)*s)) o += (char)tolower((unsigned char)*s);
  return o;
}

int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)tolower((unsigned char)c);
  return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

// "#rgb" / "#rrggbb" / "#rrrrggggbbbb" / named / grayNN. False if unknown.
bool parse_color(const char *spec, uint32_t *out) {
  if (spec[0] == '#') {
    size_t n = strlen(spec + 1);
    if (n % 3 || n == 0 || n > 12) return false;
    size_t k = n / 3;
    uint32_t c[3];
    for (int i = 0; i < 3; i++) {
      uint32_t v = 0;
      for (size_t j = 0; j < k; j++) {
        int h = hexv(spec[1 + i * k + j]);
        if (h < 0) return false;
        v = v << 4 | h;
      }
      // scale k hex digits to 8 bits
      c[i] = k == 1 ? v * 17 : k == 2 ? v : v >> (4 * (k - 2));
    }
    *out = c[0] << 16 | c[1] << 8 | c[2];
    return true;
  }
  std::string n = squash(spec);
  for (const auto &e : NAMED)
    if (n == e.name) {
      *out = e.rgb;
      return true;
    }
  if ((n.rfind("gray", 0) == 0 || n.rfind("grey", 0) == 0) && n.size() > 4 &&
      isdigit((unsigned char)n[4])) {
    int pct = atoi(n.c_str() + 4);
    if (pct >= 0 && pct <= 100) {
      uint32_t v = (uint32_t)lround(pct * 255 / 100.0);
      *out = v << 16 | v << 8 | v;
      return true;
    }
  }
  return false;
}

// ----------------------------------------------------------------- fonts
void init_font(XFontStruct *f, Font fid, int w, int asc, int desc) {
  memset(f, 0, sizeof *f);
  f->fid = fid;
  f->min_char_or_byte2 = 32;
  f->max_char_or_byte2 = 126;
  f->all_chars_exist = True;
  f->default_char = ' ';
  f->min_bounds.width = f->max_bounds.width = (short)w;
  f->min_bounds.rbearing = f->max_bounds.rbearing = (short)w;
  f->min_bounds.ascent = f->max_bounds.ascent = (short)asc;
  f->min_bounds.descent = f->max_bounds.descent = (short)desc;
  f->ascent = asc;
  f->descent = desc;
  f->per_char = nullptr;
}

// --------------------------------------------------------- rasterising
// One pixel write through the GC: clip mask, then fill style.
inline void gc_plot(Draw *d, const Gc *g, int x, int y, const Draw *clip,
                    const Draw *tile) {
  if (x < 0 || y < 0 || x >= d->w || y >= d->h) return;
  if (clip) {
    int mx = x - g->cx, my = y - g->cy;
    if (mx < 0 || my < 0 || mx >= clip->w || my >= clip->h) return;
    if (!clip->px[my * clip->w + mx]) return;
  }
  uint32_t c = g->fg;
  if (tile && (g->fill == FillTiled || g->fill == FillStippled ||
               g->fill == FillOpaqueStippled)) {
    int tx = (x - g->tsx) % tile->w, ty = (y - g->tsy) % tile->h;
    if (tx < 0) tx += tile->w;
    if (ty < 0) ty += tile->h;
    uint32_t t = tile->px[ty * tile->w + tx];
    if (g->fill == FillTiled)
      c = t;
    else if (g->fill == FillStippled) {
      if (!t) return;
    } else
      c = t ? g->fg : g->bg;
  }
  d->px[y * d->w + x] = d->depth == 1 ? (c & 1) : (c & 0xffffff);
}

void gc_sources(const Gc *g, const Draw **clip, const Draw **tile) {
  *clip = g->clip ? get(g->clip) : nullptr;
  *tile = nullptr;
  if (g->fill == FillTiled)
    *tile = get(g->tile);
  else if (g->fill == FillStippled || g->fill == FillOpaqueStippled)
    *tile = get(g->stipple);
  if (*tile && (*tile)->px.empty()) *tile = nullptr;
}

void fill_rect(Draw *d, const Gc *g, int x, int y, int w, int h) {
  const Draw *clip, *tile;
  gc_sources(g, &clip, &tile);
  int x0 = std::max(x, 0), y0 = std::max(y, 0);
  int x1 = std::min(x + w, d->w), y1 = std::min(y + h, d->h);
  if (x0 >= x1 || y0 >= y1) return;
  if (!clip && (g->fill == FillSolid || !tile)) {
    uint32_t c = d->depth == 1 ? (g->fg & 1) : (g->fg & 0xffffff);
    for (int yy = y0; yy < y1; yy++)
      std::fill(&d->px[yy * d->w + x0], &d->px[yy * d->w + x1], c);
    return;
  }
  for (int yy = y0; yy < y1; yy++)
    for (int xx = x0; xx < x1; xx++) gc_plot(d, g, xx, yy, clip, tile);
}

void plot1(Draw *d, const Gc *g, int x, int y) {
  const Draw *clip, *tile;
  gc_sources(g, &clip, &tile);
  gc_plot(d, g, x, y, clip, tile);
}

void line(Draw *d, const Gc *g, int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    plot1(d, g, x0, y0);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) err += dy, x0 += sx;
    if (e2 <= dx) err += dx, y0 += sy;
  }
}

// ------------------------------------------------------------- windows
void collect_viewable(Window w, std::vector<Window> &out) {
  Draw *d = get(w);
  if (!d || !d->mapped) return;
  out.push_back(w);
  for (Window k : d->kids) collect_viewable(k, out);
}

bool viewable(Window w) {
  for (Draw *d = get(w); d; d = get(d->parent)) {
    if (w == ROOT || d->parent == 0) return true;
    if (!d->mapped) return false;
    if (d->parent == ROOT) return true;
  }
  return false;
}

void queue_expose(Window w) {
  Draw *d = get(w);
  if (!d || !(d->mask & ExposureMask)) return;
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xexpose.type = Expose;
  e.xexpose.serial = ++g_serial;
  e.xexpose.display = &g_dpy;
  e.xexpose.window = w;
  e.xexpose.width = d->w;
  e.xexpose.height = d->h;
  e.xexpose.count = 0;
  g_q.push_back(e);
}

void clear_window(Draw *d) { std::fill(d->px.begin(), d->px.end(), d->bg); }

void blit_window(Window w, int ox, int oy, int clx0, int cly0, int clx1,
                 int cly1) {
  Draw *d = get(w);
  if (!d || !d->mapped) return;
  // border ring sits outside the window's own area, inside parent clip
  int ax = ox + d->x, ay = oy + d->y;
  if (d->bw > 0) {
    for (int yy = ay; yy < ay + d->h + 2 * d->bw; yy++)
      for (int xx = ax; xx < ax + d->w + 2 * d->bw; xx++) {
        if (xx < clx0 || yy < cly0 || xx >= clx1 || yy >= cly1) continue;
        bool inner = xx >= ax + d->bw && xx < ax + d->bw + d->w &&
                     yy >= ay + d->bw && yy < ay + d->bw + d->h;
        if (!inner) g_frame[yy * g_cw + xx] = d->border;
      }
  }
  ax += d->bw;
  ay += d->bw;
  int x0 = std::max(ax, clx0), y0 = std::max(ay, cly0);
  int x1 = std::min(ax + d->w, clx1), y1 = std::min(ay + d->h, cly1);
  for (int yy = y0; yy < y1; yy++)
    memcpy(&g_frame[yy * g_cw + x0], &d->px[(yy - ay) * d->w + (x0 - ax)],
           (x1 - x0) * 4);
  for (Window k : d->kids) blit_window(k, ax, ay, x0, y0, x1, y1);
}

// Deepest viewable window at canvas point; returns window + local coords.
Window pick(int cx, int cy, int *lx, int *ly) {
  for (auto it = g_tops.rbegin(); it != g_tops.rend(); ++it) {
    Draw *t = get(it->w);
    if (!t || !t->mapped) continue;
    int x = cx - it->sx, y = cy - it->sy;
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) continue;
    Window cur = it->w;
    bool descended = true;
    while (descended) {
      descended = false;
      Draw *c = get(cur);
      for (auto k = c->kids.rbegin(); k != c->kids.rend(); ++k) {
        Draw *kd = get(*k);
        if (!kd || !kd->mapped) continue;
        int kx = x - kd->x - kd->bw, ky = y - kd->y - kd->bw;
        if (kx >= 0 && ky >= 0 && kx < kd->w && ky < kd->h) {
          cur = *k;
          x = kx;
          y = ky;
          descended = true;
          break;
        }
      }
    }
    *lx = x;
    *ly = y;
    return cur;
  }
  return None;
}

// Walk up from w until a window selects `mask`; adjusts coords to it.
Window propagate(Window w, long mask, int *lx, int *ly) {
  while (w && w != ROOT) {
    Draw *d = get(w);
    if (!d) return None;
    if (d->mask & mask) return w;
    *lx += d->x + d->bw;
    *ly += d->y + d->bw;
    w = d->parent;
  }
  return None;
}

// ------------------------------------------------------------ keyboard
struct KeyEnt {
  SDL_Keycode sdl;
  KeySym ks0, ks1;
  const char *name;
};
// keycode = index + 8 (X keycodes start at 8)
const KeyEnt KEYS[] = {
    {SDLK_a, XK_a, XK_A, "a"},           {SDLK_b, XK_b, XK_B, "b"},
    {SDLK_c, XK_c, XK_C, "c"},           {SDLK_d, XK_d, XK_D, "d"},
    {SDLK_e, XK_e, XK_E, "e"},           {SDLK_f, XK_f, XK_F, "f"},
    {SDLK_g, XK_g, XK_G, "g"},           {SDLK_h, XK_h, XK_H, "h"},
    {SDLK_i, XK_i, XK_I, "i"},           {SDLK_j, XK_j, XK_J, "j"},
    {SDLK_k, XK_k, XK_K, "k"},           {SDLK_l, XK_l, XK_L, "l"},
    {SDLK_m, XK_m, XK_M, "m"},           {SDLK_n, XK_n, XK_N, "n"},
    {SDLK_o, XK_o, XK_O, "o"},           {SDLK_p, XK_p, XK_P, "p"},
    {SDLK_q, XK_q, XK_Q, "q"},           {SDLK_r, XK_r, XK_R, "r"},
    {SDLK_s, XK_s, XK_S, "s"},           {SDLK_t, XK_t, XK_T, "t"},
    {SDLK_u, XK_u, XK_U, "u"},           {SDLK_v, XK_v, XK_V, "v"},
    {SDLK_w, XK_w, XK_W, "w"},           {SDLK_x, XK_x, XK_X, "x"},
    {SDLK_y, XK_y, XK_Y, "y"},           {SDLK_z, XK_z, XK_Z, "z"},
    {SDLK_0, XK_0, XK_parenright, "0"},  {SDLK_1, XK_1, XK_exclam, "1"},
    {SDLK_2, XK_2, XK_at, "2"},          {SDLK_3, XK_3, XK_numbersign, "3"},
    {SDLK_4, XK_4, XK_dollar, "4"},      {SDLK_5, XK_5, XK_percent, "5"},
    {SDLK_6, XK_6, XK_asciicircum, "6"}, {SDLK_7, XK_7, XK_ampersand, "7"},
    {SDLK_8, XK_8, XK_asterisk, "8"},    {SDLK_9, XK_9, XK_parenleft, "9"},
    {SDLK_SPACE, XK_space, XK_space, "space"},
    {SDLK_MINUS, XK_minus, XK_underscore, "minus"},
    {SDLK_EQUALS, XK_equal, XK_plus, "equal"},
    {SDLK_LEFTBRACKET, XK_bracketleft, XK_braceleft, "bracketleft"},
    {SDLK_RIGHTBRACKET, XK_bracketright, XK_braceright, "bracketright"},
    {SDLK_BACKSLASH, XK_backslash, XK_bar, "backslash"},
    {SDLK_SEMICOLON, XK_semicolon, XK_colon, "semicolon"},
    {SDLK_QUOTE, XK_apostrophe, XK_quotedbl, "apostrophe"},
    {SDLK_BACKQUOTE, XK_grave, XK_asciitilde, "grave"},
    {SDLK_COMMA, XK_comma, XK_less, "comma"},
    {SDLK_PERIOD, XK_period, XK_greater, "period"},
    {SDLK_SLASH, XK_slash, XK_question, "slash"},
    {SDLK_RETURN, XK_Return, XK_Return, "Return"},
    {SDLK_ESCAPE, XK_Escape, XK_Escape, "Escape"},
    {SDLK_TAB, XK_Tab, XK_Tab, "Tab"},
    {SDLK_BACKSPACE, XK_BackSpace, XK_BackSpace, "BackSpace"},
    {SDLK_DELETE, XK_Delete, XK_Delete, "Delete"},
    {SDLK_INSERT, XK_Insert, XK_Insert, "Insert"},
    {SDLK_HOME, XK_Home, XK_Home, "Home"},
    {SDLK_END, XK_End, XK_End, "End"},
    {SDLK_PAGEUP, XK_Prior, XK_Prior, "Prior"},
    {SDLK_PAGEDOWN, XK_Next, XK_Next, "Next"},
    {SDLK_LEFT, XK_Left, XK_Left, "Left"},
    {SDLK_RIGHT, XK_Right, XK_Right, "Right"},
    {SDLK_UP, XK_Up, XK_Up, "Up"},
    {SDLK_DOWN, XK_Down, XK_Down, "Down"},
    {SDLK_LSHIFT, XK_Shift_L, XK_Shift_L, "Shift_L"},
    {SDLK_RSHIFT, XK_Shift_R, XK_Shift_R, "Shift_R"},
    {SDLK_LCTRL, XK_Control_L, XK_Control_L, "Control_L"},
    {SDLK_RCTRL, XK_Control_R, XK_Control_R, "Control_R"},
    {SDLK_LALT, XK_Alt_L, XK_Meta_L, "Alt_L"},
    {SDLK_RALT, XK_Alt_R, XK_Meta_R, "Alt_R"},
    {SDLK_CAPSLOCK, XK_Caps_Lock, XK_Caps_Lock, "Caps_Lock"},
    {SDLK_F1, XK_F1, XK_F1, "F1"},       {SDLK_F2, XK_F2, XK_F2, "F2"},
    {SDLK_F3, XK_F3, XK_F3, "F3"},       {SDLK_F4, XK_F4, XK_F4, "F4"},
    {SDLK_F5, XK_F5, XK_F5, "F5"},       {SDLK_F6, XK_F6, XK_F6, "F6"},
    {SDLK_F7, XK_F7, XK_F7, "F7"},       {SDLK_F8, XK_F8, XK_F8, "F8"},
    {SDLK_F9, XK_F9, XK_F9, "F9"},       {SDLK_F10, XK_F10, XK_F10, "F10"},
    {SDLK_F11, XK_F11, XK_F11, "F11"},   {SDLK_F12, XK_F12, XK_F12, "F12"},
    {SDLK_KP_0, XK_KP_0, XK_KP_Insert, "KP_0"},
    {SDLK_KP_1, XK_KP_1, XK_KP_End, "KP_1"},
    {SDLK_KP_2, XK_KP_2, XK_KP_Down, "KP_2"},
    {SDLK_KP_3, XK_KP_3, XK_KP_Next, "KP_3"},
    {SDLK_KP_4, XK_KP_4, XK_KP_Left, "KP_4"},
    {SDLK_KP_5, XK_KP_5, XK_KP_Begin, "KP_5"},
    {SDLK_KP_6, XK_KP_6, XK_KP_Right, "KP_6"},
    {SDLK_KP_7, XK_KP_7, XK_KP_Home, "KP_7"},
    {SDLK_KP_8, XK_KP_8, XK_KP_Up, "KP_8"},
    {SDLK_KP_9, XK_KP_9, XK_KP_Prior, "KP_9"},
    {SDLK_KP_PERIOD, XK_KP_Decimal, XK_KP_Delete, "KP_Decimal"},
    {SDLK_KP_ENTER, XK_KP_Enter, XK_KP_Enter, "KP_Enter"},
    {SDLK_KP_PLUS, XK_KP_Add, XK_KP_Add, "KP_Add"},
    {SDLK_KP_MINUS, XK_KP_Subtract, XK_KP_Subtract, "KP_Subtract"},
    {SDLK_KP_MULTIPLY, XK_KP_Multiply, XK_KP_Multiply, "KP_Multiply"},
    {SDLK_KP_DIVIDE, XK_KP_Divide, XK_KP_Divide, "KP_Divide"},
};
const int NKEYS = sizeof KEYS / sizeof KEYS[0];

int sdl_to_keycode(SDL_Keycode k) {
  for (int i = 0; i < NKEYS; i++)
    if (KEYS[i].sdl == k) return i + 8;
  return 0;
}

const KeyEnt *keyent(unsigned kc) {
  return (kc >= 8 && kc < (unsigned)NKEYS + 8) ? &KEYS[kc - 8] : nullptr;
}

unsigned int mod_state() {
  SDL_Keymod m = SDL_GetModState();
  unsigned s = 0;
  if (m & KMOD_SHIFT) s |= ShiftMask;
  if (m & KMOD_CTRL) s |= ControlMask;
  if (m & KMOD_ALT) s |= Mod1Mask;
  if (m & KMOD_CAPS) s |= LockMask;
  return s;
}

// ---------------------------------------------------------- event pump
void push_key(int type, SDL_Keycode sym) {
  int kc = sdl_to_keycode(sym);
  if (!kc || g_stack.empty()) return;
  long need = type == KeyPress ? KeyPressMask : KeyReleaseMask;
  int lx = 0, ly = 0;
  Window target = None;
  // pointer-root: deepest window under the pointer inside the focus top-level
  Window focus = g_stack.back();
  if (g_mouse_x >= 0) {
    Window w = pick(g_mouse_x, g_mouse_y, &lx, &ly);
    Window t = w;
    while (t && get(t) && get(t)->parent != ROOT) t = get(t)->parent;
    if (t == focus) target = propagate(w, need, &lx, &ly);
  }
  if (!target) {
    lx = ly = 0;
    target = propagate(focus, need, &lx, &ly);
  }
  if (!target) return;
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xkey.type = type;
  e.xkey.serial = ++g_serial;
  e.xkey.display = &g_dpy;
  e.xkey.window = target;
  e.xkey.root = ROOT;
  e.xkey.time = SDL_GetTicks();
  e.xkey.x = lx;
  e.xkey.y = ly;
  e.xkey.state = mod_state();
  e.xkey.keycode = kc;
  e.xkey.same_screen = True;
  g_q.push_back(e);
}

void push_button(int sx, int sy, int button) {
  int lx, ly;
  Window w = pick(sx, sy, &lx, &ly);
  if (!w) return;
  Window target = propagate(w, ButtonPressMask, &lx, &ly);
  if (!target) return;
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xbutton.type = ButtonPress;
  e.xbutton.serial = ++g_serial;
  e.xbutton.display = &g_dpy;
  e.xbutton.window = target;
  e.xbutton.root = ROOT;
  e.xbutton.time = SDL_GetTicks();
  e.xbutton.x = lx;
  e.xbutton.y = ly;
  e.xbutton.x_root = sx;
  e.xbutton.y_root = sy;
  e.xbutton.state = mod_state();
  e.xbutton.button = button;
  e.xbutton.same_screen = True;
  g_q.push_back(e);
}

const Atom ATOM_WM_PROTOCOLS = 1, ATOM_WM_DELETE = 2;

void push_delete() {
  // Closing the canvas = WM_DELETE_WINDOW to the bottom (main) top-level.
  if (g_stack.empty()) return;
  XEvent e;
  memset(&e, 0, sizeof e);
  e.xclient.type = ClientMessage;
  e.xclient.serial = ++g_serial;
  e.xclient.display = &g_dpy;
  e.xclient.window = g_stack.front();
  e.xclient.message_type = ATOM_WM_PROTOCOLS;
  e.xclient.format = 32;
  e.xclient.data.l[0] = (long)ATOM_WM_DELETE;
  g_q.push_back(e);
}

void pump() {
  if (!g_open) return;
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
    case SDL_QUIT:
      push_delete();
      break;
    case SDL_KEYDOWN:
      if (!ev.key.repeat) push_key(KeyPress, ev.key.keysym.sym);
      break;
    case SDL_KEYUP:
      push_key(KeyRelease, ev.key.keysym.sym);
      break;
    case SDL_MOUSEMOTION:
      g_mouse_x = ev.motion.x;
      g_mouse_y = ev.motion.y;
      break;
    case SDL_MOUSEBUTTONDOWN:
      g_mouse_x = ev.button.x;
      g_mouse_y = ev.button.y;
      push_button(ev.button.x, ev.button.y, ev.button.button);
      break;
    case SDL_WINDOWEVENT:
      if (ev.window.event == SDL_WINDOWEVENT_LEAVE) g_mouse_x = g_mouse_y = -1;
      if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) xshim_present();
      break;
    }
  }
}

// ------------------------------------------------------------------ XPM
struct XpmParsed {
  int w = 0, h = 0;
  std::vector<uint32_t> rgb;   // per pixel
  std::vector<uint8_t> opaque; // per pixel
  bool has_none = false;
  std::vector<unsigned long> pixels;
};

bool xpm_parse(char **data, XpmParsed &p) {
  int ncolors = 0, cpp = 0;
  if (sscanf(data[0], "%d %d %d %d", &p.w, &p.h, &ncolors, &cpp) != 4 ||
      p.w <= 0 || p.h <= 0 || cpp < 1 || cpp > 4)
    return false;
  struct Ent {
    std::string key;
    uint32_t rgb;
    bool none;
  };
  std::vector<Ent> tab;
  for (int i = 0; i < ncolors; i++) {
    const char *ln = data[1 + i];
    if (strlen(ln) < (size_t)cpp) return false;
    Ent e{std::string(ln, cpp), 0, false};
    // key/value pairs follow; we want the 'c' (colour visual) value, which
    // may be multi-word ("light grey") and runs until the next key token.
    std::vector<std::string> tok;
    const char *s = ln + cpp;
    while (*s) {
      while (*s && isspace((unsigned char)*s)) s++;
      const char *b = s;
      while (*s && !isspace((unsigned char)*s)) s++;
      if (s > b) tok.emplace_back(b, s - b);
    }
    std::string val;
    for (size_t t = 0; t < tok.size(); t++) {
      if (tok[t] != "c") continue;
      for (size_t u = t + 1; u < tok.size(); u++) {
        const std::string &v = tok[u];
        if (v == "m" || v == "s" || v == "g" || v == "g4") break;
        if (!val.empty()) val += ' ';
        val += v;
      }
      break;
    }
    if (val.empty() && tok.size() >= 2) val = tok.back();
    if (squash(val.c_str()) == "none") {
      e.none = true;
      p.has_none = true;
    } else if (!parse_color(val.c_str(), &e.rgb)) {
      fprintf(stderr, "xshim: unknown XPM colour '%s'\n", val.c_str());
      e.rgb = 0;
    } else {
      p.pixels.push_back(e.rgb);
    }
    tab.push_back(e);
  }
  p.rgb.assign(p.w * p.h, 0);
  p.opaque.assign(p.w * p.h, 1);
  for (int y = 0; y < p.h; y++) {
    const char *row = data[1 + ncolors + y];
    if ((int)strlen(row) < p.w * cpp) return false;
    for (int x = 0; x < p.w; x++) {
      const char *k = row + x * cpp;
      const Ent *hit = nullptr;
      for (const auto &e : tab)
        if (!memcmp(e.key.data(), k, cpp)) {
          hit = &e;
          break;
        }
      if (!hit) continue;
      if (hit->none)
        p.opaque[y * p.w + x] = 0;
      else
        p.rgb[y * p.w + x] = hit->rgb;
    }
  }
  std::sort(p.pixels.begin(), p.pixels.end());
  p.pixels.erase(std::unique(p.pixels.begin(), p.pixels.end()), p.pixels.end());
  return true;
}

void xpm_attrs(XpmAttributes *a, const XpmParsed &p) {
  if (!a) return;
  a->width = p.w;
  a->height = p.h;
  a->npixels = p.pixels.size();
  a->pixels = (Pixel *)malloc(sizeof(Pixel) * std::max<size_t>(1, p.pixels.size()));
  std::copy(p.pixels.begin(), p.pixels.end(), a->pixels);
}

XImage *make_image(int depth, int w, int h, char *data) {
  XImage *im = (XImage *)calloc(1, sizeof(XImage));
  im->width = w;
  im->height = h;
  im->format = ZPixmap;
  im->data = data;
  im->bitmap_unit = 32;
  im->bitmap_pad = 32;
  im->depth = depth;
  im->bytes_per_line = w * 4;
  im->bits_per_pixel = 32;
  return im;
}

} // namespace

// =================================================================== API
extern "C" {

void xshim_present(void) {
  if (!g_open) return;
  // canvas = largest mapped top-level
  int cw = 0, ch = 0;
  for (Window t : g_stack) {
    Draw *d = get(t);
    cw = std::max(cw, d->w);
    ch = std::max(ch, d->h);
  }
  if (!cw || !ch) return;
  if (!g_win) {
#ifdef __EMSCRIPTEN__
    // NULL title: in a browser SDL's window title IS document.title; the host page owns
    // the tab title and favicon, the game never touches them (see also nohijack.js).
    const char *title = nullptr;
#else
    const char *title = "XEvil";
#endif
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, cw, ch, 0);
    if (!g_win) {
      fprintf(stderr, "xshim: SDL_CreateWindow: %s\n", SDL_GetError());
      return;
    }
    g_ren = SDL_CreateRenderer(g_win, -1, 0);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
  }
  if (cw != g_cw || ch != g_ch) {
    g_cw = cw;
    g_ch = ch;
    SDL_SetWindowSize(g_win, cw, ch);
    SDL_RenderSetLogicalSize(g_ren, cw, ch);
    if (g_tex) SDL_DestroyTexture(g_tex);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB888,
                              SDL_TEXTUREACCESS_STREAMING, cw, ch);
    g_frame.assign((size_t)cw * ch, 0);
  }
  std::fill(g_frame.begin(), g_frame.end(), 0);
  g_tops.clear();
  for (Window t : g_stack) {
    Draw *d = get(t);
    int sx = (cw - d->w) / 2, sy = (ch - d->h) / 2;
    g_tops.push_back({t, sx, sy});
    // top-level position is the WM's business: place at (sx,sy) regardless
    int ox = sx - d->x - d->bw, oy = sy - d->y - d->bw;
    blit_window(t, ox, oy, 0, 0, cw, ch);
  }
  // Headless verification: XSHIM_DUMP=out.ppm XSHIM_DUMP_FRAME=N writes the
  // Nth presented frame (works under SDL_VIDEODRIVER=dummy).
  static int frames = 0;
  static const char *dump = getenv("XSHIM_DUMP");
  static int dumpAt = getenv("XSHIM_DUMP_FRAME") ? atoi(getenv("XSHIM_DUMP_FRAME")) : 100;
  if (dump && ++frames == dumpAt) {
    if (FILE *f = fopen(dump, "wb")) {
      fprintf(f, "P6\n%d %d\n255\n", cw, ch);
      for (uint32_t p : g_frame) {
        unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8),
                                (unsigned char)p};
        fwrite(rgb, 1, 3, f);
      }
      fclose(f);
      fprintf(stderr, "xshim: dumped frame %d (%dx%d) to %s\n", frames, cw, ch, dump);
    }
  }
  SDL_UpdateTexture(g_tex, nullptr, g_frame.data(), cw * 4);
  SDL_RenderClear(g_ren);
  SDL_RenderCopy(g_ren, g_tex, nullptr, nullptr);
  SDL_RenderPresent(g_ren);
}

void xshim_pump(void) { pump(); }

void xshim_quit(void) { push_delete(); }

// ------------------------------------------------------------- display
Display *XOpenDisplay(const char *) {
  if (g_open) return &g_dpy;
#ifdef __EMSCRIPTEN__
  // Listen for keys on the game canvas only, never on window/document,
  // which would swallow the host page's terminal input.
  SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "xshim: SDL_Init: %s\n", SDL_GetError());
    return nullptr;
  }
  g_obj.assign(2, nullptr);
  Draw *root = new Draw();
  root->kind = K_WINDOW;
  root->w = ROOT_W;
  root->h = ROOT_H;
  root->depth = 24;
  root->mapped = true;
  g_obj[ROOT] = root;
  memset(&g_dpy, 0, sizeof g_dpy);
  g_dpy.visual.c_class = TrueColor;
  g_dpy.visual.bits_per_rgb = 8;
  g_dpy.visual.map_entries = 256;
  g_dpy.screen.display = &g_dpy;
  g_dpy.screen.root = ROOT;
  g_dpy.screen.width = ROOT_W;
  g_dpy.screen.height = ROOT_H;
  g_dpy.screen.root_depth = 24;
  g_dpy.screen.root_visual = &g_dpy.visual;
  g_dpy.screen.cmap = 1;
  g_dpy.screen.white_pixel = 0xffffff;
  g_dpy.screen.black_pixel = 0x000000;
  init_font(&g_font_small, FONT_SMALL, XF_SMALL_W, XF_SMALL_ASC, XF_SMALL_DESC);
  init_font(&g_font_big, FONT_BIG, XF_BIG_W, XF_BIG_ASC, XF_BIG_DESC);
  g_dpy.default_gc = XCreateGC(&g_dpy, ROOT, 0, nullptr);
  g_open = true;
  return &g_dpy;
}

int XCloseDisplay(Display *) { return 0; }
int XSynchronize(Display *, Bool) { return 0; }
int XSync(Display *, Bool) { return 0; }
int XFlush(Display *) { return 0; }
int XFree(void *p) {
  free(p);
  return 0;
}

XErrorHandler XSetErrorHandler(XErrorHandler h) {
  XErrorHandler old = g_errh;
  g_errh = h;
  return old;
}

int XGetErrorText(Display *, int code, char *buf, int len) {
  snprintf(buf, len, "xshim error %d", code);
  return 0;
}

Atom XInternAtom(Display *, const char *name, Bool) {
  if (!strcmp(name, "WM_PROTOCOLS")) return ATOM_WM_PROTOCOLS;
  if (!strcmp(name, "WM_DELETE_WINDOW")) return ATOM_WM_DELETE;
  static std::vector<std::string> extra;
  for (size_t i = 0; i < extra.size(); i++)
    if (extra[i] == name) return 100 + i;
  extra.push_back(name);
  return 100 + extra.size() - 1;
}

char *XGetDefault(Display *, const char *, const char *) { return nullptr; }

// ------------------------------------------------------------- windows
Window XCreateSimpleWindow(Display *, Window parent, int x, int y,
                           unsigned int w, unsigned int h, unsigned int bw,
                           unsigned long border, unsigned long background) {
  Draw *p = get(parent);
  if (!p || p->kind != K_WINDOW) return None;
  Draw *d = new Draw();
  d->kind = K_WINDOW;
  d->w = std::max(1u, w);
  d->h = std::max(1u, h);
  d->depth = 24;
  d->px.assign((size_t)d->w * d->h, background & 0xffffff);
  d->parent = parent;
  d->x = x;
  d->y = y;
  d->bw = bw;
  d->mapped = false;
  d->bg = background & 0xffffff;
  d->border = border & 0xffffff;
  d->mask = 0;
  Window id = add(d);
  p->kids.push_back(id);
  return id;
}

int XDestroyWindow(Display *dpy, Window w) {
  Draw *d = get(w);
  if (!d || d->kind != K_WINDOW || w == ROOT) return 0;
  std::vector<Window> kids = d->kids;
  for (Window k : kids) XDestroyWindow(dpy, k);
  if (Draw *p = get(d->parent)) {
    auto &v = p->kids;
    v.erase(std::remove(v.begin(), v.end(), w), v.end());
  }
  g_stack.erase(std::remove(g_stack.begin(), g_stack.end(), w), g_stack.end());
  g_q.erase(std::remove_if(g_q.begin(), g_q.end(),
                           [w](const XEvent &e) { return e.xany.window == w; }),
            g_q.end());
  delete d;
  g_obj[w] = nullptr;
  return 0;
}

int XMapWindow(Display *, Window w) {
  Draw *d = get(w);
  if (!d || d->kind != K_WINDOW || d->mapped) return 0;
  d->mapped = true;
  if (is_top(d)) {
    g_stack.erase(std::remove(g_stack.begin(), g_stack.end(), w), g_stack.end());
    g_stack.push_back(w);
  }
  if (viewable(w)) {
    std::vector<Window> v;
    collect_viewable(w, v);
    for (Window x : v) {
      clear_window(get(x));
      queue_expose(x);
    }
  }
  return 0;
}

int XUnmapWindow(Display *, Window w) {
  Draw *d = get(w);
  if (!d || d->kind != K_WINDOW || !d->mapped) return 0;
  d->mapped = false;
  if (is_top(d))
    g_stack.erase(std::remove(g_stack.begin(), g_stack.end(), w), g_stack.end());
  return 0;
}

int XClearWindow(Display *, Window w) {
  Draw *d = get(w);
  if (d && d->kind == K_WINDOW) clear_window(d);
  return 0;
}

int XSelectInput(Display *, Window w, long mask) {
  if (Draw *d = get(w)) d->mask = mask;
  return 0;
}

int XChangeProperty(Display *, Window, Atom, Atom, int, int,
                    const unsigned char *, int) {
  return 0;
}

int XDefineCursor(Display *, Window, Cursor) { return 0; }

Cursor XCreatePixmapCursor(Display *, Pixmap, Pixmap, XColor *, XColor *,
                           unsigned int, unsigned int) {
  return 1;
}

Status XStringListToTextProperty(char **list, int, XTextProperty *tp) {
  tp->value = (unsigned char *)(list && list[0] ? list[0] : (char *)"");
  tp->encoding = 31; // XA_STRING
  tp->format = 8;
  tp->nitems = strlen((char *)tp->value);
  return 1;
}

void XSetWMProperties(Display *, Window, XTextProperty *, XTextProperty *,
                      char **, int, XSizeHints *, XWMHints *, XClassHint *) {
#ifndef __EMSCRIPTEN__ // in a browser this would rewrite the host page's document.title
  if (name && name->value && g_win) SDL_SetWindowTitle(g_win, (char *)name->value);
#endif
}

// -------------------------------------------------------------- events
int XEventsQueued(Display *, int mode) {
  if (mode != QueuedAlready) pump();
  return (int)g_q.size();
}

int XPending(Display *d) { return XEventsQueued(d, QueuedAfterFlush); }

int XNextEvent(Display *, XEvent *ev) {
  pump();
  while (g_q.empty()) {
    // Blocking read: keep the picture live while we wait.
    xshim_present();
    SDL_WaitEventTimeout(nullptr, 16);
    pump();
  }
  *ev = g_q.front();
  g_q.pop_front();
  return 0;
}

// ------------------------------------------------------------- colours
Status XAllocNamedColor(Display *, Colormap, const char *name, XColor *sdef,
                        XColor *edef) {
  uint32_t rgb;
  if (!parse_color(name, &rgb)) return 0;
  XColor c;
  memset(&c, 0, sizeof c);
  c.pixel = rgb;
  c.red = ((rgb >> 16) & 0xff) * 257;
  c.green = ((rgb >> 8) & 0xff) * 257;
  c.blue = (rgb & 0xff) * 257;
  c.flags = DoRed | DoGreen | DoBlue;
  if (sdef) *sdef = c;
  if (edef) *edef = c;
  return 1;
}

int XQueryColors(Display *, Colormap, XColor *defs, int n) {
  for (int i = 0; i < n; i++) {
    unsigned long p = defs[i].pixel;
    defs[i].red = ((p >> 16) & 0xff) * 257;
    defs[i].green = ((p >> 8) & 0xff) * 257;
    defs[i].blue = (p & 0xff) * 257;
    defs[i].flags = DoRed | DoGreen | DoBlue;
  }
  return 0;
}

// ------------------------------------------------------------------ GC
GC XCreateGC(Display *, Drawable, unsigned long vm, XGCValues *v) {
  Gc *g = new Gc();
  memset(g, 0, sizeof *g);
  g->fg = 0;
  g->bg = 1;
  g->fill = FillSolid;
  g->font = FONT_SMALL;
  if (v) {
    if (vm & GCForeground) g->fg = v->foreground;
    if (vm & GCBackground) g->bg = v->background;
    if (vm & GCFillStyle) g->fill = v->fill_style;
    if (vm & GCTile) g->tile = v->tile;
    if (vm & GCStipple) g->stipple = v->stipple;
    if (vm & GCTileStipXOrigin) g->tsx = v->ts_x_origin;
    if (vm & GCTileStipYOrigin) g->tsy = v->ts_y_origin;
    if (vm & GCFont) g->font = v->font;
    if (vm & GCClipMask) g->clip = v->clip_mask;
    if (vm & GCClipXOrigin) g->cx = v->clip_x_origin;
    if (vm & GCClipYOrigin) g->cy = v->clip_y_origin;
  }
  return (GC)g;
}

int XFreeGC(Display *, GC gc) {
  delete (Gc *)gc;
  return 0;
}

Status XGetGCValues(Display *, GC gc, unsigned long, XGCValues *v) {
  Gc *g = (Gc *)gc;
  memset(v, 0, sizeof *v);
  v->function = GXcopy;
  v->foreground = g->fg;
  v->background = g->bg;
  v->fill_style = g->fill;
  v->tile = g->tile;
  v->stipple = g->stipple;
  v->ts_x_origin = g->tsx;
  v->ts_y_origin = g->tsy;
  v->font = g->font;
  v->clip_mask = g->clip;
  v->clip_x_origin = g->cx;
  v->clip_y_origin = g->cy;
  return 1;
}

int XSetForeground(Display *, GC gc, unsigned long c) {
  ((Gc *)gc)->fg = c;
  return 0;
}
int XSetBackground(Display *, GC gc, unsigned long c) {
  ((Gc *)gc)->bg = c;
  return 0;
}
int XSetFillStyle(Display *, GC gc, int s) {
  ((Gc *)gc)->fill = s;
  return 0;
}
int XSetTile(Display *, GC gc, Pixmap p) {
  ((Gc *)gc)->tile = p;
  return 0;
}
int XSetStipple(Display *, GC gc, Pixmap p) {
  ((Gc *)gc)->stipple = p;
  return 0;
}
int XSetTSOrigin(Display *, GC gc, int x, int y) {
  ((Gc *)gc)->tsx = x;
  ((Gc *)gc)->tsy = y;
  return 0;
}
int XSetClipMask(Display *, GC gc, Pixmap p) {
  ((Gc *)gc)->clip = p;
  return 0;
}
int XSetClipOrigin(Display *, GC gc, int x, int y) {
  ((Gc *)gc)->cx = x;
  ((Gc *)gc)->cy = y;
  return 0;
}
int XSetFont(Display *, GC gc, Font f) {
  ((Gc *)gc)->font = f;
  return 0;
}
int XSetFunction(Display *, GC, int) { return 0; } // GXcopy only

// ------------------------------------------------------------- drawing
int XCopyArea(Display *, Drawable src, Drawable dst, GC gc, int sx, int sy,
              unsigned int w, unsigned int h, int dx, int dy) {
  Draw *s = get(src), *d = get(dst);
  if (!s || !d || s->px.empty() || d->px.empty()) return 0;
  const Gc *g = (const Gc *)gc;
  const Draw *clip = g->clip ? get(g->clip) : nullptr;
  // clamp the source rect, shifting the dest to match
  int W = (int)w, H = (int)h;
  if (sx < 0) W += sx, dx -= sx, sx = 0;
  if (sy < 0) H += sy, dy -= sy, sy = 0;
  W = std::min(W, s->w - sx);
  H = std::min(H, s->h - sy);
  if (dx < 0) W += dx, sx -= dx, dx = 0;
  if (dy < 0) H += dy, sy -= dy, dy = 0;
  W = std::min(W, d->w - dx);
  H = std::min(H, d->h - dy);
  if (W <= 0 || H <= 0) return 0;
  uint32_t m = d->depth == 1 ? 1u : 0xffffffu;
  std::vector<uint32_t> tmp;
  const uint32_t *sp = s->px.data();
  int sw = s->w;
  if (s == d) { // overlapping self-copy: snapshot the source rect
    tmp.resize((size_t)W * H);
    for (int y = 0; y < H; y++)
      memcpy(&tmp[y * W], &s->px[(sy + y) * s->w + sx], W * 4);
    sp = tmp.data();
    sw = W;
    sx = sy = 0;
  }
  for (int y = 0; y < H; y++) {
    const uint32_t *srow = sp + (sy + y) * sw + sx;
    uint32_t *drow = &d->px[(dy + y) * d->w + dx];
    if (!clip) {
      if (s->depth == d->depth)
        memcpy(drow, srow, W * 4);
      else
        for (int x = 0; x < W; x++) drow[x] = srow[x] & m;
      continue;
    }
    int my = dy + y - g->cy;
    if (my < 0 || my >= clip->h) continue;
    const uint32_t *crow = &clip->px[my * clip->w];
    for (int x = 0; x < W; x++) {
      int mx = dx + x - g->cx;
      if (mx >= 0 && mx < clip->w && crow[mx]) drow[x] = srow[x] & m;
    }
  }
  return 0;
}

int XFillRectangle(Display *, Drawable dr, GC gc, int x, int y, unsigned int w,
                   unsigned int h) {
  Draw *d = get(dr);
  if (d && !d->px.empty()) fill_rect(d, (const Gc *)gc, x, y, w, h);
  return 0;
}

int XDrawRectangle(Display *, Drawable dr, GC gc, int x, int y, unsigned int w,
                   unsigned int h) {
  Draw *d = get(dr);
  if (!d || d->px.empty()) return 0;
  const Gc *g = (const Gc *)gc;
  // X outlines cover [x, x+w] × [y, y+h] inclusive
  fill_rect(d, g, x, y, w + 1, 1);
  fill_rect(d, g, x, y + h, w + 1, 1);
  fill_rect(d, g, x, y, 1, h + 1);
  fill_rect(d, g, x + w, y, 1, h + 1);
  return 0;
}

int XDrawLine(Display *, Drawable dr, GC gc, int x1, int y1, int x2, int y2) {
  Draw *d = get(dr);
  if (d && !d->px.empty()) line(d, (const Gc *)gc, x1, y1, x2, y2);
  return 0;
}

int XDrawPoints(Display *, Drawable dr, GC gc, XPoint *pts, int n, int mode) {
  Draw *d = get(dr);
  if (!d || d->px.empty()) return 0;
  int x = 0, y = 0;
  for (int i = 0; i < n; i++) {
    if (mode == CoordModePrevious && i) {
      x += pts[i].x;
      y += pts[i].y;
    } else {
      x = pts[i].x;
      y = pts[i].y;
    }
    plot1(d, (const Gc *)gc, x, y);
  }
  return 0;
}

int XFillArc(Display *, Drawable dr, GC gc, int x, int y, unsigned int w,
             unsigned int h, int a1, int a2) {
  Draw *d = get(dr);
  if (!d || d->px.empty() || !w || !h) return 0;
  const Gc *g = (const Gc *)gc;
  const Draw *clip, *tile;
  gc_sources(g, &clip, &tile);
  bool full = abs(a2) >= 360 * 64;
  double s0 = a1 / 64.0, span = a2 / 64.0;
  double rx = w / 2.0, ry = h / 2.0, cx = x + rx, cy = y + ry;
  for (int yy = y; yy < y + (int)h; yy++)
    for (int xx = x; xx < x + (int)w; xx++) {
      double nx = (xx + 0.5 - cx) / rx, ny = (yy + 0.5 - cy) / ry;
      if (nx * nx + ny * ny > 1.0) continue;
      if (!full) {
        // X angles: counter-clockwise from 3 o'clock, y axis up
        double ang = atan2(-ny, nx) * 180.0 / M_PI;
        double rel = span >= 0 ? ang - s0 : s0 - ang;
        rel = fmod(fmod(rel, 360.0) + 360.0, 360.0);
        if (rel > fabs(span)) continue;
      }
      gc_plot(d, g, xx, yy, clip, tile);
    }
  return 0;
}

int XDrawString(Display *, Drawable dr, GC gc, int x, int y, const char *s,
                int len) {
  Draw *d = get(dr);
  if (!d || d->px.empty()) return 0;
  const Gc *g = (const Gc *)gc;
  bool big = g->font == FONT_BIG;
  int cw = big ? XF_BIG_W : XF_SMALL_W;
  int asc = big ? XF_BIG_ASC : XF_SMALL_ASC;
  int rows = big ? XF_BIG_ASC + XF_BIG_DESC : XF_SMALL_ASC + XF_SMALL_DESC;
  Gc solid = *g; // glyphs paint foreground only; ignore any tile state
  solid.fill = FillSolid;
  const Draw *clip = g->clip ? get(g->clip) : nullptr;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 32 || c > 126) c = '?';
    const uint16_t *gl = big ? xf_big[c - 32] : xf_small[c - 32];
    for (int r = 0; r < rows; r++)
      for (int b = 0; b < cw; b++)
        if (gl[r] & (0x8000 >> b))
          gc_plot(d, &solid, x + i * cw + b, y - asc + r, clip, nullptr);
  }
  return 0;
}

// --------------------------------------------------------------- fonts
XFontStruct *XLoadQueryFont(Display *, const char *name) {
  std::string n = squash(name);
  if (n.find("9x15") != std::string::npos ||
      n.find("helvetica") != std::string::npos ||
      n.find("-18-") != std::string::npos)
    return &g_font_big;
  return &g_font_small;
}

int XFreeFont(Display *, XFontStruct *) { return 0; }

int XTextWidth(XFontStruct *f, const char *, int n) {
  return f->max_bounds.width * n;
}

// ------------------------------------------------------ pixmaps/images
Pixmap XCreatePixmap(Display *, Drawable, unsigned int w, unsigned int h,
                     unsigned int depth) {
  if (!w || !h) return None;
  Draw *d = new Draw();
  d->kind = K_PIXMAP;
  d->w = w;
  d->h = h;
  d->depth = depth == 1 ? 1 : 24;
  d->px.assign((size_t)w * h, 0);
  return add(d);
}

int XFreePixmap(Display *, Pixmap p) {
  Draw *d = get(p);
  if (d && d->kind == K_PIXMAP) {
    delete d;
    g_obj[p] = nullptr;
  }
  return 0;
}

Pixmap XCreateBitmapFromData(Display *dpy, Drawable dr, const char *data,
                             unsigned int w, unsigned int h) {
  Pixmap p = XCreatePixmap(dpy, dr, w, h, 1);
  Draw *d = get(p);
  if (!d) return None;
  int stride = (w + 7) / 8; // XBM: LSB-first, rows byte-padded
  for (unsigned y = 0; y < h; y++)
    for (unsigned x = 0; x < w; x++)
      d->px[y * w + x] = (data[y * stride + x / 8] >> (x % 8)) & 1;
  return p;
}

XImage *XCreateImage(Display *, Visual *, unsigned int depth, int, int,
                     char *data, unsigned int w, unsigned int h, int, int) {
  return make_image(depth == 1 ? 1 : 24, w, h, data);
}

int XDestroyImage(XImage *im) {
  if (!im) return 0;
  free(im->data);
  free(im);
  return 0;
}

unsigned long XGetPixel(XImage *im, int x, int y) {
  return ((uint32_t *)im->data)[y * im->width + x];
}

int XPutPixel(XImage *im, int x, int y, unsigned long p) {
  ((uint32_t *)im->data)[y * im->width + x] =
      im->depth == 1 ? (p & 1) : (p & 0xffffff);
  return 0;
}

XImage *XGetImage(Display *, Drawable dr, int x, int y, unsigned int w,
                  unsigned int h, unsigned long, int) {
  Draw *d = get(dr);
  if (!d || d->px.empty()) return nullptr;
  uint32_t *buf = (uint32_t *)calloc((size_t)w * h, 4);
  for (unsigned yy = 0; yy < h; yy++)
    for (unsigned xx = 0; xx < w; xx++) {
      int sx = x + xx, sy = y + yy;
      if (sx >= 0 && sy >= 0 && sx < d->w && sy < d->h)
        buf[yy * w + xx] = d->px[sy * d->w + sx];
    }
  return make_image(d->depth, w, h, (char *)buf);
}

int XPutImage(Display *, Drawable dr, GC, XImage *im, int sx, int sy, int dx,
              int dy, unsigned int w, unsigned int h) {
  Draw *d = get(dr);
  if (!d || d->px.empty() || !im) return 0;
  uint32_t m = d->depth == 1 ? 1u : 0xffffffu;
  const uint32_t *src = (const uint32_t *)im->data;
  for (unsigned yy = 0; yy < h; yy++)
    for (unsigned xx = 0; xx < w; xx++) {
      int ix = sx + xx, iy = sy + yy, ox = dx + xx, oy = dy + yy;
      if (ix < 0 || iy < 0 || ix >= im->width || iy >= im->height) continue;
      if (ox < 0 || oy < 0 || ox >= d->w || oy >= d->h) continue;
      d->px[oy * d->w + ox] = src[iy * im->width + ix] & m;
    }
  return 0;
}

// ----------------------------------------------------------------- Xpm
int XpmCreateImageFromData(Display *, char **data, XImage **image,
                           XImage **shape, XpmAttributes *attr) {
  if (attr) attr->pixels = nullptr, attr->npixels = 0;
  XpmParsed p;
  if (!xpm_parse(data, p)) return XpmFileInvalid;
  size_t n = (size_t)p.w * p.h;
  if (image) {
    uint32_t *buf = (uint32_t *)malloc(n * 4);
    std::copy(p.rgb.begin(), p.rgb.end(), buf);
    *image = make_image(24, p.w, p.h, (char *)buf);
  }
  if (shape) {
    *shape = nullptr; // libXpm: no transparent colour → no mask
    if (p.has_none) {
      uint32_t *buf = (uint32_t *)malloc(n * 4);
      for (size_t i = 0; i < n; i++) buf[i] = p.opaque[i];
      *shape = make_image(1, p.w, p.h, (char *)buf);
    }
  }
  xpm_attrs(attr, p);
  return XpmSuccess;
}

int XpmCreatePixmapFromData(Display *dpy, Drawable dr, char **data,
                            Pixmap *pixmap, Pixmap *shape,
                            XpmAttributes *attr) {
  if (attr) attr->pixels = nullptr, attr->npixels = 0;
  XpmParsed p;
  if (!xpm_parse(data, p)) return XpmFileInvalid;
  if (pixmap) {
    *pixmap = XCreatePixmap(dpy, dr, p.w, p.h, 24);
    std::copy(p.rgb.begin(), p.rgb.end(), get(*pixmap)->px.begin());
  }
  if (shape) {
    *shape = None;
    if (p.has_none) {
      *shape = XCreatePixmap(dpy, dr, p.w, p.h, 1);
      Draw *m = get(*shape);
      for (size_t i = 0; i < p.opaque.size(); i++) m->px[i] = p.opaque[i];
    }
  }
  xpm_attrs(attr, p);
  return XpmSuccess;
}

void XpmFreeAttributes(XpmAttributes *a) {
  if (!a) return;
  free(a->pixels);
  a->pixels = nullptr;
  a->npixels = 0;
}

// ------------------------------------------------------------ keyboard
KeyCode XKeysymToKeycode(Display *, KeySym ks) {
  for (int i = 0; i < NKEYS; i++)
    if (KEYS[i].ks0 == ks || KEYS[i].ks1 == ks) return (KeyCode)(i + 8);
  return 0;
}

KeySym XKeycodeToKeysym(Display *, unsigned int kc, int index) {
  const KeyEnt *k = keyent(kc);
  if (!k) return NoSymbol;
  return index == 0 ? k->ks0 : index == 1 ? k->ks1 : NoSymbol;
}

KeySym XStringToKeysym(const char *s) {
  for (int i = 0; i < NKEYS; i++)
    if (!strcmp(KEYS[i].name, s)) return KEYS[i].ks0;
  if (strlen(s) == 1) {
    for (int i = 0; i < NKEYS; i++)
      if (KEYS[i].ks1 == (KeySym)(unsigned char)s[0]) return KEYS[i].ks1;
  }
  return NoSymbol;
}

char *XKeysymToString(KeySym ks) {
  for (int i = 0; i < NKEYS; i++) {
    if (KEYS[i].ks0 == ks) return (char *)KEYS[i].name;
  }
  static char one[2];
  if (ks >= 0x20 && ks < 0x7f) { // shifted Latin-1 keysyms equal ASCII
    one[0] = (char)ks;
    one[1] = 0;
    return one;
  }
  return nullptr;
}

int XLookupString(XKeyEvent *e, char *buf, int n, KeySym *ks,
                  XComposeStatus *) {
  const KeyEnt *k = keyent(e->keycode);
  KeySym sym = NoSymbol;
  if (k) {
    bool shift = e->state & ShiftMask;
    bool caps = (e->state & LockMask) && k->ks0 >= XK_a && k->ks0 <= XK_z;
    sym = (shift != caps) ? k->ks1 : k->ks0;
  }
  if (ks) *ks = sym;
  int len = 0;
  char c = 0;
  if (sym >= 0x20 && sym < 0x7f)
    c = (char)sym;
  else if (sym == XK_Return || sym == XK_KP_Enter)
    c = '\r';
  else if (sym == XK_BackSpace)
    c = '\b';
  else if (sym == XK_Tab)
    c = '\t';
  else if (sym == XK_Escape)
    c = 27;
  else if (sym == XK_Delete)
    c = 127;
  if (c && (e->state & ControlMask) && c >= '@' && c < 0x7f) c &= 0x1f;
  if (c && n > 0) {
    buf[0] = c;
    len = 1;
    if (n > 1) buf[1] = 0;
  }
  return len;
}

} // extern "C"
