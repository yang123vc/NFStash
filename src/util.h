#ifndef UTIL_H
#define UTIL_H

#include "nfsping.h"

nfs_fh_list *parse_fh(char *);
int print_fhandle3(struct sockaddr *, char *, fhandle3);
int print_nfs_fh3(struct sockaddr *, char *, char *, nfs_fh3);
char* reverse_fqdn(char *);
unsigned long tv2us(struct timeval);
unsigned long tv2ms(struct timeval);
void ms2tv(struct timeval *, unsigned long);
void ms2ts(struct timespec *, unsigned long);
unsigned long ts2ms(struct timespec);

#endif /* UTIL_H */
