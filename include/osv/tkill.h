#ifndef _TKILL_H_
#define _TKILL_H_
#include<osv/stubbing.hh>
int
tkill (int tid, int sig)
{
    debug_always("tkill: curr thread:%d is sending sig:%d to thread:%d\n",\
            sched::thread::current()->id(), sig, tid);
    return 0;
}
#endif 
