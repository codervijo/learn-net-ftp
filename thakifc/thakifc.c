#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/queue.h>
#include <sys/utsname.h>

#include "thakifd.h"

#define BACKLOG       128
#define THAKIFD_PORT  1234
#define BUFSIZE       1024
#define STRSIZE       512
#define MAX_USERS     1024
#define NAMESIZE      20
#define MAX_EVENTS    5
#define MAX_CLIENTS   1024

typedef enum {
	FAILURE = -1,
	SUCCESS
} thakifc_status_t;

typedef struct {
	int                     fd;
	int                     port;
	int                     tmp;
	int 				    epoll_fd;
	struct thakifc_user_s   **userslist;
	struct thakifc_client_s **clients;
} thakifc_server_t;

typedef struct thakifc_user_s {
	char                    name[STRSIZE];
	int                     id;
	struct thakifc_client_s *client;
} thakifc_user_t;

struct userq {
    thakifc_user_t          *usr;
    TAILQ_ENTRY(userq)      tailq;
};

typedef struct thakifc_cmd_s {
	int                     id;
	int                     nargs;
	char                    *name;
	char                    *desc;
	void                    (*handler)(struct thakifc_client_s *client);
} thakifc_cmd_t;

typedef struct thakifc_resp_s {
	int                     id;
	int                     code;
	char                    *name;
	char                    *desc;
} thakifc_resp_t;

typedef enum {
	thakifc_CONNECTED = 1,
	thakifc_JOINED,
	thakifc_CLOSING
} thakifc_client_state_t;

typedef struct thakifc_client_s {
	int              fd;
	int              state;
	int              wptr;
	int              rptr;
	char             rbuf[BUFSIZE];
	char             wbuf[BUFSIZE];
	thakifc_user_t   *user;
	thakifc_server_t *server;
} thakifc_client_t;

typedef struct thakifc_client_cmd_s {
	int              id;
	int              nargs;
	char             *name;
	char             *desc;
} thakifc_cli_cmd_t;

typedef struct userq userq_t;
typedef TAILQ_HEAD(uhead, userq) uinr_head_t;

/* Macros to deal with read circular buffer */
#define RBUF_SIZE(c)           (sizeof(c)->rbuf)
#define RBUF_WRITE_SIZE(c)     (((c)->rptr > (c)->wptr) ? ((c)->rptr - (c)->wptr-1) : ((c)->wptr == 0 ? (RBUF_SIZE(client)-1) : (RBUF_SIZE((c)) - (c)->wptr)))
#define RBUF_WRITE_START(c)    ((c)->rbuf + (c)->wptr)
#define RBUF_READ_START(c)     ((c)->rbuf + (c)->rptr)
#define RBUF_UNREAD_SIZE(c)    (((c)->wptr > (c)->rptr) ? ((c)->wptr - (c)->rptr-1) : (RBUF_SIZE((c)) - (c)->wptr + (c)->rptr + 1))
#define RBUF_INCR_WPTR(c, l)   (((c)->wptr + (l)) % sizeof((c)->rbuf))
#define RBUF_INCR_RPTR(c, l)   (((c)->rptr + (l)) % sizeof((c)->rbuf))
#define RBUF_IS_READ_OK(c, l)  RBUF_UNREAD_SIZE((c)) >= (l) 
#define RBUF_IS_WRITE_OK(c, l) RBUF_WRITE_SIZE((c)) >= (l)
#define RBUF_IS_EMPTY(c)       ((c)->wptr == (c)->rptr)

typedef struct thakifc_cli_opts_s {
	int      verbose_flag;
	int      help_flag;
	int      cli_err;
	int      bg_flag;
	int      dir_flag;
	int      port;
} thakifc_cli_t;

thakifc_cli_t cliopts;

static struct option long_options[] = {
	{ "verbose",  no_argument,       &cliopts.verbose_flag, 1},
	{ "bg",       no_argument,       &cliopts.bg_flag,      1},
	{ "dir",      required_argument, &cliopts.dir_flag,     1},
	{ "help",     no_argument,       &cliopts.help_flag,    1},
	{ 0, 0, 0, 0}
};

