/*
 * Copyright (C) 2008 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2009 Bart Trojanowski <bart@jukie.net>
 * Copyright (C) 2009 Amir Mohammad Saied <amirsaied@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <readline/readline.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <pcre.h>


#define zalloc(size)	calloc(size, 1)

#define dbg(format, arg...)						\
	do {								\
		if (debug)						\
			fprintf(stdout, "bti: %s: " format , __func__ , \
				## arg);				\
	} while (0)


static int debug;
static int verbose;

enum host {
	HOST_TWITTER  = 0,
	HOST_IDENTICA = 1,
	HOST_CUSTOM   = 2
};

enum action {
	ACTION_UPDATE  = 0,
	ACTION_FRIENDS = 1,
	ACTION_USER    = 2,
	ACTION_REPLIES = 4,
	ACTION_PUBLIC  = 8,
	ACTION_UNKNOWN = 16
};

struct session {
	char *password;
	char *account;
	char *tweet;
	char *proxy;
	char *time;
	char *homedir;
	char *logfile;
	char *user;
	char *hosturl;
	int bash;
	int shrink_urls;
	int dry_run;
	int page;
	enum host host;
	enum action action;
};

struct bti_curl_buffer {
	char *data;
	enum action action;
	int length;
};

static void display_help(void)
{
	fprintf(stdout, "bti - send tweet to twitter or identi.ca\n");
	fprintf(stdout, "Version: " VERSION "\n");
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, "  bti [options]\n");
	fprintf(stdout, "options are:\n");
	fprintf(stdout, "  --account accountname\n");
	fprintf(stdout, "  --password password\n");
	fprintf(stdout, "  --action action\n");
	fprintf(stdout, "    ('update', 'friends', 'public', 'replies' "
		"or 'user')\n");
	fprintf(stdout, "  --user screenname\n");
	fprintf(stdout, "  --proxy PROXY:PORT\n");
	fprintf(stdout, "  --host HOST\n");
	fprintf(stdout, "  --logfile logfile\n");
	fprintf(stdout, "  --shrink-urls\n");
	fprintf(stdout, "  --page PAGENUMBER\n");
	fprintf(stdout, "  --bash\n");
	fprintf(stdout, "  --debug\n");
	fprintf(stdout, "  --verbose\n");
	fprintf(stdout, "  --dry-run\n");
	fprintf(stdout, "  --version\n");
	fprintf(stdout, "  --help\n");
}

static void display_version(void)
{
	fprintf(stdout, "bti - version %s\n", VERSION);
}

static struct session *session_alloc(void)
{
	struct session *session;

	session = zalloc(sizeof(*session));
	if (!session)
		return NULL;
	return session;
}

static void session_free(struct session *session)
{
	if (!session)
		return;
	free(session->password);
	free(session->account);
	free(session->tweet);
	free(session->proxy);
	free(session->time);
	free(session->homedir);
	free(session->user);
	free(session->hosturl);
	free(session);
}

static struct bti_curl_buffer *bti_curl_buffer_alloc(enum action action)
{
	struct bti_curl_buffer *buffer;

	buffer = zalloc(sizeof(*buffer));
	if (!buffer)
		return NULL;

	/* start out with a data buffer of 1 byte to
	 * make the buffer fill logic simpler */
	buffer->data = zalloc(1);
	if (!buffer->data) {
		free(buffer);
		return NULL;
	}
	buffer->length = 0;
	buffer->action = action;
	return buffer;
}

static void bti_curl_buffer_free(struct bti_curl_buffer *buffer)
{
	if (!buffer)
		return;
	free(buffer->data);
	free(buffer);
}

static const char *twitter_host  = "https://twitter.com/statuses";
static const char *identica_host = "https://identi.ca/api/statuses";

static const char *user_uri    = "/user_timeline/";
static const char *update_uri  = "/update.xml";
static const char *public_uri  = "/public_timeline.xml";
static const char *friends_uri = "/friends_timeline.xml";
static const char *replies_uri = "/replies.xml";

static CURL *curl_init(void)
{
	CURL *curl;

	curl = curl_easy_init();
	if (!curl) {
		fprintf(stderr, "Can not init CURL!\n");
		return NULL;
	}
	/* some ssl sanity checks on the connection we are making */
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0);
	return curl;
}

