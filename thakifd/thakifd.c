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

#define THAKIFD_FTP_ROOT "/home/vijo/tmp"

typedef enum {
	FAILURE = -1,
	SUCCESS
} thakifd_status_t;

typedef struct {
	int                     listen_fd;
	int                     port;
	int                     tmp;
	int 				    epoll_fd;
	struct thakifd_user_s   **userslist;
	struct thakifd_client_s **clients;
} thakifd_server_t;

typedef struct thakifd_user_s {
	char                    name[STRSIZE];
	int                     id;
	struct thakifd_client_s *client;
} thakifd_user_t;

struct userq {
    thakifd_user_t          *usr;
    TAILQ_ENTRY(userq)      tailq;
};

typedef struct thakifd_cmd_s {
	int                     id;
	int                     nargs;
	char                    *name;
	char                    *desc;
	void                    (*handler)(struct thakifd_client_s *client);
} thakifd_cmd_t;

typedef struct thakifd_resp_s {
	int                     id;
	int                     code;
	char                    *name;
	char                    *desc;
} thakifd_resp_t;

typedef enum {
	THAKIFD_CONNECTED = 1,
	THAKIFD_JOINED,
	THAKIFD_CLOSING
} thakifd_client_state_t;

typedef struct thakifd_client_s {
	int              fd;
	int              state;
	int              wptr;
	int              rptr;
	char             rbuf[BUFSIZE];
	char             wbuf[BUFSIZE];
	char             rootpath[PATH_MAX];
	char             cwd[PATH_MAX];
	thakifd_user_t   *user;
	thakifd_server_t *server;
} thakifd_client_t;

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

typedef struct thakifd_cli_opts_s {
	int      verbose_flag;
	int      help_flag;
	int      cli_err;
	int      bg_flag;
	int      dir_flag;
	int      port;
} thakifd_cli_t;

thakifd_cli_t cliopts;

static struct option long_options[] = {
	{ "verbose",  no_argument,       &cliopts.verbose_flag, 1},
	{ "bg",       no_argument,       &cliopts.bg_flag,      1},
	{ "dir",      required_argument, &cliopts.dir_flag,     1},
	{ "help",     no_argument,       &cliopts.help_flag,    1},
	{ 0, 0, 0, 0}
};

static void handle_user (thakifd_client_t *client);
static void handle_pass (thakifd_client_t *client);
static void handle_acct (thakifd_client_t *client);
static void handle_cwd  (thakifd_client_t *client);
static void handle_cdup (thakifd_client_t *client);
static void handle_smnt (thakifd_client_t *client);
static void handle_quit (thakifd_client_t *client);
static void handle_rein (thakifd_client_t *client);
static void handle_port (thakifd_client_t *client);
static void handle_pasv (thakifd_client_t *client);
static void handle_type (thakifd_client_t *client);
static void handle_stru (thakifd_client_t *client);
static void handle_mode (thakifd_client_t *client);
static void handle_retr (thakifd_client_t *client);
static void handle_stor (thakifd_client_t *client);
static void handle_stou (thakifd_client_t *client);
static void handle_appe (thakifd_client_t *client);
static void handle_allo (thakifd_client_t *client);
static void handle_rest (thakifd_client_t *client);
static void handle_rnfr (thakifd_client_t *client);
static void handle_rnto (thakifd_client_t *client);
static void handle_abor (thakifd_client_t *client); 
static void handle_dele (thakifd_client_t *client);
static void handle_rmd  (thakifd_client_t *client);
static void handle_mkd  (thakifd_client_t *client);
static void handle_pwd  (thakifd_client_t *client);
static void handle_list (thakifd_client_t *client);
static void handle_nlst (thakifd_client_t *client);
static void handle_site (thakifd_client_t *client);
static void handle_syst (thakifd_client_t *client);
static void handle_stat (thakifd_client_t *client);
static void handle_help (thakifd_client_t *client); 
static void handle_noop (thakifd_client_t *client);

