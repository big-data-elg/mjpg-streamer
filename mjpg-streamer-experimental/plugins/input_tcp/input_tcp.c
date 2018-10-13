/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <resolv.h>
#include <arpa/inet.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "FILE input plugin"
#define MAX_UDP_LENGTH 65507

typedef enum _read_mode {
    NewFilesOnly,
    ExistingFiles
} read_mode;

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

static double delay = 1.0;
static char *folder = NULL;
static char *filename = NULL;
static int rm = 0;
static int plugin_number;
static read_mode mode = NewFilesOnly;

/* global variables for this plugin */
static int fd, rc, wd, size;
static struct inotify_event *ev;

/*** plugin interface functions ***/
int input_init(input_parameter *param, int id)
{
    int i;
    plugin_number = id;

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0
            },
            {"help", no_argument, 0, 0},
            {"d", required_argument, 0, 0},
            {"delay", required_argument, 0, 0},
            {"f", required_argument, 0, 0},
            {"folder", required_argument, 0, 0},
            {"r", no_argument, 0, 0},
            {"remove", no_argument, 0, 0},
            {"n", required_argument, 0, 0},
            {"name", required_argument, 0, 0},
            {"e", no_argument, 0, 0},
            {"existing", no_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* d, delay */
        case 2:
        case 3:
            DBG("case 2,3\n");
            delay = atof(optarg);
            break;

            /* f, folder */
        case 4:
        case 5:
            DBG("case 4,5\n");
            folder = malloc(strlen(optarg) + 2);
            strcpy(folder, optarg);
            if(optarg[strlen(optarg)-1] != '/')
                strcat(folder, "/");
            break;

            /* r, remove */
        case 6:
        case 7:
            DBG("case 6,7\n");
            rm = 1;
            break;

            /* n, name */
        case 8:
        case 9:
            DBG("case 8,9\n");
            filename = malloc(strlen(optarg) + 1);
            strcpy(filename, optarg);
            break;
            /* e, existing */
        case 10:
        case 11:
            DBG("case 10,11\n");
            mode = ExistingFiles;
            break;
        default:
            DBG("default case\n");
            help();
            return 1;
        }
    }

    pglobal = param->global;

#if 0
    /* check for required parameters */
    if(folder == NULL) {
        IPRINT("ERROR: no folder specified\n");
        return 1;
    }
#endif

    IPRINT("folder to watch...: %s\n", folder);
    IPRINT("forced delay......: %.4f\n", delay);
    IPRINT("delete file.......: %s\n", (rm) ? "yes, delete" : "no, do not delete");
    IPRINT("filename must be..: %s\n", (filename == NULL) ? "-no filter for certain filename set-" : filename);

    param->global->in[id].name = malloc((strlen(INPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->in[id].name, INPUT_PLUGIN_NAME);

    return 0;
}

int input_stop(int id)
{
    DBG("will cancel input thread\n");
    pthread_cancel(worker);
    return 0;
}

int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

    pthread_detach(worker);

    return 0;
}

/*** private functions for this plugin below ***/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-d | --delay ]........: delay (in seconds) to pause between frames\n" \
    " [-f | --folder ].......: folder to watch for new JPEG files\n" \
    " [-r | --remove ].......: remove/delete JPEG file after reading\n" \
    " [-n | --name ].........: ignore changes unless filename matches\n" \
    " [-e | --existing ].....: serve the existing *.jpg files from the specified directory\n" \
    " ---------------------------------------------------------------\n");
}

/* the single writer thread */
void *worker_thread(void *arg)
{
    size_t read_size = 0;
    struct timeval timestamp;
    struct sockaddr_in addr, client;
    int sd, csock, c;
    unsigned int addr_len = sizeof(addr);
    sd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8888);
    char *image, *buffer;

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
    {
        perror("bind");
        return NULL;
    }
    listen(sd, 3);

    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);

    while(!pglobal->stop)
    {
        c = sizeof(struct sockaddr_in);
        csock = accept(sd, (struct sockaddr*)&client, (socklen_t*)&c);
        if (csock < 0)
        {
            perror("accept failed");
            return NULL;
        }
        IPRINT("accepted..\n");

        while (1)
        {
            unsigned int image_size = 0;
            read_size = recv(csock, &image_size, sizeof(image_size), 0);
            if (read_size <= 0)
                goto close_client;
            IPRINT("image size:%d\n", image_size);

            image = buffer = malloc(image_size);
            if(buffer == NULL) {
                fprintf(stderr, "could not allocate memory\n");
                return NULL;
            }

            unsigned int toread = image_size;
            while (toread > 0)
            {
                read_size = recv(csock, buffer, toread, 0);
                if (read_size <= 0)
                    goto close_client;
                toread -= read_size;
                IPRINT("read:%d left:%d\n", read_size, toread);
                buffer += read_size;
            }
            pthread_mutex_lock(&pglobal->in[plugin_number].db);
            pglobal->in[plugin_number].buf = image;
            pglobal->in[plugin_number].size = image_size;

            gettimeofday(&timestamp, NULL);
            pglobal->in[plugin_number].timestamp = timestamp;
            DBG("new frame copied (size: %d)\n", pglobal->in[plugin_number].size);
            /* signal fresh_frame */
            pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
            pthread_mutex_unlock(&pglobal->in[plugin_number].db);
        }

        if(delay != 0)
            usleep(1000 * 1000 * delay);

close_client:
        close(csock);
    }


    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);

    return NULL;
}

void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;

    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }

    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");

    if(pglobal->in[plugin_number].buf != NULL) free(pglobal->in[plugin_number].buf);

    free(ev);
}