static thakifc_cmd_t ftp_cmds[] = {
    { 1, 1, THAKI_FTP_CMD_USER, THAKI_FTP_DSC_USER },
    { 2, 1, THAKI_FTP_CMD_PASS, THAKI_FTP_DSC_PASS },
    { 3, 1, THAKI_FTP_CMD_ACCT, THAKI_FTP_DSC_ACCT },
    { 4, 1, THAKI_FTP_CMD_CWD,  THAKI_FTP_DSC_CWD  },
    { 5, 1, THAKI_FTP_CMD_CDUP, THAKI_FTP_DSC_CDUP },
    { 6, 1, THAKI_FTP_CMD_SMNT, THAKI_FTP_DSC_SMNT },
    { 7, 1, THAKI_FTP_CMD_QUIT, THAKI_FTP_DSC_QUIT },
    { 8, 1, THAKI_FTP_CMD_REIN, THAKI_FTP_DSC_REIN },
    { 9, 1, THAKI_FTP_CMD_PORT, THAKI_FTP_DSC_PORT },
    {10, 1, THAKI_FTP_CMD_PASV, THAKI_FTP_DSC_PASV },
    {11, 1, THAKI_FTP_CMD_TYPE, THAKI_FTP_DSC_TYPE },
    {12, 1, THAKI_FTP_CMD_STRU, THAKI_FTP_DSC_STRU },
    {13, 1, THAKI_FTP_CMD_MODE, THAKI_FTP_DSC_MODE },
    {14, 1, THAKI_FTP_CMD_RETR, THAKI_FTP_DSC_RETR },
    {15, 1, THAKI_FTP_CMD_STOR, THAKI_FTP_DSC_STOR },
    {16, 1, THAKI_FTP_CMD_STOU, THAKI_FTP_DSC_STOU },
    {17, 1, THAKI_FTP_CMD_APPE, THAKI_FTP_DSC_APPE },
    {18, 1, THAKI_FTP_CMD_ALLO, THAKI_FTP_DSC_ALLO },
    {19, 1, THAKI_FTP_CMD_REST, THAKI_FTP_DSC_REST },
    {20, 1, THAKI_FTP_CMD_RNFR, THAKI_FTP_DSC_RNFR },
    {21, 1, THAKI_FTP_CMD_RNTO, THAKI_FTP_DSC_RNTO },
    {22, 1, THAKI_FTP_CMD_ABOR, THAKI_FTP_DSC_ABOR }, 
    {23, 1, THAKI_FTP_CMD_DELE, THAKI_FTP_DSC_DELE },
    {24, 1, THAKI_FTP_CMD_RMD,  THAKI_FTP_DSC_RMD  },
    {25, 1, THAKI_FTP_CMD_MKD,  THAKI_FTP_DSC_MKD  },
    {26, 1, THAKI_FTP_CMD_PWD,  THAKI_FTP_DSC_PWD  },
    {27, 1, THAKI_FTP_CMD_LIST, THAKI_FTP_DSC_LIST },
    {28, 1, THAKI_FTP_CMD_NLST, THAKI_FTP_DSC_NLST },
    {29, 1, THAKI_FTP_CMD_SITE, THAKI_FTP_DSC_SITE },
    {30, 1, THAKI_FTP_CMD_SYST, THAKI_FTP_DSC_SYST },
    {31, 1, THAKI_FTP_CMD_STAT, THAKI_FTP_DSC_STAT },
    {32, 1, THAKI_FTP_CMD_HELP, THAKI_FTP_DSC_HELP }, 
    {33, 1, THAKI_FTP_CMD_NOOP, THAKI_FTP_DSC_NOOP }, 
};

#define NUM_FTP_CMDS (sizeof(ftp_cmds)/sizeof(ftp_cmds[0]));

static thakifc_cli_cmd_t cli_cmds[] = {
	{ 1, 1, "help",    "Print help message"   },
	{ 2, 0, "quit",    "Quit from shell"      },
	{ 3, 0, "exit",    "Quit from shell"      },
	{ 4, 0, "pwd",     "Print working dir"    },
	{ 5, 0, "ls",      "List directory"       },
	{ 6, 0, "list",    "List directory"       },
	{ 7, 0, "dir",     "List directory"       },
	{ 8, 0, "connect", "Connect to server"    },
};

#define NUM_CLI_CMDS (sizeof(cli_cmds)/sizeof(cli_cmds[0]))

