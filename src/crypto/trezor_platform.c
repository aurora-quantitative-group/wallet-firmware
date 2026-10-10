// Platform hooks required by the vendored trezor-crypto sources.
#include <stdio.h>
#include <stdlib.h>

#include "fault_handler.h"

// Called by consteq() if a loop-completion check fails (fault injection).
// There is no safe way to continue, so stop the process; systemd restarts it.
void tc_fault_handler(const char *msg) {
  fprintf(stderr, "trezor-crypto fault: %s\n", msg);
  abort();
}
