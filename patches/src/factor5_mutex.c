// initFactor5Mutex (0x80091FC4): boot calls it twice, and the second call re-creates the queue and posts a duplicate token. Only the first runs, which keeps the recursive mutex consistent.
#include "rs64_api.h"

void osCreateMesgQueue(void* mq, void* msg, int count);
int osSendMesg(void* mq, void* msg, int flag);

#define FACTOR5_MUTEX_QUEUE ((void*)0x80149990)
#define FACTOR5_MUTEX_MSG ((void*)0x801499A8)

static int s_inited;

RECOMP_PATCH void initFactor5Mutex(void) {
    if (s_inited) {
        return;
    }
    s_inited = 1;
    osCreateMesgQueue(FACTOR5_MUTEX_QUEUE, FACTOR5_MUTEX_MSG, 1);
    osSendMesg(FACTOR5_MUTEX_QUEUE, 0, 0);
}
