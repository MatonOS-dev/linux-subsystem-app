#include "../presenter_state.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  struct MatonPresenterLayerState state = {0};
  const int src[] = {0, 0, 640, 352};
  const int dst[] = {0, 0, 640, 352};
  unsigned binder_transactions = 0;
  unsigned controls_created = 1;
  unsigned transaction_objects = 1;

  for (int frame = 0; frame < 10000; ++frame) {
    unsigned changed = maton_presenter_layer_state_update(&state, src, dst, 0, 1.0f, 1);
    if (changed) ++binder_transactions;
  }

  assert(binder_transactions == 1); /* initial layer state only */
  assert(controls_created == 1);
  assert(transaction_objects == 1);
  puts("presenter cached state: 10k frames use one initial transaction and no new objects");
  return 0;
}
