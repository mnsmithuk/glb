/*
 * Copyright (C) 2008-2012 Codership Oy <info@codership.com>
 *
 * $Id$
 */

#include "glb_cmd.h"
#include "glb_log.h"
#include "glb_limits.h"
#include "glb_signal.h"
#include "glb_daemon.h"
#include "glb_router.h"
#include "glb_pool.h"
#include "glb_listener.h"
#include "glb_control.h"

#include <unistd.h>    // for sleep()
#include <sys/types.h>
#include <sys/stat.h>  // for mkfifo()
#include <fcntl.h>     // for open()
#include <errno.h>

/* this function is to allocate all possible resources before dropping
 * privileges */
static int
allocate_resources(const glb_cnf_t* conf,
                   int* ctrl_fifo,
                   int* ctrl_sock,
                   int* listen_sock)
{
    if (mkfifo (conf->fifo_name, S_IRUSR | S_IWUSR)) {
        switch (errno)
        {
        case EEXIST:
            glb_log_error ("FIFO '%s' already exists. Check that no other "
                           "glbd instance is running and delete it "
                           "or specify another name with --fifo option.",
                           conf->fifo_name);
            break;
        default:
            glb_log_error ("Could not create FIFO '%s': %d (%s)",
                           conf->fifo_name, errno, strerror(errno));

        }
        return 1;
    }

    *ctrl_fifo = open (conf->fifo_name, O_RDWR);
    if (*ctrl_fifo < 0) {
        int err = -(*ctrl_fifo);
        glb_log_error ("Ctrl: failed to open FIFO file: %d (%s)",
                       err, strerror (err));
        goto cleanup1;
    }

    if (conf->ctrl_set) {
        *ctrl_sock = glb_socket_create (&conf->ctrl_addr,GLB_SOCK_DEFER_ACCEPT);
        if (*ctrl_sock < 0) {
//            int err = -(*ctrl_fifo);
//            glb_log_error ("Ctrl: failed to create listening socket: %d (%s)",
//                           err, strerror (err));
            goto cleanup2;
        }
    }

    *listen_sock = glb_socket_create (&conf->inc_addr, GLB_SOCK_DEFER_ACCEPT);
    if (*listen_sock < 0) {
//        int err = -(*ctrl_fifo);
//        glb_log_error ("Failed to create listening socket: %d (%s)",
//                       err, strerror (err));
        goto cleanup3;
    }

    return 0;

cleanup3:
    close (*ctrl_sock);
    *ctrl_sock = 0;
cleanup2:
    close (*ctrl_fifo);
    *ctrl_fifo = 0;
cleanup1:
    remove (glb_cnf->fifo_name);

    return 1;
}

static void
free_resources (int const ctrl_fifo, int const ctrl_sock, int const lsock)
{
    if (lsock) close (lsock);
    if (ctrl_sock) close (ctrl_sock);
    if (ctrl_fifo) {
        close (ctrl_fifo);
        remove (glb_cnf->fifo_name);
    }
}

int main (int argc, char* argv[])
{
    glb_router_t*   router;
    glb_pool_t*     pool;
    glb_listener_t* listener;
    glb_ctrl_t*     ctrl;
    uint16_t        inc_port;

    int listen_sock, ctrl_fifo, ctrl_sock = 0;

    glb_limits_init();
    if (!glb_cnf_init()) exit(EXIT_FAILURE);

    glb_cmd_parse (argc, argv);
    if (!glb_cnf) {
        fprintf (stderr, "Failed to parse arguments. Exiting.\n");
        exit (EXIT_FAILURE);
    }

    glb_cnf_print (stdout, glb_cnf);

    if (glb_log_init (GLB_LOG_PRINTF)) {
        fprintf (stderr, "Failed to initialize logger. Aborting.\n");
        exit (EXIT_FAILURE);
    }

    if (allocate_resources (glb_cnf, &ctrl_fifo, &ctrl_sock, &listen_sock)) {
        glb_log_fatal ("Failed to allocate inital resources. Aborting.\n");
        exit (EXIT_FAILURE);
    }

    glb_signal_set_handler();

    if (glb_cnf->daemonize) {
        glb_daemon_start();
        // at this point we're a child process
    }

    router = glb_router_create (glb_cnf->n_dst, glb_cnf->dst);
    if (!router) {
        glb_log_fatal ("Failed to create router. Exiting.");
        goto failure;
    }

    pool = glb_pool_create (glb_cnf->n_threads, router);
    if (!pool) {
        glb_log_fatal ("Failed to create thread pool. Exiting.");
        goto failure;
    }

    listener = glb_listener_create (router, pool, listen_sock);
    if (!listener) {
        glb_log_fatal ("Failed to create connection listener. Exiting.");
        goto failure;
    }

    inc_port = glb_socket_addr_get_port (&glb_cnf->inc_addr);
    ctrl = glb_ctrl_create (router, pool, inc_port, ctrl_fifo, ctrl_sock);
    if (!ctrl) {
        glb_log_fatal ("Failed to create control thread. Exiting.");
        goto failure;
    }

    if (glb_cnf->daemonize) {
        glb_daemon_ok (); // Tell parent that daemon successfully started
        glb_log_info ("Started.");
    }

    while (!glb_terminate) {

        if (!glb_cnf->daemonize) {
            char stats[BUFSIZ];

            glb_router_print_info (router, stats, BUFSIZ);
            puts (stats);

            glb_pool_print_info (pool, stats, BUFSIZ);
            puts (stats);
        }

        sleep (5);
    }

    // cleanup
    glb_ctrl_destroy (ctrl);

    if (glb_cnf->daemonize) {
        glb_log_info ("Exit.");
    }

    free_resources (ctrl_fifo, ctrl_sock, listen_sock);
    return 0;

failure:
    free_resources (ctrl_fifo, ctrl_sock, listen_sock);
    exit (EXIT_FAILURE);
}
