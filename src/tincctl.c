/*
    tincctl.c -- Controlling a running tincd
    Copyright (C) 2007-2009 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include <getopt.h>

#include "xalloc.h"
#include "protocol.h"
#include "control_common.h"
#include "rsagen.h"
#include "utils.h"

/* The name this program was run with. */
char *program_name = NULL;

/* If nonzero, display usage information and exit. */
bool show_help = false;

/* If nonzero, print the version on standard output and exit.  */
bool show_version = false;

/* If nonzero, it will attempt to kill a running tincd and exit. */
int kill_tincd = 0;

/* If nonzero, generate public/private keypair for this host/net. */
int generate_keys = 0;

static char *identname = NULL;				/* program name for syslog */
static char *controlsocketname = NULL;			/* pid file location */
char *netname = NULL;
char *confbase = NULL;

#ifdef HAVE_MINGW
static struct WSAData wsa_state;
#endif

static struct option const long_options[] = {
	{"config", required_argument, NULL, 'c'},
	{"net", required_argument, NULL, 'n'},
	{"help", no_argument, NULL, 1},
	{"version", no_argument, NULL, 2},
	{"controlsocket", required_argument, NULL, 5},
	{NULL, 0, NULL, 0}
};

static void usage(bool status) {
	if(status)
		fprintf(stderr, "Try `%s --help\' for more information.\n",
				program_name);
	else {
		printf("Usage: %s [options] command\n\n", program_name);
		printf("Valid options are:\n"
				"  -c, --config=DIR              Read configuration options from DIR.\n"
				"  -n, --net=NETNAME             Connect to net NETNAME.\n"
				"      --controlsocket=FILENAME  Open control socket at FILENAME.\n"
				"      --help                    Display this help and exit.\n"
				"      --version                 Output version information and exit.\n"
				"\n"
				"Valid commands are:\n"
				"  start                      Start tincd.\n"
				"  stop                       Stop tincd.\n"
				"  restart                    Restart tincd.\n"
				"  reload                     Reload configuration of running tincd.\n"
				"  pid                        Show PID of currently running tincd.\n"
				"  generate-keys [bits]       Generate a new public/private keypair.\n"
				"  dump                       Dump a list of one of the following things:\n"
				"    nodes                    - all known nodes in the VPN\n"
				"    edges                    - all known connections in the VPN\n"
				"    subnets                  - all known subnets in the VPN\n"
				"    connections              - all meta connections with ourself\n"
				"    graph                    - graph of the VPN in dotty format\n"
				"  purge                      Purge unreachable nodes\n"
				"  debug N                    Set debug level\n"
				"  retry                      Retry all outgoing connections\n"
				"  reload                     Partial reload of configuration\n"
				"\n");
		printf("Report bugs to tinc@tinc-vpn.org.\n");
	}
}

static bool parse_options(int argc, char **argv) {
	int r;
	int option_index = 0;

	while((r = getopt_long(argc, argv, "c:n:", long_options, &option_index)) != EOF) {
		switch (r) {
			case 0:				/* long option */
				break;

			case 'c':				/* config file */
				confbase = xstrdup(optarg);
				break;

			case 'n':				/* net name given */
				netname = xstrdup(optarg);
				break;

			case 1:					/* show help */
				show_help = true;
				break;

			case 2:					/* show version */
				show_version = true;
				break;

			case 5:					/* open control socket here */
				controlsocketname = xstrdup(optarg);
				break;

			case '?':
				usage(true);
				return false;

			default:
				break;
		}
	}

	return true;
}

