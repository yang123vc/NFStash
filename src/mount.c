/*
 * Get root filehandles from NFS server
 */


#include "nfsping.h"
#include "rpc.h"
#include "util.h"

/* local prototypes */
static void usage(void);
static void mount_perror(mountstat3);
static exports get_exports(struct targets *);
static mountres3 *get_root_filehandle(CLIENT *, char *, char *, unsigned long *);
static int print_exports(char *, struct exportnode *);
static targets_t *make_exports(targets_t *);
void print_output(enum outputs, const int, targets_t *, const fhandle3, const struct timespec, unsigned long);
void print_summary(targets_t *, enum outputs, const int);

/* globals */
extern volatile sig_atomic_t quitting;
int verbose = 0;


void usage() {
    printf("Usage: nfsmount [options] host[:mountpoint]\n\
    -A       show IP addresses\n\
    -c n     count of mount requests to send to target\n\
    -C n     same as -c, output parseable format\n\
    -D       print timestamp (unix time) before each line\n\
    -e       print exports (like showmount -e)\n\
    -h       display this help and exit\n\
    -J       force JSON output\n\
    -l       loop forever\n\
    -m       use multiple target IP addresses if found (implies -A)\n\
    -p n     polling interval, check targets every n ms (default 1000)\n\
    -S addr  set source address\n\
    -T       use TCP (default UDP)\n\
    -v       verbose output\n");

    exit(3);
}


/* print an error message for mount results */
/* for now just print the error name from the header */
void mount_perror(mountstat3 fhs_status) {
    static const char *labels[] = {
        [MNT3ERR_NOENT]       = "MNT3ERR_NOENT",
        [MNT3ERR_ACCES]       = "MNT3ERR_ACCES",
        [MNT3ERR_NOTDIR]      = "MNT3ERR_NOTDIR",
        [MNT3ERR_INVAL]       = "MNT3ERR_INVAL",
        [MNT3ERR_NAMETOOLONG] = "MNT3ERR_NAMETOOLONG",
        [MNT3ERR_NOTSUPP]     = "MNT3ERR_NOTSUPP",
        [MNT3ERR_SERVERFAULT] = "MNT3ERR_SERVERFAULT",
    };

    if (fhs_status && fhs_status != MNT3_OK) {
        fprintf(stderr, "%s\n", labels[fhs_status]);
    }
}


/* get the list of exports from a server */
exports get_exports(struct targets *target) {
    struct rpc_err clnt_err;
    exports ex = NULL;
    unsigned long usec;
    struct timespec call_start, call_end, call_elapsed;

    if (target->client) {
        /* first time marker */
#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &call_start);
#else
        clock_gettime(CLOCK_MONOTONIC, &call_start);
#endif

        /* the actual RPC call */
        ex = *mountproc_export_3(NULL, target->client);

        /* second time marker */
#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &call_end);
#else
        clock_gettime(CLOCK_MONOTONIC, &call_end);
#endif

        /* calculate elapsed microseconds */
        timespecsub(&call_end, &call_start, &call_elapsed);
        usec = ts2us(call_elapsed);

        /* print timing to stderr so it doesn't get piped to other commands */
        /* TODO unless we're doing graphite output */
        fprintf(stderr, "%s (%s): mountproc_export_3=%03.2f ms\n", target->name, target->ip_address, usec / 1000.0);
    }

    /* export call doesn't return errors */
    /* it also doesn't usually require a privileged port */
    if (ex == NULL) {
        /* RPC error */
        clnt_geterr(target->client, &clnt_err);
        if  (clnt_err.re_status) {
            fprintf(stderr, "%s: ", target->name);
            clnt_perror(target->client, "mountproc_export_3");
        }
    }

    return ex;
}


/* get the root filehandle from the server */
/* take a pointer to usec so we can return the elapsed call time */
mountres3 *get_root_filehandle(CLIENT *client, char *hostname, char *path, unsigned long *usec) {
    struct rpc_err clnt_err;
    mountres3 *mountres = NULL;
    struct timespec call_start, call_end, call_elapsed;

    if (client) {
        /* first time marker */
#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &call_start);
#else
        clock_gettime(CLOCK_MONOTONIC, &call_start);
#endif

        /* the actual RPC call */
        mountres = mountproc_mnt_3(&path, client);

        /* second time marker */
#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &call_end);
#else
        clock_gettime(CLOCK_MONOTONIC, &call_end);
#endif

        /* calculate elapsed microseconds */
        timespecsub(&call_end, &call_start, &call_elapsed);
        *usec = ts2us(call_elapsed);
    }

    if (mountres) {
        if (mountres->fhs_status != MNT3_OK) {
            fprintf(stderr, "%s:%s: ", hostname, path);
            /* check if we get an access error, this probably means the server wants us to use a reserved port */
            /* TODO do this check in mount_perror? */
            if (mountres->fhs_status == MNT3ERR_ACCES && geteuid()) {
                fprintf(stderr, "Unable to mount filesystem, consider running as root\n");
            } else {
                mount_perror(mountres->fhs_status);
            }
        }
    /* RPC error */
    } else {
        clnt_geterr(client, &clnt_err);
        if  (clnt_err.re_status) {
            fprintf(stderr, "%s:%s: ", hostname, path);
            clnt_perror(client, "mountproc_mnt_3");
        }
    }

    return mountres;
}


