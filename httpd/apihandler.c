/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#include <assert.h>
#include <jansson.h>
#include <glib.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <rpc/io.h>
#include <rpc/rpctypes.h>
#include <libjrpc/jrpc.h>

#include "api.h"
#include "httpd.h"
#include "log.h"

#define API_TOKEN_HEADER "X-Armadito-Token"
#define API_VERSION_HEADER "X-Armadito-Api-Version"
#define API_VERSION "armadito.v0"

#define JSON_400 "{\"code\":400, \"message\": \"Bad Request. Make sure your request has a X-Armadito-Token header and if POST request contains valid JSON\"}"
#define JSON_403 "{\"code\":403, \"message\": \"Request forbidden. Make sure your request has a User-Agent header\"}"
#define JSON_404 "{\"code\":404, \"message\": \"Not found\"}"
#define JSON_405 "{\"code\":405, \"message\": \"Method not allowed\"}"
#define JSON_415 "{\"code\":415, \"message\": \"Unsupported Media Type. Content-Type must be application/json\"}"
#define JSON_422 "{\"code\":422, \"message\": \"Unprocessable request. Make sure the JSON request is valid\"}"

struct api_handler {
	GHashTable *client_table;
	struct MHD_Response *response_400;
	struct MHD_Response *response_403;
	struct MHD_Response *response_404;
	struct MHD_Response *response_405;
	struct MHD_Response *response_415;
	struct MHD_Response *response_422;
	void *user_data;
};

static struct api_endpoint {
	const char *path;
	enum http_method accepted_methods;
	int need_token;
	process_cb_t process_cb;
	check_cb_t check_cb;
} api_endpoint_table[] = {
	{ "/register", HTTP_METHOD_GET, 0, &register_process_cb, NULL},
	{ "/unregister", HTTP_METHOD_GET, 1, &unregister_process_cb, NULL},
	{ "/ping", HTTP_METHOD_GET, 1, &ping_process_cb, NULL},
	{ "/event", HTTP_METHOD_GET, 1, &event_process_cb, NULL},
	{ "/scan", HTTP_METHOD_POST, 1, &scan_process_cb, &scan_check_cb},
	{ "/status", HTTP_METHOD_GET, 0, &status_process_cb, NULL},
	{ "/browse", HTTP_METHOD_GET, 0, &browse_process_cb, NULL},
	{ "/version", HTTP_METHOD_GET, 0, &version_process_cb, NULL},
	{ NULL, 0, 0, NULL, NULL},
};

static struct api_endpoint *get_api_endpoint(const char *path)
{
	struct api_endpoint *p;

	for (p = api_endpoint_table; p->path != NULL && strcmp(p->path, path); p++)
		;

	if (p->path != NULL)
		return p;

	return NULL;
}

const char *api_get_user_agent(struct MHD_Connection *connection)
{
	return MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_USER_AGENT);
}

static const char *api_get_content_type(struct MHD_Connection *connection)
{
	const char *s_content_type;
	char *param;

	s_content_type = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
	if (s_content_type == NULL)
		return NULL;

	param = strchr(s_content_type, ';');
	if (param != NULL && param > s_content_type) {
		size_t len = param - s_content_type;
		char *ret = malloc(len + 1);

		assert(len > 0);
		strncpy(ret, s_content_type, len);
		ret[len] = '\0';

		return ret;
	}

	return strdup(s_content_type);
}

const char *api_get_token(struct MHD_Connection *connection)
{
	const char *s_token;

	s_token = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, API_TOKEN_HEADER);

	return s_token;
}

const char *api_get_argument(struct MHD_Connection *connection, const char *key)
{
	return MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, key);
}

static int api_handler_pre_check(struct api_handler *a, struct MHD_Connection *connection,
	enum http_method method, const char *path,
	struct api_endpoint **p_endpoint, struct MHD_Response **p_error_response)
{
	struct api_endpoint *endpoint = get_api_endpoint(path);

	*p_endpoint = endpoint;

	/* return a HTTP 404 if path is not valid */
	if (endpoint == NULL) {
		log_w("request to API invalid path %s", path);
		*p_error_response = a->response_404;
		return MHD_HTTP_NOT_FOUND;
	}

	/* return a HTTP 403 forbidden if no User-Agent header */
	if (api_get_user_agent(connection) == NULL) {
		log_w("request to API path %s has no User-Agent header", path);
		*p_error_response = a->response_403;
		return MHD_HTTP_FORBIDDEN;
	}

	/* if endpoint needs token and if no token in HTTP headers, return HTTP 400 bad request */
	if (endpoint->need_token && api_get_token(connection) == NULL) {
		log_w("request to API path %s has no " API_TOKEN_HEADER " header", path);
		*p_error_response = a->response_400;
		return MHD_HTTP_BAD_REQUEST;
	}

	/* if method is not in endpoint accepted methods, return HTTP 405 method not allowed */
	if ((endpoint->accepted_methods & method) == 0) {
		log_w("method not allowed for %s", path);
		*p_error_response = a->response_405;
		return MHD_HTTP_METHOD_NOT_ALLOWED;
	}

	/* if POST, verify Content-Type and encoding and return HTTP 415 Unsupported Media Type if invalid */
	if (method == HTTP_METHOD_POST) {
		const char *content_type = api_get_content_type(connection);

		if (content_type == NULL || strcmp(content_type, "application/json") != 0) {
			log_w("invalid Content-Type %s", content_type);
			*p_error_response = a->response_415;
			if (content_type != NULL)
				free((void *)content_type);
			return MHD_HTTP_UNSUPPORTED_MEDIA_TYPE;
		}

		free((void *)content_type);
	}