static void parse_statuses(xmlDocPtr doc, xmlNodePtr current)
{
	xmlChar *text = NULL;
	xmlChar *user = NULL;
	xmlChar *created = NULL;
	xmlNodePtr userinfo;

	current = current->xmlChildrenNode;
	while (current != NULL) {
		if (current->type == XML_ELEMENT_NODE) {
			if (!xmlStrcmp(current->name, (const xmlChar *)"created_at"))
				created = xmlNodeListGetString(doc, current->xmlChildrenNode, 1);
			if (!xmlStrcmp(current->name, (const xmlChar *)"text"))
				text = xmlNodeListGetString(doc, current->xmlChildrenNode, 1);
			if (!xmlStrcmp(current->name, (const xmlChar *)"user")) {
				userinfo = current->xmlChildrenNode;
				while (userinfo != NULL) {
					if ((!xmlStrcmp(userinfo->name, (const xmlChar *)"screen_name"))) {
						if (user)
							xmlFree(user);
						user = xmlNodeListGetString(doc, userinfo->xmlChildrenNode, 1);
					}
					userinfo = userinfo->next;
				}
			}

			if (user && text && created) {
				if (verbose)
					printf("[%s] (%.16s) %s\n",
						user, created, text);
				else
					printf("[%s] %s\n",
						user, text);
				xmlFree(user);
				xmlFree(text);
				xmlFree(created);
				user = NULL;
				text = NULL;
				created = NULL;
			}
		}
		current = current->next;
	}

	return;
}

static void parse_timeline(char *document)
{
	xmlDocPtr doc;
	xmlNodePtr current;

	doc = xmlReadMemory(document, strlen(document), "timeline.xml",
			    NULL, XML_PARSE_NOERROR);
	if (doc == NULL)
		return;

	current = xmlDocGetRootElement(doc);
	if (current == NULL) {
		fprintf(stderr, "empty document\n");
		xmlFreeDoc(doc);
		return;
	}

	if (xmlStrcmp(current->name, (const xmlChar *) "statuses")) {
		fprintf(stderr, "unexpected document type\n");
		xmlFreeDoc(doc);
		return;
	}

	current = current->xmlChildrenNode;
	while (current != NULL) {
		if ((!xmlStrcmp(current->name, (const xmlChar *)"status")))
			parse_statuses(doc, current);
		current = current->next;
	}
	xmlFreeDoc(doc);

	return;
}

static size_t curl_callback(void *buffer, size_t size, size_t nmemb,
			    void *userp)
{
	struct bti_curl_buffer *curl_buf = userp;
	size_t buffer_size = size * nmemb;
	char *temp;

	if ((!buffer) || (!buffer_size) || (!curl_buf))
		return -EINVAL;

	/* add to the data we already have */
	temp = zalloc(curl_buf->length + buffer_size + 1);
	if (!temp)
		return -ENOMEM;

	memcpy(temp, curl_buf->data, curl_buf->length);
	free(curl_buf->data);
	curl_buf->data = temp;
	memcpy(&curl_buf->data[curl_buf->length], (char *)buffer, buffer_size);
	curl_buf->length += buffer_size;
	if (curl_buf->action)
		parse_timeline(curl_buf->data);

	dbg("%s\n", curl_buf->data);

	return buffer_size;
}