static thakifd_cmd_t ftp_cmds[] = {
    { 1, 1, THAKI_FTP_CMD_USER, THAKI_FTP_DSC_USER, handle_user },
    { 2, 1, THAKI_FTP_CMD_PASS, THAKI_FTP_DSC_PASS, handle_pass },
    { 3, 1, THAKI_FTP_CMD_ACCT, THAKI_FTP_DSC_ACCT, handle_acct },
    { 4, 1, THAKI_FTP_CMD_CWD,  THAKI_FTP_DSC_CWD,  handle_cwd  },
    { 5, 1, THAKI_FTP_CMD_CDUP, THAKI_FTP_DSC_CDUP, handle_cdup },
    { 6, 1, THAKI_FTP_CMD_SMNT, THAKI_FTP_DSC_SMNT, handle_smnt },
    { 7, 1, THAKI_FTP_CMD_QUIT, THAKI_FTP_DSC_QUIT, handle_quit },
    { 8, 1, THAKI_FTP_CMD_REIN, THAKI_FTP_DSC_REIN, handle_rein },
    { 9, 1, THAKI_FTP_CMD_PORT, THAKI_FTP_DSC_PORT, handle_port },
    {10, 1, THAKI_FTP_CMD_PASV, THAKI_FTP_DSC_PASV, handle_pasv },
    {11, 1, THAKI_FTP_CMD_TYPE, THAKI_FTP_DSC_TYPE, handle_type },
    {12, 1, THAKI_FTP_CMD_STRU, THAKI_FTP_DSC_STRU, handle_stru },
    {13, 1, THAKI_FTP_CMD_MODE, THAKI_FTP_DSC_MODE, handle_mode },
    {14, 1, THAKI_FTP_CMD_RETR, THAKI_FTP_DSC_RETR, handle_retr },
    {15, 1, THAKI_FTP_CMD_STOR, THAKI_FTP_DSC_STOR, handle_stor },
    {16, 1, THAKI_FTP_CMD_STOU, THAKI_FTP_DSC_STOU, handle_stou },
    {17, 1, THAKI_FTP_CMD_APPE, THAKI_FTP_DSC_APPE, handle_appe },
    {18, 1, THAKI_FTP_CMD_ALLO, THAKI_FTP_DSC_ALLO, handle_allo },
    {19, 1, THAKI_FTP_CMD_REST, THAKI_FTP_DSC_REST, handle_rest },
    {20, 1, THAKI_FTP_CMD_RNFR, THAKI_FTP_DSC_RNFR, handle_rnfr },
    {21, 1, THAKI_FTP_CMD_RNTO, THAKI_FTP_DSC_RNTO, handle_rnto },
    {22, 1, THAKI_FTP_CMD_ABOR, THAKI_FTP_DSC_ABOR, handle_abor }, 
    {23, 1, THAKI_FTP_CMD_DELE, THAKI_FTP_DSC_DELE, handle_dele },
    {24, 1, THAKI_FTP_CMD_RMD,  THAKI_FTP_DSC_RMD,  handle_rmd  },
    {25, 1, THAKI_FTP_CMD_MKD,  THAKI_FTP_DSC_MKD,  handle_mkd  },
    {26, 1, THAKI_FTP_CMD_PWD,  THAKI_FTP_DSC_PWD,  handle_pwd  },
    {27, 1, THAKI_FTP_CMD_LIST, THAKI_FTP_DSC_LIST, handle_list },
    {28, 1, THAKI_FTP_CMD_NLST, THAKI_FTP_DSC_NLST, handle_nlst },
    {29, 1, THAKI_FTP_CMD_SITE, THAKI_FTP_DSC_SITE, handle_site },
    {30, 1, THAKI_FTP_CMD_SYST, THAKI_FTP_DSC_SYST, handle_syst },
    {31, 1, THAKI_FTP_CMD_STAT, THAKI_FTP_DSC_STAT, handle_stat },
    {32, 1, THAKI_FTP_CMD_HELP, THAKI_FTP_DSC_HELP, handle_help }, 
    {33, 1, THAKI_FTP_CMD_NOOP, THAKI_FTP_DSC_NOOP, handle_noop }, 
};

#define NUM_FTP_CMDS (sizeof(ftp_cmds)/sizeof(thakifd_cmd_t));

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

