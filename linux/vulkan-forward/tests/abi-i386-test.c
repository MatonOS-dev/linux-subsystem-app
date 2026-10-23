#define adopt_thread provider_thread
#define adopt_api provider_api
#define adopt_scope provider_scope
#include "../../compat-proxy/arch/i386/adopt.h"
#undef adopt_thread
#undef adopt_api
#undef adopt_scope
#include "../arch/i386/backend.h"
#include <stddef.h>

#define SAME_SIZE(a,b) _Static_assert(sizeof(struct a)==sizeof(struct b), #a " size")
#define SAME_FIELD(a,b,f) _Static_assert(offsetof(struct a,f)==offsetof(struct b,f), #f " offset")
SAME_SIZE(provider_thread,adopt_thread);
SAME_FIELD(provider_thread,adopt_thread,tid);
SAME_FIELD(provider_thread,adopt_thread,android_tls);
SAME_FIELD(provider_thread,adopt_thread,android_selector);
SAME_FIELD(provider_thread,adopt_thread,scope_state);
SAME_FIELD(provider_thread,adopt_thread,scope_depth);
SAME_SIZE(provider_scope,adopt_scope);
SAME_FIELD(provider_scope,adopt_scope,thread);
SAME_FIELD(provider_scope,adopt_scope,previous_gs);
SAME_FIELD(provider_scope,adopt_scope,active);
SAME_FIELD(provider_scope,adopt_scope,already_android);
SAME_SIZE(provider_api,adopt_api);
SAME_FIELD(provider_api,adopt_api,version);
SAME_FIELD(provider_api,adopt_api,current);
SAME_FIELD(provider_api,adopt_api,lookup);
SAME_FIELD(provider_api,adopt_api,identity);
SAME_FIELD(provider_api,adopt_api,clones);
SAME_FIELD(provider_api,adopt_api,clone3s);
SAME_FIELD(provider_api,adopt_api,live);
SAME_FIELD(provider_api,adopt_api,fork_prepares);
SAME_FIELD(provider_api,adopt_api,fork_parents);
SAME_FIELD(provider_api,adopt_api,fork_children);
SAME_FIELD(provider_api,adopt_api,vforks);
SAME_FIELD(provider_api,adopt_api,switch_in);
SAME_FIELD(provider_api,adopt_api,switch_out);
int maton_i386_adoption_abi_check(void) { return 0; }
