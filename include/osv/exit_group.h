void exit_group(int status) {
    debug_always("exit_group: curr thread:%d is calling exit_group with status:%d\n",\
           sched::thread::current()->id(), status);
    exit(status);
    return;
}
