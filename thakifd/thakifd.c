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
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/queue.h>

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

typedef struct userq userq_t;
typedef TAILQ_HEAD(uhead, userq) uinr_head_t;

typedef enum {
	JOIN_EVENT = 1,
	LEAVE_EVENT
} thakifd_event_t;

typedef struct thakifd_cmd_s {
	int   id;
	int   nargs;
	char  name[STRSIZE];
	char *desc;
	void (*handler)(void);
} thakifd_cmd_t;

typedef struct thakifd_resp_s {
	int   id;
	char  name[STRSIZE];
	char *desc;
} thakifd_resp_t;

typedef enum {
	THAKIFD_CONNECTED = 1,
	THAKIFD_JOINED,
	THAKIFD_CLOSING
} thakifd_client_state_t;

typedef struct thakifd_client_s {
	int             fd;
	int             state;
	int             wptr;
	int             rptr;
	char            rbuf[BUFSIZE];
	char            wbuf[BUFSIZE];
	thakifd_user_t   *user;
	thakifd_server_t *server;
} thakifd_client_t;

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
	int verbose_flag;
	int help_flag;
	int cli_err;
	int bg_flag;
	int dir_flag;
	int port;
} thakifd_cli_t;

thakifd_cli_t cliopts;

static struct option long_options[] = {
	{ "verbose",  no_argument,       &cliopts.verbose_flag, 1},
	{ "bg",       no_argument,       &cliopts.bg_flag,      1},
	{ "dir",      required_argument, &cliopts.dir_flag,     1},
	{ "help",     no_argument,       &cliopts.help_flag,    1},
	{ 0, 0, 0, 0}
};

static void handle_user (void);
static void handle_pass (void);
static void handle_acct (void);
static void handle_cwd  (void);
static void handle_cdup (void);
static void handle_smnt (void);
static void handle_quit (void);
static void handle_rein (void);
static void handle_port (void);
static void handle_pasv (void);
static void handle_type (void);
static void handle_stru (void);
static void handle_mode (void);
static void handle_retr (void);
static void handle_stor (void);
static void handle_stou (void);
static void handle_appe (void);
static void handle_allo (void);
static void handle_rest (void);
static void handle_rnfr (void);
static void handle_rnto (void);
static void handle_abor (void); 
static void handle_dele (void);
static void handle_rmd  (void);
static void handle_mkd  (void);
static void handle_pwd  (void);
static void handle_list (void);
static void handle_nlst (void);
static void handle_site (void);
static void handle_syst (void);
static void handle_stat (void);
static void handle_help (void); 
static void handle_noop (void);

static thakifd_cmd_t ftp_cmds[] = {
    { 1, 1, "USER", " ", handle_user },
    { 2, 1, "PASS", " ", handle_pass },
    { 3, 1, "ACCT", " ", handle_acct },
    { 4, 1, "CWD",  " ", handle_cwd  },
    { 5, 1, "CDUP", " ", handle_cdup },
    { 6, 1, "SMNT", " ", handle_smnt },
    { 7, 1, "QUIT", " ", handle_quit },
    { 8, 1, "REIN", " ", handle_rein },
    { 9, 1, "PORT", " ", handle_port },
    {10, 1, "PASV", " ", handle_pasv },
    {11, 1, "TYPE", " ", handle_type },
    {12, 1, "STRU", " ", handle_stru },
    {13, 1, "MODE", " ", handle_mode },
    {14, 1, "RETR", " ", handle_retr },
    {15, 1, "STOR", " ", handle_stor },
    {16, 1, "STOU", " ", handle_stou },
    {17, 1, "APPE", " ", handle_appe },
    {18, 1, "ALLO", " ", handle_allo },
    {19, 1, "REST", " ", handle_rest },
    {20, 1, "RNFR", " ", handle_rnfr },
    {21, 1, "RNTO", " ", handle_rnto },
    {22, 1, "ABOR", " ", handle_abor }, 
    {23, 1, "DELE", " ", handle_dele },
    {24, 1, "RMD",  " ", handle_rmd  },
    {25, 1, "MKD",  " ", handle_mkd  },
    {26, 1, "PWD",  " ", handle_pwd  },
    {27, 1, "LIST", " ", handle_list },
    {28, 1, "NLST", " ", handle_nlst },
    {29, 1, "SITE", " ", handle_site },
    {30, 1, "SYST", " ", handle_syst },
    {31, 1, "STAT", " ", handle_stat },
    {32, 1, "HELP", " ", handle_help }, 
    {33, 1, "NOOP", " ", handle_noop }, 
};

#define NUM_FTP_CMDS (sizeof(ftp_cmds)/sizeof(thakifd_cmd_t));