/* print a list of exports, like showmount -e */
int print_exports(char *host, struct exportnode *ex) {
    int i = 0;
    size_t max = 0;
    exports first = ex;
    groups gr;

    while (ex) {
        i++;
        if (strlen(ex->ex_dir) > max) {
            max = strlen(ex->ex_dir);
        }

        ex = ex->ex_next;
    }

    ex = first;
    max++; /* spacing */

    while (ex) {
        printf("%s:%-*s", host, (int)max, ex->ex_dir);
        gr = ex->ex_groups;
        if (gr) {
            printf("%s", gr->gr_name);
            gr = gr->gr_next;
            while (gr) {
                printf(",%s", gr->gr_name);
                gr = gr->gr_next;
            }
        } else {
            /* no groups means open to everyone */
            printf("(everyone)");
        }
        printf("\n");
        ex = ex->ex_next;
    }

    return i;
}


/* make a target list by querying the server for a list of exports */
targets_t *make_exports(targets_t *target) {
    exports ex;
    targets_t dummy;
    targets_t *current = &dummy;

    dummy.next = NULL;

    if (target->client) {
        /* get the list of exports from the server */
        ex = get_exports(target);

        while (ex) {
            /* copy the target don't make a new one */
            current->next = copy_target(target);
            current = current->next;
            /* terminate the list */
            current->next = NULL;

            /* copy the hostname from the mount result into the target */
            current->path = calloc(1, MNTPATHLEN);
            strncpy(current->path, ex->ex_dir, MNTPATHLEN);

            ex = ex->ex_next;
        }
    }

    /* skip the dummy entry */
    return dummy.next;
}


/* print output to stdout in different formats for each mount result */
void print_output(enum outputs format, const int width, targets_t *target, const fhandle3 file_handle, const struct timespec wall_clock, unsigned long usec) {
    double loss = (target->sent - target->received) / target->sent * 100.0;
    char epoch[TIME_T_MAX_DIGITS]; /* the largest time_t seconds value, plus a terminating NUL */
    struct tm *secs;

    switch(format) {
        case unixtime:
            /* get the epoch time in seconds in the local timezone */
            /* TODO should we be doing everything in UTC? */
            /* strftime needs a struct tm so use localtime to convert from time_t */
            secs = localtime(&wall_clock.tv_sec);
            strftime(epoch, sizeof(epoch), "%s", secs);
            printf("[%s.%06li] ", epoch, wall_clock.tv_nsec / 1000);
            /* fall through to ping output, this just prepends the current time */
            /*FALLTHROUGH*/
        /* print "ping" style output */
        case ping:
        case fping: /* fping is only different in the summary at the end */
            printf("%s:%-*s : [%u], %03.2f ms (%03.2f avg, %.0f%% loss)\n",
                target->name,
                /* have to cast size_t to int for compiler warning */
                /* printf only accepts ints for field widths with * */
                width - (int)strlen(target->name),
                target->path,
                target->sent - 1,
                usec / 1000.0,
                target->avg / 1000.0,
                loss);
            break;
        /* print the filehandle as JSON */
        case json:
            print_fhandle3(target, file_handle, usec, wall_clock);
            break;
        default:
            fatalx(3, "Unsupported format\n");
    }
}


/* print a summary to stderr */
void print_summary(targets_t *targets, enum outputs format, const int width) {
    targets_t *current = targets;
    double loss;
    unsigned long i;

    /* print a newline between the results and the summary */
    fprintf(stderr, "\n");

    while (current) {
        /* first print the aligned host and path */
        fprintf(stderr, "%s:%-*s :",
            current->name,
            width - (int)strlen(current->name),
            current->path);

        switch (format) {
            case ping:
            case unixtime:
            case json: /* not sure if this makes sense? */
                loss = (current->sent - current->received) / current->sent * 100.0;
                fprintf(stderr, " xmt/rcv/%%loss = %u/%u/%.0f%%",
                    current->sent, current->received, loss);
                /* only print times if we got any responses */
                if (current->received) {
                    fprintf(stderr, ", min/avg/max = %.2f/%.2f/%.2f",
                        current->min / 1000.0, current->avg / 1000.0, current->max / 1000.0);
                }
                break;
            case fping:
                for (i = 0; i < current->sent; i++) {
                    if (current->results[i]) {
                        fprintf(stderr, " %.2f", current->results[i] / 1000.0);
                    } else {
                        fprintf(stderr, " -");
                    }
                }
                break;
            default:
                fatalx(3, "There should be a summary here!");
        }

        fprintf(stderr, "\n");

        current = current->next;
    }
}