#define THAKI_FTP_REPLY_INDEX_110   (0)
#define THAKI_FTP_REPLY_INDEX_120   (1)
#define THAKI_FTP_REPLY_INDEX_125   (2)
#define THAKI_FTP_REPLY_INDEX_150   (3)
#define THAKI_FTP_REPLY_INDEX_200   (4)
#define THAKI_FTP_REPLY_INDEX_202   (5)
#define THAKI_FTP_REPLY_INDEX_211   (6)
#define THAKI_FTP_REPLY_INDEX_212   (7)
#define THAKI_FTP_REPLY_INDEX_213   (8)
#define THAKI_FTP_REPLY_INDEX_214   (9)
#define THAKI_FTP_REPLY_INDEX_215   (10)
#define THAKI_FTP_REPLY_INDEX_220   (11)
#define THAKI_FTP_REPLY_INDEX_221   (12)
#define THAKI_FTP_REPLY_INDEX_225   (13)
#define THAKI_FTP_REPLY_INDEX_226   (14)
#define THAKI_FTP_REPLY_INDEX_227   (15)
#define THAKI_FTP_REPLY_INDEX_230   (16)
#define THAKI_FTP_REPLY_INDEX_250   (17)
#define THAKI_FTP_REPLY_INDEX_257   (18)
#define THAKI_FTP_REPLY_INDEX_331   (19)
#define THAKI_FTP_REPLY_INDEX_332   (20)
#define THAKI_FTP_REPLY_INDEX_350   (21)
#define THAKI_FTP_REPLY_INDEX_421   (22)
#define THAKI_FTP_REPLY_INDEX_425   (23)
#define THAKI_FTP_REPLY_INDEX_426   (24)
#define THAKI_FTP_REPLY_INDEX_450   (25)
#define THAKI_FTP_REPLY_INDEX_451   (26)
#define THAKI_FTP_REPLY_INDEX_452   (27)
#define THAKI_FTP_REPLY_INDEX_500   (28)
#define THAKI_FTP_REPLY_INDEX_501   (29)
#define THAKI_FTP_REPLY_INDEX_502   (30)
#define THAKI_FTP_REPLY_INDEX_503   (31)
#define THAKI_FTP_REPLY_INDEX_504   (32)
#define THAKI_FTP_REPLY_INDEX_530   (33)
#define THAKI_FTP_REPLY_INDEX_532   (34)
#define THAKI_FTP_REPLY_INDEX_550   (35)
#define THAKI_FTP_REPLY_INDEX_551   (36)
#define THAKI_FTP_REPLY_INDEX_552   (37)
#define THAKI_FTP_REPLY_INDEX_553   (38)

static thakifc_resp_t ftp_replies[] = {
	{ 1, 110, "Restart marker reply." , ""  },
    { 2, 120, "Service ready in nnn minutes.", ""  },
    { 3, 125, "Data connection already open; transfer starting.", "" },
    { 4, 150, "File status okay; about to open data connection.", "" },
    { 5, 200, "Command okay.", "" },
    { 6, 202, "Command not implemented, superfluous at this site.", "" },
    { 7, 211, "System status, or system help reply.", ""},
    { 8, 212, "Directory status.", "" },
    { 9, 213, "File status.", "" },
    {10, 214, "Help message.", "" },
    {11, 215, "NAME system type.", "" },
    {12, 220, "Service ready for new user.", "" },
    {13, 221, "Service closing control connection.", "" },
    {14, 225, "Data connection open; no transfer in progress.", "" },
    {15, 226, "Closing data connection.", "" },
    {16, 227, "Entering Passive Mode (h1,h2,h3,h4,p1,p2).", "" },
    {17, 230, "User logged in, proceed.", "" },
    {18, 250, "Requested file action okay, completed.", "" },
    {19, 257, "PATHNAME created.", "" },
    {20, 331, "User name okay, need password.", "" },
    {21, 332, "Need account for login.", "" },
    {22, 350, "Requested file action pending further information.", "" },
    {23, 421, "Service not available, closing control connection.", "" },
    {24, 425, "Can't open data connection.", "" },
    {25, 426, "Connection closed; transfer aborted.", "" },
    {26, 450, "Requested file action not taken.", "" },
    {27, 451, "Requested action aborted: local error in processing.", "" },
    {28, 452, "Requested action not taken.", "" },
    {29, 500, "Syntax error, command unrecognized.", "" },
    {30, 501, "Syntax error in parameters or arguments.", "" },
    {31, 502, "Command not implemented.", "" },
    {32, 503, "Bad sequence of commands.", "" },
    {33, 504, "Command not implemented for that parameter.", "" },
    {34, 530, "Not logged in.", "" },
    {35, 532, "Need account for storing files.", "" },
    {36, 550, "Requested action not taken.", "" },
    {37, 551, "Requested action aborted: page type unknown.", "" },
    {38, 552, "Requested file action aborted.", "" },
    {39, 553, "Requested action not taken.", "" },
};

