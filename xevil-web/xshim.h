/* xshim.h (cyberspace7 XEvil port): frame-driver hooks into xshim.cpp. */
#ifndef C7_XSHIM_H
#define C7_XSHIM_H
#ifdef __cplusplus
extern "C" {
#endif
void xshim_present(void); /* composite mapped windows → SDL canvas */
void xshim_pump(void);    /* drain SDL input into the X event queue */
void xshim_quit(void);    /* WM_DELETE to the main window (host closed us) */
#ifdef __cplusplus
}
#endif
#endif
