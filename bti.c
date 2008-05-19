/*
 * Copyright (C) 2008 Greg Kroah-Hartman <greg@kroah.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include "bti_version.h"


#define zalloc(size)	calloc(size, 1)

#define dbg(format, arg...)						\
	do {								\
		if (debug)						\
			printf("%s: " format , __func__ , ## arg );	\
	} while (0)


static int debug = 0;

struct session {
	char *password;
	char *account;
	char *tweet;
	int quiet;
};

struct bti_curl_buffer {
	char *data;
	int length;
};

static void display_help(void)
{
	fprintf(stdout, "bti - send tweet to twitter\n");
	fprintf(stdout, "Version: " BTI_VERSION "\n");
	fprintf(stdout, "Usage:\n");
	fprintf(stdout, "  bti [options]\n");
	fprintf(stdout, "options are:\n");
	fprintf(stdout, "  --account accountname\n");
	fprintf(stdout, "  --password password\n");
	fprintf(stdout, "  --bash\n");
	fprintf(stdout, "  --quiet\n");
	fprintf(stdout, "  --debug\n");
	fprintf(stdout, "  --help\n");
}

static char *get_string_from_stdin(void)
{
	char *temp;
	char *string;

	string = zalloc(100);
	if (!string)
		return NULL;

	if (!fgets(string, 99, stdin))
		return NULL;
	temp = strchr(string, '\n');
	*temp = '\0';
	return string;
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
	free(session);
}

static struct bti_curl_buffer *bti_curl_buffer_alloc(void)
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
	return buffer;
}

static void bti_curl_buffer_free(struct bti_curl_buffer *buffer)
{
	if (!buffer)
		return;
	free(buffer->data);
	free(buffer);
}

static const char *twitter_url = "https://twitter.com/statuses/update.xml";

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

size_t curl_callback(void *buffer, size_t size, size_t nmemb, void *userp)
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

	dbg("%s\n", curl_buf->data);

	return buffer_size;
}

static int send_tweet(struct session *session)
{
	char user_password[500];
	char data[500];
	struct bti_curl_buffer *curl_buf;
	CURL *curl = NULL;
	CURLcode res;
	struct curl_httppost *formpost = NULL;
	struct curl_httppost *lastptr = NULL;

	if (!session)
		return -EINVAL;

	curl_buf = bti_curl_buffer_alloc();
	if (!curl_buf)
		return -ENOMEM;

	snprintf(user_password, sizeof(user_password), "%s:%s",
		 session->account, session->password);
	snprintf(data, sizeof(data), "status=\"%s\"", session->tweet);

	curl = curl_init();
	if (!curl)
		return -EINVAL;

	curl_formadd(&formpost, &lastptr,
		     CURLFORM_COPYNAME, "status",
		     CURLFORM_COPYCONTENTS, session->tweet,
		     CURLFORM_END);

	curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
	curl_easy_setopt(curl, CURLOPT_URL, twitter_url);
	if (debug)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_USERPWD, user_password);

	dbg("user_password = %s\n", user_password);
	dbg("data = %s\n", data);

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, curl_buf);
	res = curl_easy_perform(curl);
	if (res) {
		printf("error(%d) trying to send tweet\n", res);
		return -EINVAL;
	}

	curl_easy_cleanup(curl);
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
	char *file;
	char *home = getenv("HOME");

	/* config file is ~/.bti  */
	file = alloca(strlen(home) + 7);

	sprintf(file, "%s/.bti", home);

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
		}
	} while (!feof(config_file));

	if (password)
		session->password = password;
	if (account)
		session->account= account;

	/* Free buffer and close file.  */
	free(line);
	fclose(config_file);
}

int main(int argc, char *argv[], char *envp[])
{
	static const struct option options[] = {
		{ "debug", 0, NULL, 'd' },
		{ "account", 1, NULL, 'a' },
		{ "password", 1, NULL, 'p' },
		{ "bash", 1, NULL, 'b' },
		{ "help", 0, NULL, 'h' },
		{ "quiet", 0, NULL, 'q' },
		{ }
	};
	struct session *session;
	char *tweet;
	int retval;
	int option;

	session = session_alloc();
	if (!session) {
		fprintf(stderr, "no more memory...\n");
		return -1;
	}

	curl_global_init(CURL_GLOBAL_ALL);
	parse_configfile(session);

	while (1) {
		option = getopt_long_only(argc, argv, "dqe:p:a:h",
					  options, NULL);
		if (option == -1)
			break;
		switch (option) {
		case 'd':
			debug = 1;
			break;
		case 'a':
			if (session->account)
				free(session->account);
			session->account = strdup(optarg);
			dbg("account = %s\n", session->account);
			break;
		case 'p':
			if (session->password)
				free(session->password);
			session->password = strdup(optarg);
			dbg("password = %s\n", session->password);
			break;
		case 'q':
			session->quiet = 1;
			break;
		case 'h':
			display_help();
			goto exit;
		default:
			display_help();
			goto exit;
		}
	}

	if (!session->account) {
		fprintf(stdout, "Enter twitter account: ");
		session->account = get_string_from_stdin();
	}

	if (!session->password) {
		fprintf(stdout, "Enter twitter password: ");
		session->password = get_string_from_stdin();
	}

	/* Add the "$ " to the start of the tweet to show it's coming from
	 * a shell */
	tweet = get_string_from_stdin();
	session->tweet = zalloc(strlen(tweet) + 10);
	sprintf(session->tweet, "$ %s", tweet);
	free(tweet);

	if (strlen(session->tweet) == 0) {
		dbg("no tweet?\n");
		return -1;
	}

	dbg("account = %s\n", session->account);
	dbg("password = %s\n", session->password);
	dbg("tweet = %s\n", session->tweet);

	retval = send_tweet(session);
	if (retval) {
		fprintf(stderr, "tweet failed\n");
		return -1;
	}
	//printf("tweet = %s\n", session->tweet);

	session_free(session);
exit:
	return 0;
}
