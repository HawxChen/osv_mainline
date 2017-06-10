void exit_group(int status) {
    debug_always("curr thread:%d is calling exit_group with status:%d\n",\
           sched::thread::current()->id(), status);
    exit(status);
    return;
}