int main(int argc, char **argv) {
    mountres3 *mountres;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        /* default to UDP */
        .ai_socktype = SOCK_DGRAM
    };
    char *host;
    char *path;
    exports ex;
    /* target lists */
    targets_t target_dummy = {0};
    /* pointer to head of list */
    targets_t *current = &target_dummy;
    /* global target list */
    targets_t *targets = current;
    /* temporary target list for building new targets from arguments */
    targets_t *new_targets;
    /* temporary target list for looking up exports on server */
    targets_t exports_dummy = {0};
    targets_t *exports_dummy_ptr = &exports_dummy;
    /* counters for results */
    unsigned int exports_count = 0;
    unsigned int exports_ok    = 0;
    /* getopt */
    int ch;
    /* command line options */
    /* default to unset so we can check in getopt */
    enum outputs format = unset;
    unsigned long count = 0;
    uint16_t port       = 0; /* 0 = use portmapper */
    int dns             = 0;
    int ip              = 0;
    int loop            = 0;
    int multiple        = 0;
    int showmount       = 0;
    u_long version      = 3;
    struct timeval timeout     = NFS_TIMEOUT;
    struct timespec sleep_time = NFS_SLEEP;
    /* source ip address for packets */
    struct sockaddr_in src_ip = {
        .sin_family = AF_INET,
        .sin_addr = 0
    };
    /* response time in microseconds */
    unsigned long usec;
    struct timespec wall_clock;
    /* for output alignment */
    /* printf requires an int for %*s formats */
    int width    = 0;
    int tmpwidth = 0;

    /* no arguments passed */
    if (argc == 1)
        usage();

    while ((ch = getopt(argc, argv, "Ac:C:DehJlmp:S:Tv")) != -1) {
        switch(ch) {
            /* show IP addresses instead of hostnames */
            case 'A':
                ip = 1;
                break;
            /* ping output with a count */
            case 'c':
                if (loop) {
                    fatal("Can't specify both -l and -c!\n");
                }
                if (format == unset) {
                    format = ping;
                } else if (format == fping) {
                    fatal("Can't specify both -C and -c!\n");
                } /* -D and -J are ok */
                count = strtoul(optarg, NULL, 10);
                if (count == 0 || count == ULONG_MAX) {
                    fatal("Zero count, nothing to do!\n");
                }
                break;
            case 'C':
                if (loop) {
                    fatal("Can't specify both -l and -C!\n");
                }
                if (format == unset) {
                    format = fping;
                } else if (format == unixtime) {
                    fatal("Can't specify both -D and -C!\n");
                } else if (format == ping) {
                    fatal("Can't specify both -c and -C!\n");
                } else if (format == json) {
                    /* JSON doesn't have a summary */
                    fatal("Can't specify both -J and -C, use -c instead!\n");
                }
                count = strtoul(optarg, NULL, 10);
                if (count == 0 || count == ULONG_MAX) {
                    fatal("Zero count, nothing to do!\n");
                }
                break;
            /* unixtime ping output */
            case 'D':
                if (format == fping) {
                    fatal("Can't specify both -C and -D!\n");
                } else if (format == json) {
                    fatal("Can't specify both -J and -D!\n");
                }
                format = unixtime;
                break;
            /* output like showmount -e */
            case 'e':
                showmount = 1;
                break;
            case 'J':
                if (format == unixtime) {
                    fatal("Can't specify both -J and -D!\n");
                } else if (format == fping) {
                    fatal("Can't specify both -J and -C, use -c instead!\n");
                } /* -c is ok */
                format = json;
                break;
            case 'l':
                /* Can't count and loop */
                if (count) {
                    //if (format == ping||unixtime) {
                    if (format == ping || format == unixtime) {
                        fatal("Can't specify both -c and -l!\n");
                    } else if (format == fping) {
                        fatal("Can't specify both -C and -l!\n");
                    }
                } else if (format == unset) {
                    format = ping;
                } /* other formats are ok */

                loop = 1;
                break;
            /* use multiple IP addresses if found */
            /* in this case we also want to default to showing IP addresses instead of names */
            case 'm':
                multiple = 1;
                /* implies -A to use IP addresses so output isn't ambiguous */
                ip = 1;
                break;
            /* time between pings to target */
            case 'p':
                /* TODO check for reasonable values */
                ms2ts(&sleep_time, strtoul(optarg, NULL, 10));
                break;
            /* specify source address */
            case 'S':
                if (inet_pton(AF_INET, optarg, &src_ip.sin_addr) != 1) {
                    fatal("Invalid source IP address!\n");
                }
                break;
            /* use TCP */
            case 'T':
                hints.ai_socktype = SOCK_STREAM;
                break;
            /* verbose */
            case 'v':
                verbose = 1;
                break;
            case 'h':
            case '?':
            default:
                usage();
        }
    }

    /* if not looping, default to JSON output to pipe to other commands */
    if (count + loop == 0) {
        format = json;
    }

    /* loop through arguments and create targets */
    /* TODO accept from stdin? */
    while (optind < argc) {
        /* split host:path arguments, path is optional */
        host = strtok(argv[optind], ":");
        path = strtok(NULL, ":");

        if (path) {
            /* check for valid path */
            if (path[0] != '/') {
                fatalx(3, "%s: Invalid path: %s\n", host, path);
            }

            /* TODO check length against MNTPATHLEN */

            if (showmount) {
                fatalx(3, "Can't specify -e (exports) and a path!\n");
            }
        }

        /* make possibly multiple new targets */
        new_targets = make_target(host, &hints, port, dns, multiple, count, format);

        /* go through this argument's list of possibly multiple dns responses/targets */
        current = new_targets;

        while (current) {
            if (path) {
                /* copy the path into the new target */
                current->path = calloc(1, MNTPATHLEN);
                strncpy(current->path, path, MNTPATHLEN);
            /* no path given, look up exports on server */
            } else {
                /* first create an rpc connection so we can query the server for an exports list */
                current->client = create_rpc_client(current->client_sock, &hints, MOUNTPROG, version, timeout, src_ip);

                if (showmount) {
                    ex = get_exports(current);
                    exports_count = print_exports(host, ex);
                } else {
                    /* look up the export list on the server and create a target for each */
                    append_target(&exports_dummy_ptr, make_exports(current));
                }
            }

            current = current->next;
        }

        /* append to the global target list */
        if (path) {
            append_target(&targets, new_targets);
        } else {
            /* TODO showmount */
            /* skip the dummy entry */
            append_target(&targets, exports_dummy_ptr->next);
        }

        /* reset the exports list to reuse on the next loop */
        exports_dummy_ptr->next = NULL;

        optind++;
    }

    /* listen for ctrl-c */
    quitting = 0;
    signal(SIGINT, sigint_handler);

    /* don't need to do the target loop for showmount */
    if (showmount) {
        targets = NULL;
    } else {
        /* skip the first dummy entry */
        targets = targets->next;

        /* calculate the maximum width for aligned printing */
        current = targets;
        while (current) {
            tmpwidth = strlen(current->name) + strlen(current->path);
            if (tmpwidth > width) {
                width = tmpwidth;
            }

            current = current->next;
        }
    }


    /* now we have a target list, loop through and query the server(s) */
    while(1) {
        /* reset to head of list */
        current = targets;

        while(current) {

            if (current->client == NULL) {
                /* TODO see if we can reuse the previous target's connection */
                /* create an rpc connection */
                current->client = create_rpc_client(current->client_sock, &hints, MOUNTPROG, version, timeout, src_ip);
                /* mounts don't need authentication because they return a list of authentication flavours supported so leave it as default (AUTH_NONE) */
            }

            if (current->client) {
                exports_count++;

                /* get the current timestamp */
                clock_gettime(CLOCK_REALTIME, &wall_clock);

                /* the RPC call */
                mountres = get_root_filehandle(current->client, current->name, current->path, &usec);

                current->sent++;

                if (mountres && mountres->fhs_status == MNT3_OK) {
                    current->received++;
                    exports_ok++;

                    /* only calculate these if we're looping */
                    if (count || loop) {
                        if (usec < current->min) current->min = usec;
                        if (usec > current->max) current->max = usec;
                        /* calculate the average time */
                        current->avg = (current->avg * (current->received - 1) + usec) / current->received;

                        if (format == fping) {
                            current->results[current->sent - 1] = usec;
                        }
                    }

                    print_output(format, width, current, mountres->mountres3_u.mountinfo.fhandle, wall_clock, usec);
                }
            }

            current = current->next;

        } /* while(current) */

        /* ctrl-c */
        if (quitting) {
            break;
        }

        if ((count && targets->sent < count) || loop) {
            /* sleep between rounds */
            nanosleep(&sleep_time, NULL);
        } else {
            break;
        }

    } /* while(1) */

    /* only print summary if looping */
    if (count || loop) {
        print_summary(targets, format, width);
    }

    if (exports_count && exports_count == exports_ok) {
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