#define PS1 "Thaki FTP>>"

/* DJB2 algo to hash strings */
unsigned long
hash (char *str)
{
    unsigned long hash = 5381;
    int c;

    while ((c = *str++) != 0)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

thakifc_status_t
thakifc_connect (thakifc_server_t *srvr, int sport)
{
	int                s = 0, fd = 0;
	struct sockaddr_in saddr = { 0 };
	struct epoll_event event;

	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	saddr.sin_port = htons(srvr->port);

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		fprintf(stderr, "Socket creation failed\n");
		return FAILURE;
	}

	if (srvr == NULL) {
		fprintf(stderr, "No memory to store listend FD\n");
		return FAILURE;
	}

	saddr.sin_family = AF_INET;
	saddr.sin_port   = htons(sport);
	srvr->fd  = fd;
	printf("Set socket FD:%d\n", srvr->fd);

	s = inet_pton(AF_INET, "127.0.0.1", &(saddr.sin_addr));
	if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
		fprintf(stderr, "Failed to connect to server\n");
		return FAILURE;
	}
	srvr->epoll_fd  = epoll_create1(0);
	if (srvr->epoll_fd == -1) {
		fprintf(stderr, "Epoll create failed\n");
		/* Let us continue for now */
	} else {
		memset(&event, 0, sizeof(event));
		event.events = EPOLLIN;
		event.data.fd = srvr->fd;

		int status = fcntl(srvr->fd, F_SETFL, fcntl(srvr->fd, F_GETFL, 0) | O_NONBLOCK);	
		if (status == -1){
	  		perror("calling fcntl");
	  		// handle the error.  Doesn't usually fail.
		}
		if(epoll_ctl(srvr->epoll_fd, EPOLL_CTL_ADD, srvr->fd, &event))
		{
			fprintf(stderr, "Failed to add listen_fd file descriptor to epoll\n");
			//server->epoll_fd = -1;
			//return FAILURE;
		}
	}

	return SUCCESS;
}

static void
thakifc_addto_epoll (thakifc_client_t *client)
{
	struct epoll_event event;

	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN | EPOLLET ;
	event.data.fd = client->fd;
	event.data.ptr = client;

	errno = 0;
	if(epoll_ctl(client->server->epoll_fd, EPOLL_CTL_ADD, client->fd, &event))
	{
		perror("epoll_ctl");
		fprintf(stderr, "Failed to add file descriptor to epoll\n");
		close(client->server->epoll_fd);
		client->server->epoll_fd = -1;
		//return FAILURE;
	}
}

static char *
get_error_msg (void)
{
	return "ERROR\n";
}

thakifc_status_t
thakifc_send_msg (thakifc_server_t *server, char *msg)
{
	write(server->fd, msg, strlen(msg));
	return SUCCESS;
}

thakifc_status_t
handle_error (thakifc_client_t *client)
{
	/* Not implemented */
	return SUCCESS;
}

static inline void
get_list (char *dirpath)
{
	DIR *d;
	struct dirent *de;

	d = opendir(dirpath);
	while ((de = readdir(d)) != NULL) {
		printf("%s\n", de->d_name);
	}
	return;
}

thakifc_status_t
handle_commands (thakifc_client_t *client)
{
	while (! RBUF_IS_EMPTY(client)) {
		/* Skip over any control characters */
		while (client->rbuf[client->rptr] == 0 || iscntrl(client->rbuf[client->rptr]))
			client->rptr = RBUF_INCR_RPTR(client, 1);
		for (int i = 0; i < sizeof(ftp_cmds)/sizeof(thakifc_cmd_t); i++) {
			if ((toupper((char)*RBUF_READ_START(client))) == ftp_cmds[i].name[0]) {
				int n = strlen(ftp_cmds[i].name);
				if (strncasecmp(ftp_cmds[i].name, RBUF_READ_START(client), n) == 0) {
					printf("Found command %s\n", ftp_cmds[i].name);
					client->rptr = RBUF_INCR_RPTR(client, n+1);
					while (client->rbuf[client->rptr] == 0 || client->rbuf[client->rptr] != '\n')
						client->rptr = RBUF_INCR_RPTR(client, 1);
					(*ftp_cmds[i].handler)(client);
					return SUCCESS;
				}
			}
		}
		printf("Command from client not found\n");
	}
	return SUCCESS;
}

