/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/app.hh>
#include <string>
#include <osv/run.hh>
#include <osv/power.hh>
#include <osv/trace.hh>
#include <functional>
#include <thread>
#include <libgen.h>
#include <errno.h>
#include <algorithm>
#include <boost/range/algorithm/transform.hpp>
#include <osv/wait_record.hh>
#include "libc/pthread.hh"
#include<osv/stubbing.hh>
#define __APP_SOL__


using namespace boost::range;

extern int optind;

// Java uses this global variable (supplied by Glibc) to figure out
// aproximatively where the initial thread's stack end.
void *__libc_stack_end;

extern "C" void __libc_start_main(int (*main)(int, char**), int, char**,
    void(*)(), void(*)(), void(*)(), void*)
{
    debug_always("libc_start_main\n");
    auto app = osv::application::get_current();
    assert(app->_entry_point);
    app->_main = main;
    app->run_main();
    app.reset();
    pthread_exit(nullptr);
}

namespace osv {

__thread application* override_current_app;

void app_registry::join() {
    while (true) {
        shared_app_t p;
        WITH_LOCK(lock) {
            if (apps.empty()) {
                return;
            }
            p = apps.front();
        }
        try {
            p->join();
        } catch (const multiple_join_error& e) {
            // At the clean up stage even if join was called before
            // it will be ignore allowing to complete the join
        }
    }
}

bool app_registry::remove(application* app) {
    bool found = false;
    WITH_LOCK(lock) {
        apps.remove_if([app, &found](shared_app_t a){
            if (a.get() == app) {
                found = true;
                return true;
            }
            return false;
        });
    }
    return found;
}

void app_registry::push(shared_app_t app) {
    WITH_LOCK(lock) {
        apps.push_back(app);
    }
}

application_runtime::~application_runtime()
{
    if (app._joiner) {
        app._joiner->wake_with([&] { app._terminated.store(true); });
    }
}

app_registry application::apps;

shared_app_t application::get_current()
{
    auto runtime = sched::thread::current()->app_runtime();
    if (!runtime) {
        return nullptr;
    }
    return runtime->app.get_shared();
}

bool application::unsafe_stop_and_abandon_other_threads()
{
    debug_always("application::unsafe_stop_and_abandon_other_threads\n");
    auto current = sched::thread::current();
    auto current_runtime = current->app_runtime();
    bool success = true;
    // We don't have a list of threads belonging to this app, so need to do
    // it the long slow way, listing all threads...
    sched::with_all_threads([&](sched::thread &t) {
        if (&t != current && t.app_runtime() == current_runtime) {
            if (t.unsafe_stop()) {
                t.set_app_runtime(nullptr);
            } else {
                success = false;
            }
        }
    });
    return success;
}

shared_app_t application::run(const std::vector<std::string>& args)
{
    debug_always("application:: run @args\n");
    return run(args[0], args);
}

shared_app_t application::run(const std::string& command,
                      const std::vector<std::string>& args,
                      bool new_program,
                      const std::unordered_map<std::string, std::string> *env)
{
    debug_always("application:: run @new program\n");
    auto app = std::make_shared<application>(command, args, new_program, env);
    app->start();
    apps.push(app);
    return app;
}

shared_app_t application::run_and_join(const std::string& command,
                      const std::vector<std::string>& args,
                      bool new_program,
                      const std::unordered_map<std::string, std::string> *env,
                      waiter* setup_waiter)
{
    auto app = std::make_shared<application>(command, args, new_program, env);
    app->start_and_join(setup_waiter);
    return app;
}

application::application(const std::string& command,
                     const std::vector<std::string>& args,
                     bool new_program,
                     const std::unordered_map<std::string, std::string> *env)
    : _args(args)
    , _command(command)
    , _termination_requested(false)
    , _runtime(new application_runtime(*this))
    , _joiner(nullptr)
    , _terminated(false)
{
    debug_always("application::application\n");
    try {
        elf::program *current_program;

        if (new_program) {
            this->new_program();
            clone_osv_environ();
            current_program = _program.get();
        } else {
            // Do it in a separate branch because elf::get_program() would not
            // have found us yet in the previous branch.
            current_program = elf::get_program();
        }

        merge_in_environ(new_program, env);
	prepare_argv(current_program);
        _lib = current_program->get_library(_command, {}, true);
#if 0
        _lib = current_program->get_library(_command);
#endif
    } catch(const std::exception &e) {
        throw launch_error(e.what());
    }

    if (!_lib) {
        throw launch_error("Failed to load object: " + command);
    }


    _main = _lib->lookup<int (int, char**)>("main");
#ifdef __APP_SOL__
    if (!_main) {
        _entry_point = _lib->lookup<void ()>("GoMain");
        debug_always("Get GoMain:%p\n", _entry_point);

    }

    if (!_entry_point && !_main) {
        debug_always("Did not Get GoMain\n");
        _entry_point = reinterpret_cast<void(*)()>(_lib->entry_point());
    }
#else
    if (!_main) {
        _entry_point = reinterpret_cast<void(*)()>(_lib->entry_point());
    }
#endif

    if (!_entry_point && !_main) {
        throw launch_error("Failed looking up main");
    }
}

void application::start()
{
    // FIXME: we cannot create the thread inside the constructor because
    // the thread would attempt to call shared_from_this() before object
    // is constructed which is illegal.
    debug_always("application::start\n");
    override_current_app = this;
    auto err = pthread_create(&_thread, NULL, [](void *app) -> void* {
        ((application*)app)->main();
        return nullptr;
    }, this);
    override_current_app = nullptr;
    if (err) {
        throw launch_error("Failed to create the main thread, err=" + std::to_string(err));
    }
    debug_always("application::start finished safely\n");
}

TRACEPOINT(trace_app_destroy, "app=%p", application*);

application::~application()
{
    assert(_runtime.use_count() == 1 || !_runtime);
    trace_app_destroy(this);
}

TRACEPOINT(trace_app_join, "app=%p", application*);
TRACEPOINT(trace_app_join_ret, "return_code=%d", int);

int application::join()
{
    if (!apps.remove(this)) {
        throw multiple_join_error();
    }
    trace_app_join(this);
    auto err = pthread_join(_thread, NULL);
    assert(!err);

    _joiner = sched::thread::current();
    _runtime.reset();
    sched::thread::wait_until([&] { return _terminated.load(); });

    _termination_request_callbacks.clear();
    _lib.reset();

    trace_app_join_ret(_return_code);
    return _return_code;
}

void application::start_and_join(waiter* setup_waiter)
{
    debug_always("application::start_and_join\n");
    // We start the new application code in the current thread. We temporarily
    // change the app_runtime pointer of this thread, while keeping the old
    // pointer saved and restoring it when the new application ends (keeping
    // the shared pointer also keeps the calling application alive).
    auto original_app = sched::thread::current()->app_runtime();
    sched::thread::current()->set_app_runtime(runtime());
    auto original_name = sched::thread::current()->name();
    if (setup_waiter) {
        setup_waiter->wake();
    }
    _thread = pthread_self(); // may be null if the caller is not a pthread.
    main();
    // FIXME: run_tsd_dtors() is a hack - If the new program registered a
    // destructor via pthread_key_create() and this thread keeps living with
    // data for this key, the destructor function, part of the new program,
    // may be called after the program is unloaded - and crash. Let's run
    // the destructors now. This is wrong if the calling program has its own
    // thread-local data. It is fine if the thread was created specially for
    // running start_and_join (or run_and_join() or osv::run()).
    pthread_private::run_tsd_dtors();
    sched::thread::current()->set_name(original_name);
    sched::thread::current()->set_app_runtime(original_app);
    original_app.reset();
    _joiner = sched::thread::current();
    _runtime.reset();
    sched::thread::wait_until([&] { return _terminated.load(); });
    _termination_request_callbacks.clear();
    _lib.reset();
}

TRACEPOINT(trace_app_main, "app=%p, cmd=%s", application*, const char*);
TRACEPOINT(trace_app_main_ret, "return_code=%d", int);

void application::main()
{
    debug_always("application::main by id:%u\n", sched::thread::current()->id());
    __libc_stack_end = __builtin_frame_address(0);

    elf::get_program()->init_library(_args.size(), _argv.get());
    sched::thread::current()->set_name(_command);

    if (_main) {
        debug_always("application::main ---> run_main() by id:%u\n", sched::thread::current()->id());
        run_main();
    } else {
        // The application is expected not to initialize the environment in
        // which it runs on its owns but to call __libc_start_main(). If that's
        // not the case bad things may happen: constructors of global objects
        // may be called twice, TLS may be overriden and the program may not
        // received correct arguments, environment variables and auxiliary
        // vector.
        debug_always("application::main ---> _entry_point:%p tid:%u\n", _entry_point, sched::thread::current()->id());
        //auto runtime = sched::thread::current()->app_runtime();

        // _lib->lookup<void ()>("GoMain")();

        _entry_point();
    }

    // _entry_point() doesn't return
}

void application::prepare_argv(elf::program *program)
{
    // Prepare program_* variable used by the libc
    char *c_path = (char *)(_command.c_str());
    program_invocation_name = c_path;
    debug_always("program_invocation_name: %s\n", program_invocation_name);
    program_invocation_short_name = basename(c_path);
    debug_always("program_invocation_short_name: %s\n", program_invocation_short_name);

    // Allocate a continuous buffer for arguments: _argv_buf
    // First count the trailing zeroes
    auto sz = _args.size();
    // Then add the sum of each argument size to sz
    int i_idx = 0;
    for (auto &str: _args) {
        debug_always("_args[%d]: %s, len:%d\n", i_idx, _args[i_idx], str.size());
        sz += str.size() + 1;
        i_idx++;
    }
    _argv_buf.reset(new char[sz]);

    // In Linux, the pointer arrays argv[] and envp[] are continguous.
    // Unfortunately, some programs rely on this fact (e.g., libgo's
    // runtime_goenvs_unix()) so it is useful that we do this too.

    // First count the number of environment variables
    int envcount = 0;
    while (environ[envcount]) {
        envcount++;
    }

    // Allocate the continuous buffer for argv[] and envp[]
    _argv.reset(new char*[_args.size() + 1 + envcount + 1 + sizeof(Elf64_auxv_t) * 2]);

    // Fill the argv part of these buffers
    char *ab = _argv_buf.get();
    char **contig_argv = _argv.get();
    for (size_t i = 0; i < _args.size(); i++) {
	auto &str = _args[i];
        memcpy(ab, str.c_str(), str.size());
        ab[str.size()] = '\0';
        contig_argv[i] = ab;
        ab += str.size() + 1;
    }
    contig_argv[_args.size()] = nullptr;

    // Do the same for environ
    for (int i = 0; i < envcount; i++) {
        contig_argv[_args.size() + 1 + i] = environ[i];
    }
    contig_argv[_args.size() + 1 + envcount] = nullptr;

    _libvdso = program->get_library("libvdso.so");
    if (!_libvdso) {
        abort("could not load libvdso.so\n");
        return;
    }

    // Pass the VDSO library to the application.
    Elf64_auxv_t* _auxv =
        reinterpret_cast<Elf64_auxv_t *>(&contig_argv[_args.size() + 1 + envcount + 1]);
    _auxv[0].a_type = AT_SYSINFO_EHDR;
    _auxv[0].a_un.a_val = reinterpret_cast<uint64_t>(_libvdso->base());

    _auxv[1].a_type = AT_NULL;
    _auxv[1].a_un.a_val = 0;
}

void application::run_main(std::string path, int argc, char** argv)
{
    debug_always("application::run_main @argc,argv\n");
    char *c_path = (char *)(path.c_str());
    // path is guaranteed to keep existing this function
    program_invocation_name = c_path;
    program_invocation_short_name = basename(c_path);

    auto sz = argc; // for the trailing 0's.
    for (int i = 0; i < argc; ++i) {
        sz += strlen(argv[i]);
    }

    std::unique_ptr<char []> argv_buf(new char[sz]);
    char *ab = argv_buf.get();
    // In Linux, the pointer arrays argv[] and envp[] are continguous.
    // Unfortunately, some programs rely on this fact (e.g., libgo's
    // runtime_goenvs_unix()) so it is useful that we do this too.
    int envcount = 0;
    while (environ[envcount]) {
        envcount++;
    }
    char *contig_argv[argc + 1 + envcount + 1];

    for (int i = 0; i < argc; ++i) {
        size_t asize = strlen(argv[i]);
        memcpy(ab, argv[i], asize);
        ab[asize] = '\0';
        contig_argv[i] = ab;
        ab += asize + 1;
    }
    contig_argv[argc] = nullptr;

    for (int i = 0; i < envcount; i++) {
        contig_argv[argc + 1 + i] = environ[i];
    }
    contig_argv[argc + 1 + envcount] = nullptr;

    // make sure to have a fresh optind across calls
    // FIXME: fails if run() is executed in parallel
    int old_optind = optind;
    optind = 0;
    _return_code = _main(argc, contig_argv);
    optind = old_optind;
}

void application::run_main()
{
    debug_always("application::run_main @void\n");
    trace_app_main(this, _command.c_str());

#if 0
    // C main wants mutable arguments, so we have can't use strings directly
    std::vector<std::vector<char>> mut_args;
    transform(_args, back_inserter(mut_args),
            [](std::string s) { return std::vector<char>(s.data(), s.data() + s.size() + 1); });
    std::vector<char*> argv;
    transform(mut_args.begin(), mut_args.end(), back_inserter(argv),
            [](std::vector<char>& s) { return s.data(); });
    auto argc = argv.size();
    argv.push_back(nullptr);
    run_main(_command, argc, argv.data());
#endif

    int old_optind = optind;
    optind = 0;
    _return_code = _main(_args.size(), _argv.get());
    optind = old_optind;

    if (_return_code) {
        debug("program %s returned %d\n", _command.c_str(), _return_code);
    }

    trace_app_main_ret(_return_code);
}

TRACEPOINT(trace_app_termination_callback_added, "app=%p", application*);
TRACEPOINT(trace_app_termination_callback_fired, "app=%p", application*);

void application::on_termination_request(std::function<void()> callback)
{
    auto app = get_current();
    std::unique_lock<mutex> lock(app->_termination_mutex);
    if (app->_termination_requested) {
        lock.unlock();
        callback();
        return;
    }

    app->_termination_request_callbacks.push_front(std::move(callback));
}

TRACEPOINT(trace_app_request_termination, "app=%p, requested=%d", application*, bool);
TRACEPOINT(trace_app_request_termination_ret, "");

void application::request_termination()
{
    WITH_LOCK(_termination_mutex) {
        trace_app_request_termination(this, _termination_requested);
        if (_termination_requested) {
            trace_app_request_termination_ret();
            return;
        }
        _termination_requested = true;
    }

    if (get_current().get() == this) {
        for (auto &callback : _termination_request_callbacks) {
            callback();
        }
    } else {
        override_current_app = this;
        std::thread terminator([&] {
            for (auto &callback : _termination_request_callbacks) {
                callback();
            }
        });
        override_current_app = nullptr;
        terminator.join();
    }

    trace_app_request_termination_ret();
}

int application::get_return_code()
{
    return _return_code;
}

std::string application::get_command()
{
    return _command;
}

pid_t application::get_main_thread_id() {
    return pthread_gettid_np(_thread);
}

// For simplicity, we will not reuse bits in the bitmap, since no destructor is
// assigned to the program. In that case, a simple counter would do. But coding
// this way is easy, and make this future extension simple.
constexpr int max_namespaces = 32;
std::bitset<max_namespaces> namespaces(1);

void application::new_program()
{
    debug_always("application::new_program\n");
    for (unsigned long i = 0; i < max_namespaces; ++i) {
        if (!namespaces.test(i)) {
            namespaces.set(i);
            // This currently limits the size of the executable and shared
            // libraries in each "program" to 1<<33 bytes, i.e., 8 GB.
            // This should hopefully be more than enough; It is not a
            // limit on the amount of memory this program can allocate -
            // just a limit on the code size.
            void *addr =
	        reinterpret_cast<void *>(elf::program_base) + ((i + 1) << 33);
           _program.reset(new elf::program(addr));
           return;
        }
    }
    abort("application::new_program() out of namespaces.\n");
}

elf::program *application::program() {
    debug_always("elf::program *application::program\n");
    return _program.get();
}


void application::clone_osv_environ()
{
    debug_always("application:: clone_osv_environ\n");
    _libenviron = _program->get_library("libenviron.so");
    if (!_libenviron) {
        abort("could not load libenviron.so\n");
        return;
    }

    if (!environ) {
        return;
    }

    auto putenv = _libenviron->lookup<int (const char *)>("putenv");

    for (int i=0; environ[i] ; i++) {
        // putenv simply assign the char * we have to duplicate it.
        // FIXME: this will leak memory when the application is destroyed.
        putenv(strdup(environ[i]));
    }
}

void application::set_environ(const std::string &key, const std::string &value,
                              bool new_program)
{
    // create a pointer to OSv's libc setenv()
    debug_always("application:: setenviron\n");
    auto my_setenv = setenv;

    if (new_program) {
        // If we are starting a new program use the libenviron.so's setenv()
        my_setenv =
            _libenviron->lookup<int (const char *, const char *, int)>("setenv");
    }

    // We do not need to strdup() since the libc will malloc() for us
    // Note that we merge in the existing environment variables by
    // using setenv() merge parameter.
    // FIXME: This will leak at application exit.
    my_setenv(key.c_str(), value.c_str(), 1);
}

void application::merge_in_environ(bool new_program,
        const std::unordered_map<std::string, std::string> *env)
{
    if (!env) {
        return;
    }

    for (auto &iter: *env) {
        set_environ(iter.first, iter.second, new_program);
    }
}

void with_all_app_threads(std::function<void(sched::thread &)> f, sched::thread& th1) {
    sched::with_all_threads([&](sched::thread &th2) {
        if (th2.app_runtime() == th1.app_runtime()) {
            f(th2);
        }
    });
}

namespace this_application {

void on_termination_request(std::function<void()> callback)
{
    application::on_termination_request(callback);
}

}

}
