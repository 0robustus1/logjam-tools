#include "importer-watchdog.h"

// the watchdog actor aborts the process if does not receive ticks for
// 10 consecutive ticks

#define CREDIT 10

typedef struct {
    int credit;                     // number of ticks left before we shut down
    bool received_term_cmd;         // whether we have received a TERM command
} watchdog_state_t;


static int timer_event(zloop_t *loop, int timer_id, void *arg)
{
    watchdog_state_t *state = arg;
    state->credit--;
    if (state->credit == 0) {
        fflush(stdout);
        fprintf(stderr, "[E] watchdog: no credit left, aborting process\n");
        abort();
    } else if (state->credit < CREDIT - 1) {
        printf("[I] watchdog: credit left: %d\n", state->credit);
    }
    return 0;
}

static
int actor_command(zloop_t *loop, zsock_t *socket, void *arg)
{
    int rc = 0;
    watchdog_state_t *state = arg;
    zmsg_t *msg = zmsg_recv(socket);
    if (msg) {
        char *cmd = zmsg_popstr(msg);
        if (streq(cmd, "$TERM")) {
            state->received_term_cmd = true;
            // fprintf(stderr, "[D] watchdog[0]: received $TERM command\n");
            rc = -1;
        }
        else if (streq(cmd, "tick")) {
            if (verbose)
                printf("[I] watchdog: credit: %d\n", state->credit);
            state->credit = CREDIT;
        } else {
            fprintf(stderr, "[E] watchdog[0]: received unknown actor command: %s\n", cmd);
        }
        free(cmd);
        zmsg_destroy(&msg);
    }
    return rc;
}


void watchdog(zsock_t *pipe, void *args)
{
    set_thread_name("watchdog[0]");

    int rc;
    watchdog_state_t state = { .credit = CREDIT, .received_term_cmd = false };

    // signal readyiness
    zsock_signal(pipe, 0);

    // set up event loop
    zloop_t *loop = zloop_new();
    assert(loop);
    zloop_set_verbose(loop, 0);
    // we rely on the controller shutting us down
    zloop_ignore_interrupts(loop);

    // decrease credit every second
    rc = zloop_timer(loop, 1000, 0, timer_event, &state);
    assert(rc != -1);

    // setup handler for actor messages
    rc = zloop_reader(loop, pipe, actor_command, &state);
    assert(rc == 0);

    // run the loop
    bool should_continue_to_run = getenv("CPUPROFILE") != NULL;
    do {
        rc = zloop_start(loop);
        should_continue_to_run &= errno == EINTR;
        if (!state.received_term_cmd)
            log_zmq_error(rc, __FILE__, __LINE__);
    } while (should_continue_to_run);

    if (!quiet)
        printf("[I] watchdog[0]: shutting down\n");

    // shutdown
    zloop_destroy(&loop);
    assert(loop == NULL);

    if (!quiet)
        printf("[I] watchdog[0]: terminated\n");
}