static int send_request(struct session *session)
{
	char endpoint[100];
	char user_password[500];
	char data[500];
	struct bti_curl_buffer *curl_buf;
	CURL *curl = NULL;
	CURLcode res;
	struct curl_httppost *formpost = NULL;
	struct curl_httppost *lastptr = NULL;
	struct curl_slist *slist = NULL;

	if (!session)
		return -EINVAL;

	curl_buf = bti_curl_buffer_alloc(session->action);
	if (!curl_buf)
		return -ENOMEM;

	curl = curl_init();
	if (!curl)
		return -EINVAL;

	switch (session->action) {
	case ACTION_UPDATE:
		snprintf(user_password, sizeof(user_password), "%s:%s",
			 session->account, session->password);
		snprintf(data, sizeof(data), "status=\"%s\"", session->tweet);
		curl_formadd(&formpost, &lastptr,
			     CURLFORM_COPYNAME, "status",
			     CURLFORM_COPYCONTENTS, session->tweet,
			     CURLFORM_END);

		curl_formadd(&formpost, &lastptr,
			     CURLFORM_COPYNAME, "source",
			     CURLFORM_COPYCONTENTS, "bti",
			     CURLFORM_END);

		curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
		slist = curl_slist_append(slist, "Expect:");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
		
		sprintf(endpoint, "%s%s", session->hosturl, update_uri);
		curl_easy_setopt(curl, CURLOPT_URL, endpoint);
		curl_easy_setopt(curl, CURLOPT_USERPWD, user_password);

		break;
	case ACTION_FRIENDS:
		snprintf(user_password, sizeof(user_password), "%s:%s",
			 session->account, session->password);
		sprintf(endpoint, "%s%s?page=%d", session->hosturl,
			friends_uri, session->page);
		curl_easy_setopt(curl, CURLOPT_URL, endpoint);
		curl_easy_setopt(curl, CURLOPT_USERPWD, user_password);

		break;
	case ACTION_USER:
		sprintf(endpoint, "%s%s%s.xml?page=%d", session->hosturl, user_uri,
				session->user, session->page);
		curl_easy_setopt(curl, CURLOPT_URL, endpoint);

		break;
	case ACTION_REPLIES:
		snprintf(user_password, sizeof(user_password), "%s:%s",
			 session->account, session->password);
		sprintf(endpoint, "%s%s?page=%d", session->hosturl, replies_uri, session->page);
		curl_easy_setopt(curl, CURLOPT_URL, endpoint);
		curl_easy_setopt(curl, CURLOPT_USERPWD, user_password);

		break;
	case ACTION_PUBLIC:
		sprintf(endpoint, "%s%s?page=%d", session->hosturl, public_uri, session->page);
		curl_easy_setopt(curl, CURLOPT_URL, endpoint);

		break;
	default:
		break;
	}

	if (session->proxy)
		curl_easy_setopt(curl, CURLOPT_PROXY, session->proxy);

	if (debug)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);

	dbg("user_password = %s\n", user_password);
	dbg("data = %s\n", data);
	dbg("proxy = %s\n", session->proxy);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, curl_buf);
	if (!session->dry_run) {
		res = curl_easy_perform(curl);
		if (res && !session->bash) {
			fprintf(stderr, "error(%d) trying to perform "
				"operation\n", res);
			return -EINVAL;
		}
	}

	curl_easy_cleanup(curl);
	if (session->action == ACTION_UPDATE)
		curl_formfree(formpost);
	bti_curl_buffer_free(curl_buf);
	return 0;
}