static thakifd_resp_t ftp_replies[] = {
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

thakifd_status_t
thakifd_start (thakifd_server_t *srvr)
{
	int lfd = 0;
	struct sockaddr_in saddr = { 0 };
	struct epoll_event event;

	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	saddr.sin_port = htons(srvr->port);

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1) {
		perror("socket");
		fprintf(stderr, "Socket creation failed\n");
		return FAILURE;
	}
	if (bind(lfd, (struct sockaddr*)&saddr, sizeof(saddr)) == -1) {
		perror("bind");
		fprintf(stderr, "Bind failed!\n");
		return FAILURE;
	}
	if (listen(lfd, BACKLOG) == -1) {
		perror("listen");
		fprintf(stderr, "Listen failed\n");
		return FAILURE;
	}

	if (srvr == NULL) {
		fprintf(stderr, "No memory to store listend FD\n");
		return FAILURE;
	}

	srvr->listen_fd = lfd;
	srvr->epoll_fd  = epoll_create1(0);
	if (srvr->epoll_fd == -1) {
		fprintf(stderr, "Epoll create failed\n");
		/* Let us continue for now */
	} else {
		memset(&event, 0, sizeof(event));
		event.events = EPOLLIN;
		event.data.fd = srvr->listen_fd;

		int status = fcntl(srvr->listen_fd, F_SETFL, fcntl(srvr->listen_fd, F_GETFL, 0) | O_NONBLOCK);	
		if (status == -1){
	  		perror("calling fcntl");
	  		// handle the error.  Doesn't usually fail.
		}
		if(epoll_ctl(srvr->epoll_fd, EPOLL_CTL_ADD, srvr->listen_fd, &event))
		{
			fprintf(stderr, "Failed to add listen_fd file descriptor to epoll\n");
			//server->epoll_fd = -1;
			//return FAILURE;
		}
	}

	return SUCCESS;
}

static void
thakifd_addto_epoll (thakifd_client_t *client)
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

thakifd_status_t
thakifd_accept (thakifd_server_t *server)
{
	int cfd, cid;
	socklen_t clen;
	struct sockaddr_in caddr;

	clen = sizeof(caddr);
	memset(&caddr, 0, clen);
	if ((cfd = accept(server->listen_fd, (struct sockaddr*)&caddr, &clen)) == -1) {
		perror("accept");
		fprintf(stderr, "Accept failed\n");
		return FAILURE;
	}

	printf("Client connected!\n");
	server->tmp = cfd;
	cid = cfd % MAX_CLIENTS;
	fprintf(stderr, "CID for new client:%d from FD %d\n", cid, cfd);
	server->clients[cid] = (thakifd_client_t *)malloc(sizeof(thakifd_client_t));
	if (server->clients[cid] == NULL) {
		fprintf(stderr, "Failed to allocate memory for client\n");
		return FAILURE;
	}
	memset(server->clients[cid], 0, sizeof(thakifd_client_t));
	server->clients[cid]->fd = cfd;
    memcpy(server->clients[cid]->rootpath, THAKIFD_FTP_ROOT, sizeof(THAKIFD_FTP_ROOT));
    memcpy(server->clients[cid]->cwd,      THAKIFD_FTP_ROOT, sizeof(THAKIFD_FTP_ROOT));
	int status = fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);	
	if (status == -1){
  		perror("calling fcntl");
  		// handle the error.  Doesn't usually fail.
	}
	server->clients[cid]->state  = THAKIFD_CONNECTED;
	server->clients[cid]->server = server; /* not circular, i declare */

	thakifd_addto_epoll(server->clients[cid]);

	return SUCCESS;
}

static char *
get_error_msg (void)
{
	return "ERROR\n";
}

thakifd_status_t
thakifd_send_msg (thakifd_client_t *client, char *msg)
{
	write(client->fd, msg, strlen(msg));
	return SUCCESS;
}

thakifd_status_t
thakifd_send_reply (thakifd_client_t *client, int index, char *msg)
{
	thakifd_resp_t *reply;
	int l = 0;
	char wbuf[2*BUFSIZE];

	memset(wbuf, 0, sizeof(wbuf));
	reply = &ftp_replies[index];
	l = sprintf(wbuf, "%3d %s", reply->code, reply->name);
	if (msg != NULL)
		sprintf(wbuf+l, "\n%s\n", msg);
	thakifd_send_msg(client, wbuf);
}

thakifd_status_t
handle_error (thakifd_client_t *client)
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

static void
handle_user (thakifd_client_t *client)
{
	printf("Handling command %s\n", "USER");
	return;
}

static void
handle_pass (thakifd_client_t *client)
{
	printf("Handling command %s\n", "PASS");
	return;
}

static void
handle_acct (thakifd_client_t *client)
{
	printf("Handling command %s\n", "ACCT");
	return;
}

static void
handle_cwd  (thakifd_client_t *client)
{
	printf("Handling command %s\n", "CWD");
	return;
}

static void
handle_cdup (thakifd_client_t *client)
{
	printf("Handling command %s\n", "CDUP");
	if (strncmp(client->rootpath, client->cwd, strlen(client->rootpath)) == 0) {
		printf("Already on root, failed to go up from there\n");
	}
	return;
}