thakifc_status_t
thakifc_close (thakifc_client_t *client)
{
	//userq_t *u;
	int uid, rid;

	client->state = thakifc_CLOSING;

	if (close(client->fd) == -1) {
		perror("close");
		fprintf(stderr, "failed to close connection\n");
		return FAILURE;
	}
	/* close(2) will remove it from epoll_fd */

	return SUCCESS;
}

void
thakifc_run (thakifc_server_t *server)
{
	int                rlen = 0;
	int 			   ecount;
	struct epoll_event events[MAX_EVENTS];

	memset(events, 0, sizeof(events));
    ecount = epoll_wait(server->epoll_fd, events, MAX_EVENTS, 3000);
    fprintf(stderr, "%d ready events\n", ecount);
    for(int i = 0; i < ecount; i++)
    {
    	thakifc_client_t *client;
		fprintf(stderr, "Reading file descriptor '%d' -- ", events[i].data.fd);
		client = events[i].data.ptr;
		if (client == NULL) {
			fprintf(stderr, "Got event on null data.ptr\n");
			continue;
		}
		if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) ||
			 (!(events[i].events & EPOLLIN))) {
				int err = thakifc_close(client);
				if (err) {
					fprintf(stderr, "failed to destroy connection\n");
					//return -1;
				}

				//free(events[i].data.ptr);
				continue;
		}

		do {
			printf("Reading for size %ld starting at rptr %d\n", RBUF_WRITE_SIZE(client), client->rptr);
			rlen = read(client->fd, RBUF_WRITE_START(client), RBUF_WRITE_SIZE(client));
			fprintf(stderr, "%d bytes read.\n", rlen);

			if (rlen == 0) {
				printf("Finished reading\n");
				thakifc_close(client);
				break;
			} else if (rlen > 0) {
				if (rlen > RBUF_WRITE_SIZE(client))
					fprintf(stderr, "read more than a safe size(unusual)%d\n", rlen);
				client->wptr = RBUF_INCR_WPTR(client, rlen);
				printf("Next wptr:%d\n", client->wptr);
			}
			/* Once joined, user may send a short message 'hi' */
			if (client->state == thakifc_JOINED || (!RBUF_IS_EMPTY(client) && RBUF_UNREAD_SIZE(client) >= 9)) {
				handle_commands(client);
				printf("rptr after handling:%d\n", client->rptr);
			}
		} while (rlen > 0);
	}
}

thakifc_status_t
usage (char *fname)
{
	fprintf(stderr, "Usage: %s --bg [--help|-h] <port>", fname);
	fprintf(stderr, "\n\nOptions explained:\n");
	fprintf(stderr, "	--bg : Start %s in background\n", fname);
	fprintf(stderr, "	--dir : Directory to store logs/stderr (use with bg)\n");
	fprintf(stderr, "	--help | -h : Print help for %s\n", fname);
	fprintf(stderr, "\n\nOptional argument : Port number for server\n");
	fprintf(stderr, "Default port if no port is in CLI options : %d\n", THAKIFD_PORT);

	return SUCCESS;
}

static void
daemonize (void)
{
	pid_t pid;

	pid = fork();
	if (pid < 0)
		exit(EXIT_FAILURE);
	if (pid > 0) {
		sleep(3);
		exit(EXIT_SUCCESS);
	}
	if (setsid() < 0)
		exit(EXIT_FAILURE);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	umask(0);
	chdir("/");
	/* close any open fds */
	int x;
	for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
		close(x);
}