static void parse_configfile(struct session *session)
{
	FILE *config_file;
	char *line = NULL;
	size_t len = 0;
	char *account = NULL;
	char *password = NULL;
	char *host = NULL;
	char *proxy = NULL;
	char *logfile = NULL;
	char *action = NULL;
	char *user = NULL;
	char *file;
	int shrink_urls = 0;

	/* config file is ~/.bti  */
	file = alloca(strlen(session->homedir) + 7);

	sprintf(file, "%s/.bti", session->homedir);

	config_file = fopen(file, "r");

	/* No error if file does not exist or is unreadable.  */
	if (config_file == NULL)
		return;

	do {
		ssize_t n = getline(&line, &len, config_file);
		if (n < 0)
			break;
		if (line[n - 1] == '\n')
			line[n - 1] = '\0';
		/* Parse file.  Format is the usual value pairs:
		   account=name
		   passwort=value
		   # is a comment character
		*/
		*strchrnul(line, '#') = '\0';
		char *c = line;
		while (isspace(*c))
			c++;
		/* Ignore blank lines.  */
		if (c[0] == '\0')
			continue;

		if (!strncasecmp(c, "account", 7) && (c[7] == '=')) {
			c += 8;
			if (c[0] != '\0')
				account = strdup(c);
		} else if (!strncasecmp(c, "password", 8) &&
			   (c[8] == '=')) {
			c += 9;
			if (c[0] != '\0')
				password = strdup(c);
		} else if (!strncasecmp(c, "host", 4) &&
			   (c[4] == '=')) {
			c += 5;
			if (c[0] != '\0')
				host = strdup(c);
		} else if (!strncasecmp(c, "proxy", 5) &&
			   (c[5] == '=')) {
			c += 6;
			if (c[0] != '\0')
				proxy = strdup(c);
		} else if (!strncasecmp(c, "logfile", 7) &&
			   (c[7] == '=')) {
			c += 8;
			if (c[0] != '\0')
				logfile = strdup(c);
		} else if (!strncasecmp(c, "action", 6) &&
			   (c[6] == '=')) {
			c += 7;
			if (c[0] != '\0')
				action = strdup(c);
		} else if (!strncasecmp(c, "user", 4) &&
				(c[4] == '=')) {
			c += 5;
			if (c[0] != '\0')
				user = strdup(c);
		} else if (!strncasecmp(c, "shrink-urls", 11) &&
				(c[11] == '=')) {
			c += 12;
			if (!strncasecmp(c, "true", 4) ||
					!strncasecmp(c, "yes", 3))
				shrink_urls = 1;
		}
		else if (!strncasecmp(c, "verbose", 7) &&
				(c[7] == '=')) {
			c += 8;
			if (!strncasecmp(c, "true", 4) ||
					!strncasecmp(c, "yes", 3))
				verbose = 1;
		}	
	} while (!feof(config_file));

	if (password)
		session->password = password;
	if (account)
		session->account = account;
	if (host) {
		if (strcasecmp(host, "twitter") == 0) {
			session->host = HOST_TWITTER;
			session->hosturl = strdup(twitter_host);
		} else if (strcasecmp(host, "identica") == 0) {
			session->host = HOST_IDENTICA;
			session->hosturl = strdup(identica_host);
		} else {
			session->host = HOST_CUSTOM;
			session->hosturl = strdup(host);
		}
		free(host);
	}
	if (proxy) {
		if (session->proxy)
			free(session->proxy);
		session->proxy = proxy;
	}
	if (logfile)
		session->logfile = logfile;
	if (action) {
		if (strcasecmp(action, "update") == 0)
			session->action = ACTION_UPDATE;
		else if (strcasecmp(action, "friends") == 0)
			session->action = ACTION_FRIENDS;
		else if (strcasecmp(action, "user") == 0)
			session->action = ACTION_USER;
		else if (strcasecmp(action, "replies") == 0)
			session->action = ACTION_REPLIES;
		else if (strcasecmp(action, "public") == 0)
			session->action = ACTION_PUBLIC;
		else
			session->action = ACTION_UNKNOWN;
		free(action);
	}
	if (user)
		session->user = user;
	session->shrink_urls = shrink_urls;

	/* Free buffer and close file.  */
	free(line);
	fclose(config_file);
}

static void log_session(struct session *session, int retval)
{
	FILE *log_file;
	char *filename;
	char *host;

	/* Only log something if we have a log file set */
	if (!session->logfile)
		return;

	filename = alloca(strlen(session->homedir) +
			  strlen(session->logfile) + 3);

	sprintf(filename, "%s/%s", session->homedir, session->logfile);

	log_file = fopen(filename, "a+");
	if (log_file == NULL)
		return;
	switch (session->host) {
	case HOST_TWITTER:
		host = "twitter";
		break;
	case HOST_IDENTICA:
		host = "identi.ca";
		break;
	default:
		host = session->hosturl;
		break;
	}

	switch (session->action) {
	case ACTION_UPDATE:
		if (retval)
			fprintf(log_file, "%s: host=%s tweet failed\n",
				session->time, host);
		else
			fprintf(log_file, "%s: host=%s tweet=%s\n",
				session->time, host, session->tweet);
		break;
	case ACTION_FRIENDS:
		fprintf(log_file, "%s: host=%s retrieving friends timeline\n",
			session->time, host);
		break;
	case ACTION_USER:
		fprintf(log_file, "%s: host=%s retrieving %s's timeline\n",
			session->time, host, session->user);
		break;
	case ACTION_REPLIES:
		fprintf(log_file, "%s: host=%s retrieving replies\n",
			session->time, host);
		break;
	case ACTION_PUBLIC:
		fprintf(log_file, "%s: host=%s retrieving public timeline\n",
			session->time, host);
		break;
	default:
		break;
	}

	fclose(log_file);
}

static char *get_string_from_stdin(void)
{
	char *temp;
	char *string;

	string = zalloc(1000);
	if (!string)
		return NULL;

	if (!fgets(string, 999, stdin))
		return NULL;
	temp = strchr(string, '\n');
	*temp = '\0';
	return string;
}