	return MHD_HTTP_OK;
}

static json_t *api_parse_json_request(const char *post_data, size_t post_data_size)
{
	json_t *j_request;
	json_error_t error;

	j_request = json_loads(post_data, post_data_size, &error);

	if (j_request == NULL)
		log_w("error in JSON parsing: %s", error.text);

	return j_request;
}

static int api_queue_response(struct MHD_Connection *connection, int http_status, json_t *j_response)
{
	struct MHD_Response *response;
	const char *json_buff;
	int ret;

	json_buff = json_dumps(j_response, 0);
	response = MHD_create_response_from_buffer(strlen(json_buff), (char *)json_buff, MHD_RESPMEM_MUST_COPY);
	json_decref(j_response); /* free the json object */

	if (response == NULL)
		return MHD_NO;

	MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
	MHD_add_response_header(response, MHD_HTTP_HEADER_CONNECTION, "close");
	MHD_add_response_header(response, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
	MHD_add_response_header(response, API_VERSION_HEADER, API_VERSION);
	/* Access-Control-Allow-Headers: Origin, X-Requested-With, Content-Type, Accept */

	ret = MHD_queue_response(connection, http_status, response);
	MHD_destroy_response(response);

	return ret;
}

/* wrap the response inside a JSON error object */
static int api_queue_response_500(struct MHD_Connection *connection, json_t *j_response)
{
	json_t *j_error;

	j_error = json_object();
	json_object_set(j_error, "code", json_integer(500));
	json_object_set(j_error, "message",
		json_string("Request processing triggered an internal error"));

	if (j_response != NULL)
		json_object_set(j_error, "data", j_response);

	return api_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, j_error);
}

int api_handler_serve(struct api_handler *a, struct MHD_Connection *connection,
	enum http_method method, const char *path, const char *post_data, size_t post_data_size)
{
	struct api_endpoint *endpoint;
	struct MHD_Response *response;
	int http_status_code;
	int ret;
	json_t *j_request = NULL;
	json_t *j_response = NULL;

	log_d("request to API: path %s", path);

	http_status_code = api_handler_pre_check(a, connection, method, path, &endpoint, &response);
	if (http_status_code != MHD_HTTP_OK)
		return MHD_queue_response(connection, http_status_code, response);

	if (method == HTTP_METHOD_POST && post_data_size) {
		j_request = api_parse_json_request(post_data, post_data_size);
		if (j_request == NULL) {
			log_w("request to API path %s does not contain valid JSON", path);
			return MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, a->response_400);
		}
	}

	/* if request parameters are not valid return HTTP 422 Unprocessable Entity  */
	if (endpoint->check_cb != NULL && (*endpoint->check_cb)(connection, j_request)) {
		log_w("request to API path %s does not contain valid parameters", path);
		return MHD_queue_response(connection, MHD_HTTP_UNPROCESSABLE_ENTITY, a->response_422);
	}

	/* process request */
	ret = (*(endpoint->process_cb))(a, connection, j_request, &j_response, a->user_data);

	if (j_request != NULL)
		json_decref(j_request);

	/* if request processing failed return HTTP 500 Internal Server Error  */
	if (ret) {
		log_w("processing request to API path %s failed", path);
		/* a failed processing can create a JSON response */
		return api_queue_response_500(connection, j_response);
	}

	return api_queue_response(connection, MHD_HTTP_OK, j_response);
}

static struct MHD_Response *create_std_response(const char *json)
{
	struct MHD_Response *resp;

	resp = MHD_create_response_from_buffer(strlen(json), (char *)json, MHD_RESPMEM_PERSISTENT);
	MHD_add_response_header(resp,  MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
	MHD_add_response_header(resp, MHD_HTTP_HEADER_CONNECTION, "close");

	return resp;
}

struct api_handler *api_handler_new(void *user_data)
{
	struct api_handler *a = malloc(sizeof(struct api_handler));

	a->client_table = g_hash_table_new_full(g_str_hash, g_str_equal, (GDestroyNotify)free, (GDestroyNotify)api_client_free);

	a->response_400 = create_std_response(JSON_400);
	a->response_403 = create_std_response(JSON_403);
 	a->response_404 = create_std_response(JSON_404);
 	a->response_405 = create_std_response(JSON_405);
	a->response_415 = create_std_response(JSON_415);
	a->response_422 = create_std_response(JSON_422);

	a->user_data = user_data;

	return a;
}

int api_handler_add_client(struct api_handler *a, const char *token, struct api_client *client)
{
	if (g_hash_table_contains(a->client_table, token)) {
		log_w("API token %s already registered", token);
		return 1;
	}

	g_hash_table_insert(a->client_table, strdup(token), client);

	return 0;
}

struct api_client *api_handler_get_client(struct api_handler *a, const char *token)
{
	struct api_client *c;

	c = g_hash_table_lookup(a->client_table, token);

	if (c == NULL)
		log_w("API token %s is not registered", token);

	return c;
}

int api_handler_remove_client(struct api_handler *a, const char *token)
{
	int ret;

	ret = g_hash_table_remove(a->client_table, token);

	if (!ret) {
		log_w("API token %s is not registered", token);
		return 1;
	}

	return 0;
}

