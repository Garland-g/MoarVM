#include "moarvm.h"

#define init_mutex(loc, name) do { \
    if ((apr_init_stat = apr_thread_mutex_create(&loc, APR_THREAD_MUTEX_DEFAULT, instance->apr_pool)) != APR_SUCCESS) { \
        char error[256]; \
        fprintf(stderr, "MoarVM: Initialization of " name " mutex failed\n    %s\n", \
            apr_strerror(apr_init_stat, error, 256)); \
        exit(1); \
	} \
} while (0)

/* Create a new instance of the VM. */
MVMInstance * MVM_vm_create_instance(void) {
    MVMInstance *instance;
    apr_status_t apr_init_stat;
    
    /* Set up APR related bits. */
    apr_init_stat = apr_initialize();
    if (apr_init_stat != APR_SUCCESS) {
        char error[256];
        fprintf(stderr, "MoarVM: Initialization of APR failed\n    %s\n",
            apr_strerror(apr_init_stat, error, 256));
        exit(1);
    }

    /* Set up instance data structure. */
    instance = calloc(1, sizeof(MVMInstance));
    instance->boot_types = calloc(1, sizeof(struct _MVMBootTypes));
    
    /* Allocate instance APR pool. */
    if ((apr_init_stat = apr_pool_create(&instance->apr_pool, NULL)) != APR_SUCCESS) {
        char error[256];
        fprintf(stderr, "MoarVM: Initialization of APR pool failed\n    %s\n",
            apr_strerror(apr_init_stat, error, 256));
        exit(1);
    }
    
    /* Create the main thread's ThreadContext and stash it. */
    instance->main_thread = MVM_tc_create(instance);
    
    /* No user threads when we start, and next thread to be created gets ID 1
     * (the main thread got ID 0). */
    instance->num_user_threads    = 0;
    instance->next_user_thread_id = 1;

    /* Set up the permanent roots storage. */
    instance->num_permroots   = 0;
    instance->alloc_permroots = 16;
    instance->permroots       = malloc(sizeof(MVMCollectable **) * instance->alloc_permroots);
    init_mutex(instance->mutex_permroots, "permanent roots");
    
    /* Set up HLL config mutex. */
    init_mutex(instance->mutex_hllconfigs, "hll configs");

    /* Bootstrap 6model. It is assumed the GC will not be called during this. */
    MVM_6model_bootstrap(instance->main_thread);
    
    /* Create main thread object, and also make it the start of the all threads
     * linked list. */
    instance->threads = 
        instance->main_thread->thread_obj = (MVMThread *)
            REPR(instance->boot_types->BOOTThread)->allocate(
                instance->main_thread, STABLE(instance->boot_types->BOOTThread));
    instance->threads->body.stage = MVM_thread_stage_started;
    instance->threads->body.tc = instance->main_thread;
    
    /* Create compiler registry */
    instance->compiler_registry = MVM_repr_alloc_init(instance->main_thread, instance->boot_types->BOOTHash);
    MVM_gc_root_add_permanent(instance->main_thread, (MVMCollectable **)&instance->compiler_registry);
    
    /* Set up compiler registr mutex. */
    init_mutex(instance->mutex_compiler_registry, "compiler registry");
    
    /* Create hll symbol tables */
    instance->hll_syms = MVM_repr_alloc_init(instance->main_thread, instance->boot_types->BOOTHash);
    MVM_gc_root_add_permanent(instance->main_thread, (MVMCollectable **)&instance->hll_syms);
    
    /* Set up hll symbol tables mutex. */
    init_mutex(instance->mutex_hll_syms, "hll syms");
    
    return instance;
}

/* This callback is passed to the interpreter code. It takes care of making
 * the initial invocation. */
static void toplevel_initial_invoke(MVMThreadContext *tc, void *data) {
    /* Dummy, 0-arg callsite. */
    static MVMCallsite no_arg_callsite;
    no_arg_callsite.arg_flags = NULL;
    no_arg_callsite.arg_count = 0;
    no_arg_callsite.num_pos   = 0;
    
    /* Create initial frame, which sets up all of the interpreter state also. */
    MVM_frame_invoke(tc, (MVMStaticFrame *)data, &no_arg_callsite, NULL, NULL, NULL);
}

/* Loads bytecode from the specified file name and runs it. */
void MVM_vm_run_file(MVMInstance *instance, char *filename) {
    /* Map the compilation unit into memory and dissect it. */
    MVMThreadContext *tc = instance->main_thread;
    MVMCompUnit      *cu = MVM_cu_map_from_file(tc, filename);
    
    /* Run the frame marked main, or if there is none then fall back to the
     * first frame. */
    MVMStaticFrame *start_frame = cu->main_frame ? cu->main_frame : cu->frames[0];
    MVM_interp_run(tc, &toplevel_initial_invoke, start_frame);
}

/* Loads bytecode from the specified file name and dumps it. */
void MVM_vm_dump_file(MVMInstance *instance, char *filename) {
    /* Map the compilation unit into memory and dissect it. */
    MVMThreadContext *tc = instance->main_thread;
    MVMCompUnit      *cu = MVM_cu_map_from_file(tc, filename);
    char *dump = MVM_bytecode_dump(tc, cu);
    
    printf("%s", dump);
    free(dump);
}

/* Destroys a VM instance. */
void MVM_vm_destroy_instance(MVMInstance *instance) {
    MVMuint16 i;
    
    /* TODO: Lots of cleanup. */
    
    /* Destroy main thread contexts. */
    MVM_tc_destroy(instance->main_thread);
    
    /* Clean up GC permanent roots related resources. */
    apr_thread_mutex_destroy(instance->mutex_permroots);
    free(instance->permroots);

    /* Free APR pool. */
    apr_pool_destroy(instance->apr_pool);
    
    /* Clear up VM instance memory. */
    free(instance);
    
    /* Terminate APR. */
    apr_terminate();
}