/* DJB2 algo to hash strings */
unsigned long
hash(char *str)
{
    unsigned long hash = 5381;
    int c;

    while ((c = *str++) != 0)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

thakifd_status_t
thakifd_start(thakifd_server_t *srvr)
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
thakifd_addto_epoll(thakifd_client_t *client)
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
thakifd_accept(thakifd_server_t *server)
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
get_error_msg(void)
{
	return "ERROR\n";
}

thakifd_status_t
thakifd_send_msg(thakifd_client_t *client, char *msg)
{
	write(client->fd, msg, strlen(msg));
	return SUCCESS;
}

thakifd_status_t
thakifd_bcast_event(thakifd_event_t event, thakifd_user_t *usr)
{
	userq_t *u;
	char wbuf[BUFSIZE];

	memset(wbuf, 0, sizeof(wbuf));
	sprintf(wbuf, "%s has joined\n", usr->name);


	return SUCCESS;
}

thakifd_status_t
thakifd_join_room(thakifd_client_t *client, char *user, char *room)
{
	thakifd_user_t *u;
	userq_t      *uinr;
	int uid, rid;

	uid = hash(user) % MAX_USERS;

	u = client->server->userslist[uid];
	fprintf(stderr, "Uid:%d,u:%p\n", uid, u);
	if (u == NULL) {
		u = (thakifd_user_t *)malloc(sizeof(thakifd_user_t));
		if (u == NULL) {
			fprintf(stderr, "Failed to allocate memory for user\n");
			return FAILURE;
		}
		memset(u, 0, sizeof(thakifd_user_t));
	} else {
		fprintf(stderr, "Found user %s\n", u->name);
	} 
	/* TODO else handle hash chains */
	memcpy(u->name, user, strlen(user));
	u->client = client;
	client->user = u;
	client->state = THAKIFD_JOINED;
	uinr = (userq_t *)malloc(sizeof(userq_t));
	memset(uinr, 0, sizeof(userq_t));
	uinr->usr = u;

	client->server->userslist[uid] = u;
	thakifd_bcast_event(JOIN_EVENT, u);
	return SUCCESS;
}

//#define BUFSTART (client->rbuf + client->rptr)

thakifd_status_t
handle_join(thakifd_client_t *client)
{
	if (RBUF_IS_READ_OK(client, 5) &&  strncasecmp(RBUF_READ_START(client), "JOIN ", 5) == 0) {
		char username[STRSIZE];
		int  ri = 0, ui = -1;

		fprintf(stderr, "Found JOIN Command\n");
		client->rptr = RBUF_INCR_RPTR(client, 5);
		if (RBUF_IS_EMPTY(client)) {
			fprintf(stderr, "Bufsize for client read buffer not enough\n");
			return FAILURE;
		}
		memset(username, 0, sizeof(username));
		for (int i = client->rptr; i < client->wptr && client->rbuf[i] != '\r'; i++) {
			/* 2 valid states => read room name, user name */
			if (ui == -1 || client->rbuf[i] == ' ') {
				if (client->rbuf[i] == ' ') {
					ui++;
					continue;
				}
			} else
				username[ui++] = client->rbuf[i];
			client->rptr = RBUF_INCR_RPTR(client, 1);
		}
		username[ui] = '\0';
		if (strlen(username) > NAMESIZE) {
			fprintf(stderr, "Name sizes are too large\n");
			thakifd_send_msg(client, get_error_msg());
			return FAILURE;
		}
 		client->rptr = RBUF_INCR_RPTR(client, 1);
		/* Skip over any empty input lines */
		while (client->rbuf[client->rptr] == '\n' || client->rbuf[client->rptr] == '\r')
			client->rptr = RBUF_INCR_RPTR(client, 1);
		if (RBUF_IS_EMPTY(client)) {
			fprintf(stderr, "Read caught up\n");
		}
		return SUCCESS;
	}

	fprintf(stderr, "JOIN command expected, was not found\n");
	thakifd_send_msg(client, get_error_msg());
	/* Skip over any empty input lines */
	while (client->rbuf[client->rptr] == '\n' || client->rbuf[client->rptr] == '\r')
		client->rptr = RBUF_INCR_RPTR(client, 1);

	return FAILURE;
}

thakifd_status_t
handle_leave(thakifd_client_t *client)
{
	if (RBUF_IS_READ_OK(client, 5) &&  strncasecmp(RBUF_READ_START(client), "LEAVE", 5) == 0) {
		fprintf(stderr, "Found LEAVE Command\n");
		return SUCCESS;
	}
	/* TODO */
	return SUCCESS;
}

thakifd_status_t
handle_error(thakifd_client_t *client)
{
	/* Not implemented */
	return SUCCESS;
}

thakifd_status_t
handle_message(thakifd_client_t *client)
{
	int  ri = 0;
	char rbuf[BUFSIZE] = {0};
	char wbuf[BUFSIZE] = {0};
	userq_t *u;

	if (client->state != THAKIFD_JOINED) {
		fprintf(stderr, "Got message <%d> while unjoined\n", client->rptr);
		while (! RBUF_IS_EMPTY(client)) {
			client->rptr = RBUF_INCR_RPTR(client, 1);
		}
		//client->rptr = RBUF_INCR_RPTR(client, 1);
		return thakifd_send_msg(client, get_error_msg());
	}
	/* Send message to all users in the room */
	while (! RBUF_IS_EMPTY(client)) {
		rbuf[ri++]   = *RBUF_READ_START(client);
		client->rptr = RBUF_INCR_RPTR(client, 1);
	}
	/*if (i >= BUFSIZE-NAMESIZE) {
		fprintf(stderr, "Read message > %d\n", BUFSIZE-NAMESIZE);
		return thakifd_send_msg(client, get_error_msg());
	}*/

	//client->rptr = RBUF_INCR_RPTR(client, 1);
	/* Skip over any empty input lines */
	while (! RBUF_IS_EMPTY(client) && (client->rbuf[client->rptr] == '\n' || client->rbuf[client->rptr] == '\r'))
		client->rptr = RBUF_INCR_RPTR(client, 1);
	sprintf(wbuf, "%s : %s\n", client->user->name, rbuf);
	return SUCCESS;
}

static void
handle_user (void)
{
	printf("Handling command %s\n", "USER");
	return;
}

static void
handle_pass (void)
{
	printf("Handling command %s\n", "PASS");
	return;
}

static void
handle_acct (void)
{
	printf("Handling command %s\n", "ACCT");
	return;
}

static void
handle_cwd  (void)
{
	printf("Handling command %s\n", "CWD");
	return;
}

static void
handle_cdup (void)
{
	printf("Handling command %s\n", "CDUP");
	return;
}

static void
handle_smnt (void)
{
	printf("Handling command %s\n", "SMNT");
	return;
}

static void
handle_quit (void)
{
	printf("Handling command %s\n", "QUIT");
	return;
}

static void
handle_rein (void)
{
	printf("Handling command %s\n", "REIN");
	return;
}

static void
handle_port (void)
{
	printf("Handling command %s\n", "PORT");
	return;
}

static void
handle_pasv (void)
{
	printf("Handling command %s\n", "PASV");
	return;
}

static void
handle_type (void)
{
	printf("Handling command %s\n", "TYPE");
	return;
}

static void
handle_stru (void)
{
	printf("Handling command %s\n", "STRU");
	return;
}

static void
handle_mode (void)
{
	printf("Handling command %s\n", "MODE");
	return;
}

static void
handle_retr (void)
{
	printf("Handling command %s\n", "RETR");
	return;
}

static void
handle_stor (void)
{
	printf("Handling command %s\n", "STOR");
	return;
}

static void
handle_stou (void)
{
	printf("Handling command %s\n", "STOU");
	return;
}

static void
handle_appe (void)
{
	printf("Handling command %s\n", "APPE");
	return;
}

static void
handle_allo (void)
{
	printf("Handling command %s\n", "ALLO");
	return;
}

static void
handle_rest (void)
{
	printf("Handling command %s\n", "REST");
	return;
}

static void
handle_rnfr (void)
{
	printf("Handling command %s\n", "RNFR");
	return;
}

static void
handle_rnto (void)
{
	printf("Handling command %s\n", "RNTO");
	return;
}

static void
handle_abor (void)
{
	printf("Handling command %s\n", "ABOR");
	return;
} 

static void
handle_dele (void)
{
	printf("Handling command %s\n", "DELE");
	return;
}

static void
handle_rmd  (void)
{
	printf("Handling command %s\n", "RMD");
	return;
}

static void
handle_mkd  (void)
{
	printf("Handling command %s\n", "MKD");
	return;
}

static void
handle_pwd  (void)
{
	printf("Handling command %s\n", "PWD");
	return;
}

static void
handle_list (void)
{
	printf("Handling command %s\n", "LIST");
	return;
}

static void
handle_nlst (void)
{
	printf("Handling command %s\n", "NLST");
	return;
}
 
static void
handle_site (void)
{
	printf("Handling command %s\n", "SITE");
	return;
}

static void
handle_syst (void)
{
	printf("Handling command %s\n", "SYST");
	return;
}

static void
handle_stat (void)
{
	printf("Handling command %s\n", "STAT");
	return;
}

static void
handle_help (void)
{
	printf("Handling command %s\n", "HELP");
	return;
} 

static void
handle_noop (void)
{
	printf("Handling command %s\n", "NOOP");
	return;
}

thakifd_status_t
handle_commands(thakifd_client_t *client)
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
					(*ftp_cmds[i].handler)();
					return SUCCESS;
				}
			}
		}
		printf("Command from client not found\n");
	}
	return SUCCESS;
}

thakifd_status_t
thakifd_close(thakifd_client_t *client)
{
	//userq_t *u;
	int uid, rid;

	client->state = THAKIFD_CLOSING;

	uid = hash(client->user->name) % MAX_USERS;
	//u = client->server->userslist[uid];
	//free(u);
	client->server->userslist[uid] = NULL;
	//free(client->user);
	if (close(client->fd) == -1) {
		perror("close");
		fprintf(stderr, "failed to close connection\n");
		return FAILURE;
	}
	/* close(2) will remove it from epoll_fd */

	return SUCCESS;
}

void
thakifd_run(thakifd_server_t *server)
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
daemonize(void)
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
handle_args(int argc, char *argv[], thakifd_server_t *server)
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
thakifd_closeup(void)
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