static void
handle_smnt (thakifd_client_t *client)
{
	printf("Handling command %s\n", "SMNT");
	return;
}

static void
handle_quit (thakifd_client_t *client)
{
	printf("Handling command %s\n", "QUIT");
	return;
}

static void
handle_rein (thakifd_client_t *client)
{
	printf("Handling command %s\n", "REIN");
	return;
}

static void
handle_port (thakifd_client_t *client)
{
	printf("Handling command %s\n", "PORT");
	return;
}

static void
handle_pasv (thakifd_client_t *client)
{
	printf("Handling command %s\n", "PASV");
	return;
}

static void
handle_type (thakifd_client_t *client)
{
	printf("Handling command %s\n", "TYPE");
	return;
}

static void
handle_stru (thakifd_client_t *client)
{
	printf("Handling command %s\n", "STRU");
	printf("Only file structure is supported\n");
	return;
}

static void
handle_mode (thakifd_client_t *client)
{
	printf("Handling command %s\n", "MODE");
	return;
}

static void
handle_retr (thakifd_client_t *client)
{
	printf("Handling command %s\n", "RETR");
	return;
}

static void
handle_stor (thakifd_client_t *client)
{
	printf("Handling command %s\n", "STOR");
	return;
}

static void
handle_stou (thakifd_client_t *client)
{
	printf("Handling command %s\n", "STOU");
	return;
}

static void
handle_appe (thakifd_client_t *client)
{
	printf("Handling command %s\n", "APPE");
	return;
}

static void
handle_allo (thakifd_client_t *client)
{
	printf("Handling command %s\n", "ALLO");
	return;
}

static void
handle_rest (thakifd_client_t *client)
{
	printf("Handling command %s\n", "REST");
	return;
}

static void
handle_rnfr (thakifd_client_t *client)
{
	printf("Handling command %s\n", "RNFR");
	return;
}

static void
handle_rnto (thakifd_client_t *client)
{
	printf("Handling command %s\n", "RNTO");
	return;
}

static void
handle_abor (thakifd_client_t *client)
{
	printf("Handling command %s\n", "ABOR");
	printf("Aborting connection to client id %d\n", client->fd);
    thakifd_send_reply(client, THAKI_FTP_REPLY_INDEX_226, NULL);
    close(client->fd);
	return;
} 

static void
handle_dele (thakifd_client_t *client)
{
	printf("Handling command %s\n", "DELE");
	return;
}

static void
handle_rmd  (thakifd_client_t *client)
{
	printf("Handling command %s\n", "RMD");
	return;
}

static void
handle_mkd  (thakifd_client_t *client)
{
	printf("Handling command %s\n", "MKD");
	return;
}

static void
handle_pwd  (thakifd_client_t *client)
{
	printf("Handling command %s\n", "PWD");
	printf("PWD is %s\n", client->cwd);
	thakifd_send_reply(client, THAKI_FTP_REPLY_INDEX_200, client->cwd);
	return;
}

static void
handle_list (thakifd_client_t *client)
{
	printf("Handling command %s\n", "LIST");
	printf("START LIST\n");
	get_list(client->cwd);
	printf("END LIST\n\n");
	return;
}

static void
handle_nlst (thakifd_client_t *client)
{
	printf("Handling command %s\n", "NLST");
	printf("START NLST\n");
	get_list(client->cwd);
	printf("END NLST\n\n");
	return;
}
 
static void
handle_site (thakifd_client_t *client)
{
	printf("Handling command %s\n", "SITE");
	return;
}

/* find out the type of operating system at the serve */
static void
handle_syst (thakifd_client_t *client)
{
	struct utsname *un;

	printf("Handling command %s\n", "SYST");
	un = malloc(sizeof(struct utsname));
	memset(un, 0, sizeof(struct utsname));
	uname(un);
	printf("OS      : %s\n", un->sysname);
	printf("Version : %s\n", un->version);
	free(un);
	return;
}

static void
handle_stat (thakifd_client_t *client)
{
	printf("Handling command %s\n", "STAT");
	return;
}

static void
handle_help (thakifd_client_t *client)
{
	printf("Handling command %s\n", "HELP");
	thakifd_send_reply(client, THAKI_FTP_REPLY_INDEX_214, "Reply from Server\n");
	return;
} 

