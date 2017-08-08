/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef ARCH_TLS_HH
#define ARCH_TLS_HH

// Don't change the declaration sequence of all existing members'.
// Please add new members from the last.
struct thread_control_block {
    thread_control_block* self;
    void* tls_base;
    // This field, a per-thread stack for SYSCALL instruction,
    // is used in arch/x64/entry.S for %fs's offset.
    // We currently keep this field in the TCB to make it easier to access in assembly code
    // through a known offset into %fs. But with more effort, we could have used an
    // ordinary thread-local variable instead and avoided extending the TCB here.
    void* syscall_stack_addr;
};

#endif /* ARCH_TLS_HH */
