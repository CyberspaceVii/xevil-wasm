/*
 * xevil_main.cpp (cyberspace7 XEvil port): replaces x11/main.cpp.
 *
 * Upstream main() owns a blocking `while (True)` loop and sleeps in
 * Game::yield() → select(). A browser can't block its main thread, so one
 * turn is factored into turn() and driven two ways:
 *   native: the upstream loop shape, unchanged pacing (yield sleeps);
 *   wasm:   emscripten_set_main_loop at rAF rate, running whole turns off
 *             a fixed-step accumulator of Game::get_quanta() ms.
 * The licence dialog is a modal XNextEvent loop that can't run under the
 * browser loop either; "-accept_agreement" (XEvil's own switch) skips it,
 * the GPL notice is credited on the hosting page instead.
 */
#include "utils.h"

extern "C" {
#include <X11/Xutil.h>
#include <X11/Xos.h>
}

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "game.h"
#include "xshim.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using namespace std;

namespace {

class TurnStarter : public ITurnStarter {
public:
  TurnStarter(struct timeval *timer) : m_timer(timer) {}
  virtual void start_turn() { gettimeofday(m_timer, NULL); }

private:
  struct timeval *m_timer;
};

GameP game;
struct timeval startTime;
TurnStarter turnStarter(&startTime);

// One game turn: clock, drain X events, clock, show. False once the game quits.
bool turn() {
  game->pre_clock();
  if (game->has_ui()) {
    for (int dpyNum = 0; dpyNum < game->get_dpy_max(); dpyNum++) {
      int n = XEventsQueued(game->get_dpy(dpyNum), QueuedAfterReading);
      for (int m = 0; m < n; m++) {
        XEvent event;
        XNextEvent(game->get_dpy(dpyNum), &event);
        game->process_event(dpyNum, &event);
      }
    }
  }
  game->post_clock();
  xshim_present();
  return !game->quit_game();
}

#ifdef __EMSCRIPTEN__
double lastMs = 0, acc = 0;

void frame() {
  double now = emscripten_get_now();
  double q = game->get_quanta();
  acc += now - lastMs;
  lastMs = now;
  if (acc > 10 * q) acc = q; // tab was backgrounded: don't fast-forward
  while (acc >= q) {
    acc -= q;
    if (!turn()) {
      emscripten_cancel_main_loop();
      delete game;
      exit(0); // EXIT_RUNTIME → Module.onExit; the host drops this instance
    }
  }
}
#endif

} // namespace

#ifdef __EMSCRIPTEN__
// Host window minimized/restored: stop burning CPU while hidden; on resume,
// restart the clock so the accumulator doesn't try to catch up.
extern "C" EMSCRIPTEN_KEEPALIVE void xevil_set_paused(int paused) {
  if (paused) {
    emscripten_pause_main_loop();
  } else {
    lastMs = emscripten_get_now();
    acc = 0;
    emscripten_resume_main_loop();
  }
}
#endif

int main(int argc, char **argv) {
  Utils::seed_random();

  // Always skip the modal licence dialog (see header).
  vector<char *> args(argv, argv + argc);
  bool have = false;
  for (char *a : args) have |= !strcmp(a, "-accept_agreement");
  static char accept[] = "-accept_agreement";
  if (!have) args.push_back(accept);
  int ac = (int)args.size();
  args.push_back(nullptr);

  game = new Game(&ac, args.data());
  turnStarter.start_turn();

#ifdef __EMSCRIPTEN__
  lastMs = emscripten_get_now();
  emscripten_set_main_loop(frame, 0, 0);
  // Headless test harnesses: virtual time doesn't advance rAF, so allow a
  // setTimeout-paced loop on request. Real pages keep vsync'd rAF.
  if (EM_ASM_INT({ return Module.c7timeoutLoop ? 1 : 0; }))
    emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 16);
#else
  while (turn()) game->yield(startTime, game->get_quanta(), &turnStarter);
  delete game;
#endif
  return 0;
}