static void
handle_noop (thakifd_client_t *client)
{
	printf("Handling command %s\n", "NOOP");
	printf("Doing-DONE NOOP for client id=%d\n", client->fd);
	thakifd_send_reply(client, THAKI_FTP_REPLY_INDEX_200, "Reply from Server\n");
	return;
}

thakifd_status_t
handle_commands (thakifd_client_t *client)
{
	while (! RBUF_IS_EMPTY(client)) {
		/* Skip over any control characters */
		while (client->rbuf[client->rptr] == 0 || iscntrl(client->rbuf[client->rptr]))
			client->rptr = RBUF_INCR_RPTR(client, 1);
		for (int i = 0; i < sizeof(ftp_cmds)/sizeof(thakifd_cmd_t); i++) {
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

thakifd_status_t
thakifd_close (thakifd_client_t *client)
{
	//userq_t *u;
	int uid, rid;

	client->state = THAKIFD_CLOSING;

	if (close(client->fd) == -1) {
		perror("close");
		fprintf(stderr, "failed to close connection\n");
		return FAILURE;
	}
	/* close(2) will remove it from epoll_fd */

	return SUCCESS;
}

void
thakifd_run (thakifd_server_t *server)
{
	int                rlen = 0;
	int 			   ecount;
	struct epoll_event events[MAX_EVENTS];

	memset(events, 0, sizeof(events));
    ecount = epoll_wait(server->epoll_fd, events, MAX_EVENTS, 3000);
    fprintf(stderr, "%d ready events\n", ecount);
    for(int i = 0; i < ecount; i++)
    {
    	thakifd_client_t *client;
		fprintf(stderr, "Reading file descriptor '%d' -- ", events[i].data.fd);
		if (events[i].data.fd == server->listen_fd) {
			if (thakifd_accept(server) == FAILURE) {
				fprintf(stderr, "Faoiled to accept connection from client \n");
			}
			continue;
		}
		client = events[i].data.ptr;
		if (client == NULL) {
			fprintf(stderr, "Got event on null data.ptr\n");
			continue;
		}
		if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) ||
			 (!(events[i].events & EPOLLIN))) {
				int err = thakifd_close(client);
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
				thakifd_close(client);
				break;
			} else if (rlen > 0) {
				if (rlen > RBUF_WRITE_SIZE(client))
					fprintf(stderr, "read more than a safe size(unusual)%d\n", rlen);
				client->wptr = RBUF_INCR_WPTR(client, rlen);
				printf("Next wptr:%d\n", client->wptr);
			}
			/* Once joined, user may send a short message 'hi' */
			if (client->state == THAKIFD_JOINED || (!RBUF_IS_EMPTY(client) && RBUF_UNREAD_SIZE(client) >= 9)) {
				handle_commands(client);
				printf("rptr after handling:%d\n", client->rptr);
			}
		} while (rlen > 0);
	}
}

thakifd_status_t
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

thakifd_status_t
handle_args (int argc, char *argv[], thakifd_server_t *server)
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
thakifd_closeup (void)
{
	#if 0
	if (userslist != NULL) /* any more cleanup */
		free(userslist);
	if (roomslist != NULL)
		free(roomslist);
	#endif
}

int
main(int argc, char *argv[])
{
	thakifd_server_t thakifd;

	atexit(thakifd_closeup);

	memset(&thakifd, 0, sizeof(thakifd_server_t));
	thakifd.userslist = (thakifd_user_t **)malloc(MAX_USERS*sizeof(thakifd_user_t *));
	if (thakifd.userslist == NULL) {
		fprintf(stderr, "Failed to get memory for users and rooms\n");
		exit(-2);
	}
	memset(thakifd.userslist, 0, MAX_USERS*sizeof(thakifd_user_t *));

	thakifd.clients = (thakifd_client_t **)malloc(MAX_CLIENTS*sizeof(thakifd_client_t *));
	if (thakifd.clients == NULL) {
		fprintf(stderr, "Failed to allocate memory for client\n");
		exit(-2);
	}

	thakifd.port = THAKIFD_PORT;
	if (handle_args(argc, argv, &thakifd) == FAILURE) {
		fprintf(stderr, "Failed to parse options to continue\n");
		exit(-2);
	}

	if (thakifd_start(&thakifd) == FAILURE) {
		fprintf(stderr, "Failed to start thakifd server on port %d\n", thakifd.port);
		exit(-2);
	}

	while (1) {
		thakifd_run(&thakifd);
	}

	exit(0);
}