FILE *ask_and_open(const char *filename, const char *what, const char *mode) {
	FILE *r;
	char *directory;
	char buf[PATH_MAX];
	char buf2[PATH_MAX];
	size_t len;

	/* Check stdin and stdout */
	if(isatty(0) && isatty(1)) {
		/* Ask for a file and/or directory name. */
		fprintf(stdout, "Please enter a file to save %s to [%s]: ",
				what, filename);
		fflush(stdout);

		if(fgets(buf, sizeof buf, stdin) < 0) {
			fprintf(stderr, "Error while reading stdin: %s\n",
					strerror(errno));
			return NULL;
		}

		len = strlen(buf);
		if(len)
			buf[--len] = 0;

		if(len)
			filename = buf;
	}

#ifdef HAVE_MINGW
	if(filename[0] != '\\' && filename[0] != '/' && !strchr(filename, ':')) {
#else
	if(filename[0] != '/') {
#endif
		/* The directory is a relative path or a filename. */
		directory = get_current_dir_name();
		snprintf(buf2, sizeof buf2, "%s/%s", directory, filename);
		filename = buf2;
	}

	umask(0077);				/* Disallow everything for group and other */

	/* Open it first to keep the inode busy */

	r = fopen(filename, mode);

	if(!r) {
		fprintf(stderr, "Error opening file `%s': %s\n", filename, strerror(errno));
		return NULL;
	}

	return r;
}

/*
  Generate a public/private RSA keypair, and ask for a file to store
  them in.
*/
static bool keygen(int bits) {
	rsa_t key;
	FILE *f;
	char *name = NULL;
	char *filename;

	fprintf(stderr, "Generating %d bits keys:\n", bits);

	if(!rsa_generate(&key, bits, 0x10001)) {
		fprintf(stderr, "Error during key generation!\n");
		return false;
	} else
		fprintf(stderr, "Done.\n");

	xasprintf(&filename, "%s/rsa_key.priv", confbase);
	f = ask_and_open(filename, "private RSA key", "a");

	if(!f)
		return false;
  
#ifdef HAVE_FCHMOD
	/* Make it unreadable for others. */
	fchmod(fileno(f), 0600);
#endif
		
	if(ftell(f))
		fprintf(stderr, "Appending key to existing contents.\nMake sure only one key is stored in the file.\n");

	rsa_write_pem_private_key(&key, f);

	fclose(f);
	free(filename);

	if(name)
		xasprintf(&filename, "%s/hosts/%s", confbase, name);
	else
		xasprintf(&filename, "%s/rsa_key.pub", confbase);

	f = ask_and_open(filename, "public RSA key", "a");

	if(!f)
		return false;

	if(ftell(f))
		fprintf(stderr, "Appending key to existing contents.\nMake sure only one key is stored in the file.\n");

	rsa_write_pem_public_key(&key, f);

	fclose(f);
	free(filename);

	return true;
}

/*
  Set all files and paths according to netname
*/
static void make_names(void) {
#ifdef HAVE_MINGW
	HKEY key;
	char installdir[1024] = "";
	long len = sizeof installdir;
#endif

	if(netname)
		xasprintf(&identname, "tinc.%s", netname);
	else
		identname = xstrdup("tinc");

#ifdef HAVE_MINGW
	if(!RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\tinc", 0, KEY_READ, &key)) {
		if(!RegQueryValueEx(key, NULL, 0, 0, installdir, &len)) {
			if(!confbase) {
				if(netname)
					xasprintf(&confbase, "%s/%s", installdir, netname);
				else
					xasprintf(&confbase, "%s", installdir);
			}
		}
		RegCloseKey(key);
		if(*installdir)
			return;
	}
#endif

	if(!controlsocketname)
		xasprintf(&controlsocketname, "%s/run/%s.control/socket", LOCALSTATEDIR, identname);

	if(netname) {
		if(!confbase)
			xasprintf(&confbase, CONFDIR "/tinc/%s", netname);
		else
			fprintf(stderr, "Both netname and configuration directory given, using the latter...\n");
	} else {
		if(!confbase)
			xasprintf(&confbase, CONFDIR "/tinc");
	}
}

static int fullread(int fd, void *data, size_t datalen) {
	int rv, len = 0;

	while(len < datalen) {
		rv = read(fd, data + len, datalen - len);
		if(rv == -1 && errno == EINTR)
			continue;
		else if(rv == -1)
			return rv;
		else if(rv == 0) {
#ifdef HAVE_MINGW
			errno = 0;
#else
			errno = ENODATA;
#endif
			return -1;
		}
		len += rv;
	}
	return 0;
}

/*
   Send a request (raw)
*/
static int send_ctl_request(int fd, enum request_type type,
						   void const *outdata, size_t outdatalen,
						   int *res_errno_p, void **indata_p,
						   size_t *indatalen_p) {
	tinc_ctl_request_t req;
	int rv;
	void *indata;

	memset(&req, 0, sizeof req);
	req.length = sizeof req + outdatalen;
	req.type = type;
	req.res_errno = 0;

#ifdef HAVE_MINGW
	if(send(fd, (void *)&req, sizeof req, 0) != sizeof req || send(fd, outdata, outdatalen, 0) != outdatalen)
		return -1;
#else
	struct iovec vector[2] = {
		{&req, sizeof req},
		{(void*) outdata, outdatalen}
	};

	if(res_errno_p == NULL)
		return -1;

	while((rv = writev(fd, vector, 2)) == -1 && errno == EINTR) ;
	if(rv != req.length)
		return -1;
#endif

	if(fullread(fd, &req, sizeof req) == -1)
		return -1;

	if(req.length < sizeof req) {
		errno = EINVAL;
		return -1;
	}

	if(req.length > sizeof req) {
		if(indata_p == NULL) {
			errno = EINVAL;
			return -1;
		}

		indata = xmalloc(req.length - sizeof req);

		if(fullread(fd, indata, req.length - sizeof req) == -1) {
			free(indata);
			return -1;
		}

		*indata_p = indata;
		if(indatalen_p != NULL)
			*indatalen_p = req.length - sizeof req;
	}

	*res_errno_p = req.res_errno;

	return 0;
}

/*
   Send a request (with printfs)
*/
static int send_ctl_request_cooked(int fd, enum request_type type, void const *outdata, size_t outdatalen) {
	int res_errno = -1;
	char *buf = NULL;
	size_t buflen = 0;

	if(send_ctl_request(fd, type, outdata, outdatalen, &res_errno,
						(void**) &buf, &buflen)) {
		fprintf(stderr, "Error sending request: %s\n", strerror(errno));
		return -1;
	}

	if(buf != NULL) {
		printf("%*s", (int)buflen, buf);
		free(buf);
	}

	if(res_errno != 0) {
		fprintf(stderr, "Server reported error: %s\n", strerror(res_errno));
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[], char *envp[]) {
	tinc_ctl_greeting_t greeting;
	int fd;
	int result;

	program_name = argv[0];

	if(!parse_options(argc, argv))
		return 1;
	
	make_names();

	if(show_version) {
		printf("%s version %s (built %s %s, protocol %d)\n", PACKAGE,
			   VERSION, __DATE__, __TIME__, PROT_CURRENT);
		printf("Copyright (C) 1998-2009 Ivo Timmermans, Guus Sliepen and others.\n"
				"See the AUTHORS file for a complete list.\n\n"
				"tinc comes with ABSOLUTELY NO WARRANTY.  This is free software,\n"
				"and you are welcome to redistribute it under certain conditions;\n"
				"see the file COPYING for details.\n");

		return 0;
	}

	if(show_help) {
		usage(false);
		return 0;
	}

	if(optind >= argc) {
		fprintf(stderr, "Not enough arguments.\n");
		usage(true);
		return 1;
	}

	// First handle commands that don't involve connecting to a running tinc daemon.

	if(!strcasecmp(argv[optind], "generate-keys")) {
		return !keygen(optind > argc ? atoi(argv[optind + 1]) : 2048);
	}

	if(!strcasecmp(argv[optind], "start")) {
		argv[optind] = NULL;
		execve(SBINDIR "/tincd", argv, envp);
		fprintf(stderr, "Could not start tincd: %s", strerror(errno));
		return 1;
	}

	/*
	 * Now handle commands that do involve connecting to a running tinc daemon.
	 * Authenticate the server by ensuring the parent directory can be
	 * traversed only by root. Note this is not totally race-free unless all
	 * ancestors are writable only by trusted users, which we don't verify.
	 */

#ifdef HAVE_MINGW
	if(WSAStartup(MAKEWORD(2, 2), &wsa_state)) {
		fprintf(stderr, "System call `%s' failed: %s", "WSAStartup", winerror(GetLastError()));
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001);
	addr.sin_port = htons(55555);

	fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(fd < 0) {
		fprintf(stderr, "Cannot create TCP socket: %s\n", sockstrerror(sockerrno));
		return 1;
	}

	fprintf(stderr, "Got socket %d\n", fd);

	unsigned long arg = 0;

	if(ioctlsocket(fd, FIONBIO, &arg) != 0) {
		fprintf(stderr, "ioctlsocket failed: %s", sockstrerror(sockerrno));
	}
#else
	struct sockaddr_un addr;
	struct stat statbuf;
	char *lastslash = strrchr(controlsocketname, '/');
	if(lastslash != NULL) {
		/* control socket is not in cwd; stat its parent */
		*lastslash = 0;
		result = stat(controlsocketname, &statbuf);
		*lastslash = '/';
	} else
		result = stat(".", &statbuf);

	if(result < 0) {
		fprintf(stderr, "Unable to check control socket directory permissions: %s\n", strerror(errno));
		return 1;
	}

	if(statbuf.st_uid != 0 || (statbuf.st_mode & S_IXOTH) != 0 || (statbuf.st_gid != 0 && (statbuf.st_mode & S_IXGRP)) != 0) {
		fprintf(stderr, "Insecure permissions on control socket directory\n");
		return 1;
	}

	if(strlen(controlsocketname) >= sizeof addr.sun_path) {
		fprintf(stderr, "Control socket filename too long!\n");
		return 1;
	}

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if(fd < 0) {
		fprintf(stderr, "Cannot create UNIX socket: %s\n", strerror(errno));
		return 1;
	}

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, controlsocketname, sizeof addr.sun_path - 1);
#endif

	if(connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
			
		fprintf(stderr, "Cannot connect to %s: %s\n", controlsocketname, sockstrerror(sockerrno));
		return 1;
	}

	fprintf(stderr, "Connected!\n");

	if(fullread(fd, &greeting, sizeof greeting) == -1) {
		fprintf(stderr, "Cannot read greeting from control socket: %s\n",
				sockstrerror(sockerrno));
		return 1;
	}

	if(greeting.version != TINC_CTL_VERSION_CURRENT) {
		fprintf(stderr, "Version mismatch: server %d, client %d\n",
				greeting.version, TINC_CTL_VERSION_CURRENT);
		return 1;
	}

	if(!strcasecmp(argv[optind], "pid")) {
		printf("%d\n", greeting.pid);
		return 0;
	}

	if(!strcasecmp(argv[optind], "stop")) {
		return send_ctl_request_cooked(fd, REQ_STOP, NULL, 0) != -1;
	}

	if(!strcasecmp(argv[optind], "reload")) {
		return send_ctl_request_cooked(fd, REQ_RELOAD, NULL, 0) != -1;
	}
	
	if(!strcasecmp(argv[optind], "restart")) {
		return send_ctl_request_cooked(fd, REQ_RESTART, NULL, 0) != -1;
	}

	if(!strcasecmp(argv[optind], "dump")) {
		if(argc < optind + 2) {
			fprintf(stderr, "Not enough arguments.\n");
			usage(true);
			return 1;
		}

		if(!strcasecmp(argv[optind+1], "nodes")) {
			return send_ctl_request_cooked(fd, REQ_DUMP_NODES, NULL, 0) != -1;
		}

		if(!strcasecmp(argv[optind+1], "edges")) {
			return send_ctl_request_cooked(fd, REQ_DUMP_EDGES, NULL, 0) != -1;
		}

		if(!strcasecmp(argv[optind+1], "subnets")) {
			return send_ctl_request_cooked(fd, REQ_DUMP_SUBNETS, NULL, 0) != -1;
		}

		if(!strcasecmp(argv[optind+1], "connections")) {
			return send_ctl_request_cooked(fd, REQ_DUMP_CONNECTIONS, NULL, 0) != -1;
		}

		if(!strcasecmp(argv[optind+1], "graph")) {
			return send_ctl_request_cooked(fd, REQ_DUMP_GRAPH, NULL, 0) != -1;
		}

		fprintf(stderr, "Unknown dump type '%s'.\n", argv[optind+1]);
		usage(true);
		return 1;
	}

	if(!strcasecmp(argv[optind], "purge")) {
		return send_ctl_request_cooked(fd, REQ_PURGE, NULL, 0) != -1;
	}

	if(!strcasecmp(argv[optind], "debug")) {
		int debuglevel;

		if(argc != optind + 2) {
			fprintf(stderr, "Invalid arguments.\n");
			return 1;
		}
		debuglevel = atoi(argv[optind+1]);
		return send_ctl_request_cooked(fd, REQ_SET_DEBUG, &debuglevel,
									   sizeof debuglevel) != -1;
	}

	if(!strcasecmp(argv[optind], "retry")) {
		return send_ctl_request_cooked(fd, REQ_RETRY, NULL, 0) != -1;
	}

	if(!strcasecmp(argv[optind], "reload")) {
		return send_ctl_request_cooked(fd, REQ_RELOAD, NULL, 0) != -1;
	}

	fprintf(stderr, "Unknown command `%s'.\n", argv[optind]);
	usage(true);
	
	close(fd);

	return 0;
}