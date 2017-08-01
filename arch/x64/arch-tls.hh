/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_TLS_HH
#define ARCH_TLS_HH

//Don't change the declaration sequence of all existing members'.
//Please add new members from the last.
struct thread_control_block {
    thread_control_block* self;
    void* tls_base;
    //FIXME: Through linke and TLS, stack_addr could be deleted. 
    //FIXME: The discussion form issue #808 provides details.
    void* stack_addr; //This field is used in arch/x64/entry.S for %fs's offset.
};

#endif /* ARCH_TLS_HH */
