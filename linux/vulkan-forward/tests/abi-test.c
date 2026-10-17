#define _GNU_SOURCE
#include <stddef.h>
#define adopt_thread provider_thread
#define adopt_api provider_api
#include "../../gpu-forward-spike/spike3/adopt.h"
#undef adopt_thread
#undef adopt_api
#include "backend.h"
#define CHECK(f) _Static_assert(offsetof(struct provider_thread,f)==offsetof(struct adopt_thread,f),"adoption offset " #f)
CHECK(resident_fs);CHECK(selector);CHECK(app_fs);
_Static_assert(offsetof(struct provider_api,current)==offsetof(struct adopt_api,current),"current offset");
int main(void){return 0;}