static int find_urls(const char *tweet, int **pranges)
{
	/*
	 * magic obtained from
	 * http://www.geekpedia.com/KB65_How-to-validate-an-URL-using-RegEx-in-Csharp.html
	 */
	static const char *re_magic =
		"(([a-zA-Z][0-9a-zA-Z+\\-\\.]*:)/{1,3}"
		"[0-9a-zA-Z;/~?:@&=+$\\.\\-_'()%]+)"
		"(#[0-9a-zA-Z;/?:@&=+$\\.\\-_!~*'()%]+)?";
	pcre *re;
	const char *errptr;
	int erroffset;
	int ovector[10] = {0,};
	const size_t ovsize = sizeof(ovector)/sizeof(*ovector);
	int startoffset, tweetlen;
	int i, rc;
	int rbound = 10;
	int rcount = 0;
	int *ranges = malloc(sizeof(int) * rbound);

	re = pcre_compile(re_magic,
			PCRE_NO_AUTO_CAPTURE,
			&errptr, &erroffset, NULL);
	if (!re) {
		fprintf(stderr, "pcre_compile @%u: %s\n", erroffset, errptr);
		exit(1);
	}

	tweetlen = strlen(tweet);
	for (startoffset = 0; startoffset < tweetlen; ) {

		rc = pcre_exec(re, NULL, tweet, strlen(tweet), startoffset, 0,
				ovector, ovsize);
		if (rc == PCRE_ERROR_NOMATCH)
			break;

		if (rc < 0) {
			fprintf(stderr, "pcre_exec @%u: %s\n",
				erroffset, errptr);
			exit(1);
		}

		for (i = 0; i < rc; i += 2) {
			if ((rcount+2) == rbound) {
				rbound *= 2;
				ranges = realloc(ranges, sizeof(int) * rbound);
			}

			ranges[rcount++] = ovector[i];
			ranges[rcount++] = ovector[i+1];
		}

		startoffset = ovector[1];
	}

	pcre_free(re);

	*pranges = ranges;
	return rcount;
}

/**
 * bidirectional popen() call
 *
 * @param rwepipe - int array of size three
 * @param exe - program to run
 * @param argv - argument list
 * @return pid or -1 on error
 *
 * The caller passes in an array of three integers (rwepipe), on successful
 * execution it can then write to element 0 (stdin of exe), and read from
 * element 1 (stdout) and 2 (stderr).
 */
static int popenRWE(int *rwepipe, const char *exe, const char *const argv[])
{
	int in[2];
	int out[2];
	int err[2];
	int pid;
	int rc;

	rc = pipe(in);
	if (rc < 0)
		goto error_in;

	rc = pipe(out);
	if (rc < 0)
		goto error_out;

	rc = pipe(err);
	if (rc < 0)
		goto error_err;

	pid = fork();
	if (pid > 0) {
		/* parent */
		close(in[0]);
		close(out[1]);
		close(err[1]);
		rwepipe[0] = in[1];
		rwepipe[1] = out[0];
		rwepipe[2] = err[0];
		return pid;
	} else if (pid == 0) {
		/* child */
		close(in[1]);
		close(out[0]);
		close(err[0]);
		close(0);
		rc = dup(in[0]);
		close(1);
		rc = dup(out[1]);
		close(2);
		rc = dup(err[1]);

		execvp(exe, (char **)argv);
		exit(1);
	} else
		goto error_fork;

	return pid;

error_fork:
	close(err[0]);
	close(err[1]);
error_err:
	close(out[0]);
	close(out[1]);
error_out:
	close(in[0]);
	close(in[1]);
error_in:
	return -1;
}

static int pcloseRWE(int pid, int *rwepipe)
{
	int rc, status;
	close(rwepipe[0]);
	close(rwepipe[1]);
	close(rwepipe[2]);
	rc = waitpid(pid, &status, 0);
	return status;
}

static char *shrink_one_url(int *rwepipe, char *big)
{
	int biglen = strlen(big);
	char *small;
	int smalllen;
	int rc;

	rc = dprintf(rwepipe[0], "%s\n", big);
	if (rc < 0)
		return big;

	smalllen = biglen + 128;
	small = malloc(smalllen);
	if (!small)
		return big;

	rc = read(rwepipe[1], small, smalllen);
	if (rc < 0 || rc > biglen)
		goto error_free_small;

	if (strncmp(small, "http://", 7))
		goto error_free_small;

	smalllen = rc;
	while (smalllen && isspace(small[smalllen-1]))
			small[--smalllen] = 0;

	free(big);
	return small;

error_free_small:
	free(small);
	return big;
}