thakifc_status_t
handle_args (int argc, char *argv[], thakifc_server_t *server)
{
	optind = 0;
	while (1) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "hbd:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			switch (option_index) {
			case 3:
				usage(argv[0]);
				return FAILURE;

			case 2:
		        if (optarg)
                       printf(" with arg %s", optarg);
                else
                	return FAILURE; /* Should never be here */
                break;

			case 1:
				daemonize();
				break;

			default:
				printf("Unknown long options\n");
				return FAILURE;
			}
			break;

		case 'b':
			daemonize();
			break;

		case 'd':
			printf("Using dir %s for log\n", optarg);
			break;

		case 'h':
		case '?':
		default:
			usage (argv[0]);
			return FAILURE;
		}
	}

	if (optind < argc)
    {
    	long pport; /* Potential port as argument */
    	char *endptr;

    	pport = strtol(argv[optind], &endptr, 10);
    	if ((errno == ERANGE && (pport == LONG_MAX || pport == LONG_MIN))
                             || (errno != 0 && pport == 0)) {
            perror("strtol");
            exit(EXIT_FAILURE);
        }

        if (endptr == argv[optind]) {
            fprintf(stderr, "No digits were found\n");
            exit(EXIT_FAILURE);
        }

        printf("strtol() returned %ld\n", pport);

        if (*endptr != '\0') {
        	fprintf(stderr, "Unknown characters after port number: %s\n", endptr);
        	return FAILURE;
        }
        optind++;

        if (optind < argc) {
			while (optind < argc)
				fprintf (stderr, "Unknwon argument :%s ", argv[optind++]);
			fprintf(stderr, "\n");
			return FAILURE;
		}
		server->port = pport;
    }

	return SUCCESS;
}

void
thakifc_closeup (void)
{
	#if 0
	if (userslist != NULL) /* any more cleanup */
		free(userslist);
	if (roomslist != NULL)
		free(roomslist);
	#endif
}

thakifc_status_t
handle_help (void)
{
	printf("Help from Thaki FTP client\n");
	return 0;
}

int
main(int argc, char *argv[])
{
	thakifc_server_t thakifc;
	int redo = 1;

	atexit(thakifc_closeup);

	memset(&thakifc, 0, sizeof(thakifc_server_t));
	thakifc.userslist = (thakifc_user_t **)malloc(MAX_USERS*sizeof(thakifc_user_t *));
	if (thakifc.userslist == NULL) {
		fprintf(stderr, "Failed to get memory for users and rooms\n");
		exit(-2);
	}
	memset(thakifc.userslist, 0, MAX_USERS*sizeof(thakifc_user_t *));

	thakifc.clients = (thakifc_client_t **)malloc(MAX_CLIENTS*sizeof(thakifc_client_t *));
	if (thakifc.clients == NULL) {
		fprintf(stderr, "Failed to allocate memory for client\n");
		exit(-2);
	}

	thakifc.port = THAKIFD_PORT;
	if (handle_args(argc, argv, &thakifc) == FAILURE) {
		fprintf(stderr, "Failed to parse options to continue\n");
		exit(-2);
	}

	while (1) {
		static int li = 0;
		char line[BUFSIZE];
		char buff[BUFSIZE];

		if (redo != 0) {
			fprintf(stdout, "%s ", PS1);
			fflush(stdout);
		}
		redo = 0;
		#if 0

		thakifc_run(&thakifc);
		#endif

		memset(line, 0, sizeof(line));
		while (li == 0 || line[li-1] != '\n') {
			line[li] = getchar();
			if (isspace(line[li])) {
				for (int i = 0; i < NUM_CLI_CMDS; i++) {
					int sport;
					if (li > 1 && strncmp(line, cli_cmds[i].name, li) == 0) {
						printf("Got CLI command [%s]", line);
						switch (cli_cmds[i].id) {
						case 1:
							handle_help();
							thakifc_send_msg(&thakifc, "HELP dfsgf sfgsdfg sfdgsdfgs sdfgsdfg \r\n\n");
							memset(buff, 0, sizeof(buff));
							read(thakifc.fd, &buff, sizeof(buff));
							printf("From server <%s>\n", buff);
							break;

						case 2:
						case 3:
							thakifc_send_msg(&thakifc, "ABOR");
							exit(0);

						case 8:
							sport = 1234;//atoi(line[li+2]);
							if (thakifc_connect(&thakifc, sport) == FAILURE) {
								fprintf(stderr, "Failed to start thakifc server on port %d\n", thakifc.port);
								exit(-2);
							}
							break;
						}
					}
				}
			}
			li++;
			redo = 1;
			//printf("li:%d redo:%d line:%s\n", li, redo, line);
		}
		li = 0;
		//printf("Outside CR, li:%d redo:%d line:%s\n", li, redo, line);
	}

	exit(0);
}