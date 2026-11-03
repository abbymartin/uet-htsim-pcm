// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
//
// A fixed window: the PCM version of `-sender_cc_algo constant`. The handler
// runs on every ACK but never changes the window, so a run with
// `-pcm constant.yaml` sends exactly like one with `-sender_cc_algo constant`.
// It is the smallest complete PCM algorithm, and a check that connecting PCM to
// the sender changes nothing by itself.

#include "uet_pcm.h"

PCM_ALGORITHM(constant);
PCM_SIGNAL(T_ACK);

PCM_HANDLER(constant) {
    // Nothing to do: the window stays as the simulator set it up.
}