static char *shrink_urls(char *text)
{
	int *ranges;
	int rcount;
	int i;
	int inofs = 0;
	int outofs = 0;
	const char *const shrink_args[] = {
		"bti-shrink-urls",
		NULL
	};
	int shrink_pid;
	int shrink_pipe[3];
	int inlen = strlen(text);

	dbg("before len=%u\n", inlen);

	shrink_pid = popenRWE(shrink_pipe, shrink_args[0], shrink_args);
	if (shrink_pid < 0)
		return text;

	rcount = find_urls(text, &ranges);
	if (!rcount)
		return text;

	for (i = 0; i < rcount; i += 2) {
		int url_start = ranges[i];
		int url_end = ranges[i+1];
		int long_url_len = url_end - url_start;
		char *url = strndup(text + url_start, long_url_len);
		int short_url_len;
		int not_url_len = url_start - inofs;

		dbg("long  url[%u]: %s\n", long_url_len, url);
		url = shrink_one_url(shrink_pipe, url);
		short_url_len = url ? strlen(url) : 0;
		dbg("short url[%u]: %s\n", short_url_len, url);

		if (!url || short_url_len >= long_url_len) {
			/* The short url ended up being too long
			 * or unavailable */
			if (inofs) {
				strncpy(text + outofs, text + inofs,
						not_url_len + long_url_len);
			}
			inofs += not_url_len + long_url_len;
			outofs += not_url_len + long_url_len;

		} else {
			/* copy the unmodified block */
			strncpy(text + outofs, text + inofs, not_url_len);
			inofs += not_url_len;
			outofs += not_url_len;

			/* copy the new url */
			strncpy(text + outofs, url, short_url_len);
			inofs += long_url_len;
			outofs += short_url_len;
		}

		free(url);
	}

	/* copy the last block after the last match */
	if (inofs) {
		int tail = inlen - inofs;
		if (tail) {
			strncpy(text + outofs, text + inofs, tail);
			outofs += tail;
		}
	}

	free(ranges);

	(void)pcloseRWE(shrink_pid, shrink_pipe);

	text[outofs] = 0;
	dbg("after len=%u\n", outofs);
	return text;
}

int main(int argc, char *argv[], char *envp[])
{
	static const struct option options[] = {
		{ "debug", 0, NULL, 'd' },
		{ "verbose", 0, NULL, 'V' },
		{ "account", 1, NULL, 'a' },
		{ "password", 1, NULL, 'p' },
		{ "host", 1, NULL, 'H' },
		{ "proxy", 1, NULL, 'P' },
		{ "action", 1, NULL, 'A' },
		{ "user", 1, NULL, 'u' },
		{ "logfile", 1, NULL, 'L' },
		{ "shrink-urls", 0, NULL, 's' },
		{ "help", 0, NULL, 'h' },
		{ "bash", 0, NULL, 'b' },
		{ "dry-run", 0, NULL, 'n' },
		{ "page", 1, NULL, 'g' },
		{ "version", 0, NULL, 'v' },
		{ }
	};
	struct session *session;
	pid_t child;
	char *tweet;
	int retval = 0;
	int option;
	char *http_proxy;
	time_t t;
	int page_nr;

	debug = 0;
	verbose = 0;
	rl_bind_key('\t', rl_insert);

	session = session_alloc();
	if (!session) {
		fprintf(stderr, "no more memory...\n");
		return -1;
	}

	/* get the current time so that we can log it later */
	time(&t);
	session->time = strdup(ctime(&t));
	session->time[strlen(session->time)-1] = 0x00;

	session->homedir = strdup(getenv("HOME"));

	curl_global_init(CURL_GLOBAL_ALL);

	/* Set environment variables first, before reading command line options
	 * or config file values. */
	http_proxy = getenv("http_proxy");
	if (http_proxy) {
		if (session->proxy)
			free(session->proxy);
		session->proxy = strdup(http_proxy);
		dbg("http_proxy = %s\n", session->proxy);
	}

	parse_configfile(session);

	while (1) {
		option = getopt_long_only(argc, argv, "dp:P:H:a:A:u:hg:snVv",
					  options, NULL);
		if (option == -1)
			break;
		switch (option) {
		case 'd':
			debug = 1;
			break;
		case 'V':
			verbose = 1;
			break;
		case 'a':
			if (session->account)
				free(session->account);
			session->account = strdup(optarg);
			dbg("account = %s\n", session->account);
			break;
		case 'g':
			page_nr = atoi(optarg);
			dbg("page = %d\n", page_nr);
			session->page = page_nr;
			break;
		case 'p':
			if (session->password)
				free(session->password);
			session->password = strdup(optarg);
			dbg("password = %s\n", session->password);
			break;
		case 'P':
			if (session->proxy)
				free(session->proxy);
			session->proxy = strdup(optarg);
			dbg("proxy = %s\n", session->proxy);
			break;
		case 'A':
			if (strcasecmp(optarg, "update") == 0)
				session->action = ACTION_UPDATE;
			else if (strcasecmp(optarg, "friends") == 0)
				session->action = ACTION_FRIENDS;
			else if (strcasecmp(optarg, "user") == 0)
				session->action = ACTION_USER;
			else if (strcasecmp(optarg, "replies") == 0)
				session->action = ACTION_REPLIES;
			else if (strcasecmp(optarg, "public") == 0)
				session->action = ACTION_PUBLIC;
			else
				session->action = ACTION_UNKNOWN;
			dbg("action = %d\n", session->action);
			break;
		case 'u':
			if (session->user)
				free(session->user);
			session->user = strdup(optarg);
			dbg("user = %s\n", session->user);
			break;
		case 'L':
			if (session->logfile)
				free(session->logfile);
			session->logfile = strdup(optarg);
			dbg("logfile = %s\n", session->logfile);
			break;
		case 's':
			session->shrink_urls = 1;
			break;
		case 'H':
			if (session->hosturl)
				free(session->hosturl);
			if (strcasecmp(optarg, "twitter") == 0) {
				session->host = HOST_TWITTER;
				session->hosturl = strdup(twitter_host);
			} else if (strcasecmp(optarg, "identica") == 0) {
				session->host = HOST_IDENTICA;
				session->hosturl = strdup(identica_host);
			} else {
				session->host = HOST_CUSTOM;
				session->hosturl = strdup(optarg);
			}
			dbg("host = %d\n", session->host);
			break;
		case 'b':
			session->bash = 1;
			break;
		case 'h':
			display_help();
			goto exit;
		case 'n':
			session->dry_run = 1;
			break;
		case 'v':
			display_version();
			goto exit;
		default:
			display_help();
			goto exit;
		}
	}

	/*
	 * Show the version to make it easier to determine what
	 * is going on here
	 */
	if (debug)
		display_version();

	if (session->action == ACTION_UNKNOWN) {
		fprintf(stderr, "Unknown action, valid actions are:\n");
		fprintf(stderr, "'update', 'friends', 'public', "
			"'replies' or 'user'.\n");
		goto exit;
	}

	if (!session->account) {
		fprintf(stdout, "Enter twitter account: ");
		session->account = readline(NULL);
	}

	if (!session->password) {
		fprintf(stdout, "Enter twitter password: ");
		session->password = readline(NULL);
	}

	if (session->action == ACTION_UPDATE) {
		if (session->bash)
			tweet = get_string_from_stdin();
		else
			tweet = readline("tweet: ");
		if (!tweet || strlen(tweet) == 0) {
			dbg("no tweet?\n");
			return -1;
		}

		if (session->shrink_urls)
			tweet = shrink_urls(tweet);

		session->tweet = zalloc(strlen(tweet) + 10);
		if (session->bash)
			sprintf(session->tweet, "%c %s", getuid() ? '$' : '#', tweet);
		else
			sprintf(session->tweet, "%s", tweet);

		free(tweet);
		dbg("tweet = %s\n", session->tweet);
	}

	if (!session->user)
		session->user = strdup(session->account);

	if (session->page == 0)
		session->page = 1;
	dbg("account = %s\n", session->account);
	dbg("password = %s\n", session->password);
	dbg("host = %d\n", session->host);
	dbg("action = %d\n", session->action);

	/* fork ourself so that the main shell can get on
	 * with it's life as we try to connect and handle everything
	 */
	if (session->bash) {
		child = fork();
		if (child) {
			dbg("child is %d\n", child);
			exit(0);
		}
	}

	retval = send_request(session);
	if (retval && !session->bash)
		fprintf(stderr, "operation failed\n");

	log_session(session, retval);
exit:
	session_free(session);
	return retval;;
}
