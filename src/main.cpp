#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>

#define DORF_PORT "3500"

typedef int SOCKET;

SOCKET server_socket;

volatile long active_thread_count;

struct Server_Stats
{
	U32 snapshot_count;
	U32 snapshot_index;
	long *active_thread_counts;
	pthread_mutex_t lock;
};

Server_Stats global_stats;

struct HTTP_Status_Description {
	int status_code;
	const char *description;
} http_status_descriptions[] = {
	{ 200, "OK" },
	{ 404, "Not found" },
};

const char *get_http_status_description(int status_code) {
	for (U32 i = 0; i < Count(http_status_descriptions); i++) {
		if (http_status_descriptions[i].status_code == status_code) {
			return http_status_descriptions[i].description;
		}
	}
	return "Unknown";
}

void handle_kill(int signal)
{
	puts("server is kill");

	shutdown(server_socket, 2);
	
	exit(0);
}

struct World_Instance
{
	World *world;
	pthread_mutex_t lock;
	time_t last_updated;
};

void update_to_now(World_Instance *world_instance)
{
	int count = 0;
	time_t now = time(NULL);
	while (world_instance->last_updated < now) {
		count++;
		world_tick(world_instance->world);
		world_instance->last_updated++;
	}
}

void *thread_background_world_update(void *world_instance_ptr)
{
	World_Instance *world_instance = (World_Instance*)world_instance_ptr;

	for (;;) {
		pthread_mutex_lock(&world_instance->lock);
		update_to_now(world_instance);
		pthread_mutex_unlock(&world_instance->lock);

		sleep(10);
	}
}

void *thread_background_stat_update(void *server_stats)
{
	Server_Stats *stats = (Server_Stats*)server_stats;
	for (;;) {

		pthread_mutex_lock(&stats->lock);
		stats->active_thread_counts[stats->snapshot_index] = active_thread_count;

		stats->snapshot_index = (stats->snapshot_index + 1) % stats->snapshot_count;
		pthread_mutex_unlock(&stats->lock);

		sleep(1);
	}
}

int render_stats(Server_Stats *stats, char *body)
{
	char *ptr = body;

	ptr += sprintf(ptr, "<html><head><title>Server stats</title></head><body>");
	ptr += sprintf(ptr, "<h5>Active thread count</h5>");
	ptr += sprintf(ptr, "<svg width=\"400\" height=\"200\">\n");

	long max_thread_count = 1;
	for (U32 i = 0; i < stats->snapshot_count; i++) {
		max_thread_count = max(max_thread_count, stats->active_thread_counts[i]);
	}

	long ruler_size = (long)ceilf((float)max_thread_count / 5);
	long ruler_count = max_thread_count / ruler_size + 1;
	long graph_height = ruler_count * ruler_size;
	for (long i = 0; i <= ruler_count; i++) {
		long value = i * ruler_size;
		float y = 195.0f - (float)value / graph_height * 170.0f;
		ptr += sprintf(ptr, "<path d=\"M30 %f L400 %f\" stroke=\"#ddd\" stroke-width=\"1\""
			" fill=\"none\" />\n", y, y);
		ptr += sprintf(ptr, "<text x=\"25\" y=\"%f\" text-anchor=\"end\" "
			"fill=\"gray\">%d</text>", y + 4.0f, value);
	}

	ptr += sprintf(ptr, "<path d=\"");
	char command_char = 'M';
	for (U32 i = 0; i < stats->snapshot_count; i++) {
		int snapshot_index = (stats->snapshot_index - 1 - i + stats->snapshot_count)
			% stats->snapshot_count;

		float x = 400.0f - ((float)i / (stats->snapshot_count - 1)) * 370.0f;
		float y = 195.0f - (float)stats->active_thread_counts[snapshot_index]
			/ graph_height * 170.0f;

		ptr += sprintf(ptr, "%c%f %f ", command_char, x, y);
		command_char = 'L';
	}
	ptr += sprintf(ptr, "\" stroke=\"black\" stroke-width=\"2\" fill=\"none\" />\n");
	ptr += sprintf(ptr, "</svg>");

	return 200;
}

struct Response_Thread_Data
{
	SOCKET client_socket;
	World_Instance *world_instance;
	char *body_storage;
	int thread_id;
};

struct Socket_Buffer
{
	SOCKET socket;
	char *data;
	int data_size;
	int pos;
	int size;
};

struct Read_Block
{
	char *data;
	int length;
};

Socket_Buffer buffer_new(SOCKET socket, int size=1024)
{
	Socket_Buffer buffer = { 0 };
	buffer.socket = socket;
	buffer.data = (char*)malloc(size);
	buffer.data_size = size;
	return buffer;
}

// NOTE: Does not free the socket
void buffer_free(Socket_Buffer *buffer)
{
	free(buffer->data);
}

bool buffer_fill_read(Socket_Buffer *buffer)
{
	int bytes_read = recv(buffer->socket, buffer->data, buffer->data_size, 0);
	buffer->size = bytes_read;
	buffer->pos = 0;
	return bytes_read > 0;
}

bool buffer_peek(Socket_Buffer *buffer, Read_Block *block, int length)
{
	int buffer_left = buffer->size - buffer->pos;
	if (buffer_left == 0) {
		if (!buffer_fill_read(buffer))
			return false;
		buffer_left = buffer->size - buffer->pos;
	}

	block->data = buffer->data + buffer->pos;
	block->length = min(length, buffer_left);
	return true;
}

bool buffer_read(Socket_Buffer *buffer, Read_Block *block, int length)
{
	if (!buffer_peek(buffer, block, length))
		return false;
	buffer->pos += block->length;
	return true;
}

bool buffer_accept(Socket_Buffer *buffer, const char *value, int length)
{
	int pos = 0;
	Read_Block block;
	while (length - pos > 0) {
		if (!buffer_read(buffer, &block, length - pos))
			return false;
		if (memcmp(block.data, value + pos, block.length))
			return false;
		pos += block.length;
	}
	return true;
}

int buffer_read_line(Socket_Buffer *buffer, char *line, int length)
{
	int pos = 0;
	Read_Block block;
	while (length - pos > 0) {
		if (!buffer_peek(buffer, &block, length - pos)) return -3;

		char *end = (char*)memchr(block.data, '\r', block.length);
		if (end) {
			int in_length = (int)(end - block.data);
			if (in_length > length - pos - 1) return -1;

			memcpy(line + pos, block.data, in_length);
			pos += in_length;

			buffer->pos += in_length;
			if (!buffer_accept(buffer, "\r\n", 2)) return -2;
			line[pos] = '\0';

			return pos;
		} else {
			if (block.length > length - pos - 1) return -1;
			
			memcpy(line + pos, block.data, block.length);
			buffer->pos += block.length;
			pos += block.length;
		}
	}

	// Reached the end of the line buffer without finding a \r
	return -1;
}

void send_response(SOCKET socket, const char *content_type, int status,
	const char *body, size_t body_length)
{
	const char *status_desc = get_http_status_description(status);
	char response_start[128];
	sprintf(response_start, "HTTP/1.1 %d %s\r\n", status, status_desc);

	char content_length_header[128];
	sprintf(content_length_header, "Content-Length: %d\r\n", body_length);
	char content_type_header[128];
	sprintf(content_type_header, "Content-Type: %s\r\n", content_type);
	const char *separator = "\r\n";

	send(socket, response_start, (int)strlen(response_start), 0);
	send(socket, content_length_header, (int)strlen(content_length_header), 0);
	send(socket, content_type_header, (int)strlen(content_type_header), 0);
	send(socket, separator, (int)strlen(separator), 0);
	send(socket, body, (int)strlen(body), 0);
}

void send_text_response(SOCKET socket, const char *content_type, int status,
	const char *body)
{
	send_response(socket, content_type, status, body, strlen(body));
}

void *thread_do_response(void *thread_data)
{
	Response_Thread_Data *data = (Response_Thread_Data*)thread_data;
	SOCKET client_socket = data->client_socket;
	World_Instance *world_instance = data->world_instance;
	char *body = data->body_storage;

	__sync_fetch_and_add(&active_thread_count, 1);

	Socket_Buffer buffer = buffer_new(client_socket);

	for (;;) {

		char line[256];
		if (buffer_read_line(&buffer, line, sizeof(line)) < 0)
			break;

		char method[64];
		char path[128];
		char http_version[32];
		sscanf(line, "%s %s %s\r\n", method, path, http_version);

		printf("%d: Request to path %s %s\n", data->thread_id, method, path);

		bool failed = false;
		while (strlen(line)) {
			if (buffer_read_line(&buffer, line, sizeof(line)) < 0) {
				failed = true;
				break;
			}
		}
		if (failed)
			break;

		U32 id;
		if (!strcmp(path, "/favicon.ico")) {
			FILE *icon = fopen("data/icon.ico", "rb");
			fseek(icon, 0, SEEK_END);
			int size = ftell(icon);
			fseek(icon, 0, SEEK_SET);

			const char *response_start = "HTTP/1.1 200 OK\r\n";
			char content_length[128];
			sprintf(content_length, "Content-Length: %d\r\n", size);
			const char *content_type = "Content-Type: image/x-icon\r\n";
			const char *separator = "\r\n";

			send(client_socket, response_start, (int)strlen(response_start), 0);
			send(client_socket, content_length, (int)strlen(content_length), 0);
			send(client_socket, content_type, (int)strlen(content_type), 0);
			send(client_socket, separator, (int)strlen(separator), 0);

			while (!feof(icon)) {
				char iconbuf[512];
				int num = (int)fread(iconbuf, 1, sizeof(iconbuf), icon);

				send(client_socket, iconbuf, num, 0);
			}

			fclose(icon);

		} else if (!strcmp(path, "/dwarves")) {

			pthread_mutex_lock(&world_instance->lock);
			update_to_now(world_instance);
			int status = render_dwarves(world_instance->world, body);
			pthread_mutex_unlock(&world_instance->lock);

			send_response(client_socket, "text/html", status, body, strlen(body));

		} else if (!strcmp(path, "/feed")) {

			pthread_mutex_lock(&world_instance->lock);
			update_to_now(world_instance);
			int status = render_feed(world_instance->world, body);
			pthread_mutex_unlock(&world_instance->lock);

			send_text_response(client_socket, "text/html", status, body);
		
			// TODO: Seriously need a real routing scheme
		} else if (sscanf(path, "/entities/%d", &id) == 1 && strstr(path, "avatar.svg")) {

			pthread_mutex_lock(&world_instance->lock);
			update_to_now(world_instance);
			int status = render_entity_avatar(world_instance->world, id, body);
			pthread_mutex_unlock(&world_instance->lock);

			send_text_response(client_socket, "image/svg+xml", status, body);

		} else if (sscanf(path, "/entities/%d", &id) == 1) {

			pthread_mutex_lock(&world_instance->lock);
			update_to_now(world_instance);
			int status = render_entity(world_instance->world, id, body);
			pthread_mutex_unlock(&world_instance->lock);

			send_text_response(client_socket, "text/html", status, body);

		} else if (!strcmp(path, "/locations")) {

			pthread_mutex_lock(&world_instance->lock);
			update_to_now(world_instance);
			int status = render_locations(world_instance->world, body);
			pthread_mutex_unlock(&world_instance->lock);

			send_text_response(client_socket, "text/html", status, body);

		} else if (sscanf(path, "/locations/%d", &id) == 1) {

			pthread_mutex_lock(&world_instance->lock);
			update_to_now(world_instance);
			int status = render_location(world_instance->world, id, body);
			pthread_mutex_unlock(&world_instance->lock);

			send_text_response(client_socket, "text/html", status, body);

		} else if (!strcmp(path, "/stats")) {

			pthread_mutex_lock(&global_stats.lock);
			int status = render_stats(&global_stats, body);
			pthread_mutex_unlock(&global_stats.lock);

			send_text_response(client_socket, "text/html", status, body);

		}  else {
			const char *body = "<html><body><h1>Hello world!</h1></body></html>";
			send_text_response(client_socket, "text/html", 200, body);
		}
	}

	buffer_free(&buffer);
	shutdown(client_socket, 2);

	free(body);
	free(thread_data);

	__sync_fetch_and_sub(&active_thread_count, 1);
}

int main(int argc, char **argv)
{
	signal(SIGINT, handle_kill);

	global_stats.snapshot_count = 100;
	global_stats.active_thread_counts = (long*)calloc(global_stats.snapshot_count, sizeof(long));
	pthread_mutex_init(&global_stats.lock, 0);

	struct addrinfo *addr = NULL;
	struct addrinfo hints = { 0 };
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE;

	getaddrinfo(NULL, DORF_PORT, &hints, &addr);

	server_socket = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
	if (bind(server_socket, addr->ai_addr, (int)addr->ai_addrlen) == -1) {
		printf("bind failed: %d", errno);
	}
	if (listen(server_socket, SOMAXCONN) == -1) {
		printf("listen failed: %d", errno);
	}

	freeaddrinfo(addr);

	const char *names[] = {
		"Urist", "Gimli", "Thir", "Tharun", "Dofor", "Ufir",
		"Bohir",
	};

	puts("Dorfbook serving at port " DORF_PORT);
	puts("Enter ^C to stop");

	char name_buf[512], *name_ptr = name_buf;

	static World world = { 0 };
	world.random_series = series_from_seed32(0xD02F);

	world.locations[1].id = 1;
	world.locations[1].name = "Initial Cave";
	world.locations[2].id = 2;
	world.locations[2].name = "The Great Outdoors";
	world.locations[3].id = 3;
	world.locations[3].name = "Some Pub";
	world.locations[3].has_food = true;
	world.locations[4].id = 4;
	world.locations[4].name = "Bedroom";
	world.locations[4].has_bed = true;

	for (U32 id = 1; id < 10; id++) {
		char *name = name_ptr;
		name_ptr += 1 + sprintf(name_ptr, "%s %sson",
			names[rand() % Count(names)], names[rand() % Count(names)]);

		Dwarf *dwarf = &world.dwarves[id];
		dwarf->id = id;
		dwarf->location = 1;
		dwarf->name = name;
		dwarf->hunger = rand() % 50;
		dwarf->sleep = rand() % 50;
		dwarf->alive = true;
		dwarf->seed = next32(&world.random_series);
	}

	World_Instance world_instance = { 0 };
	world_instance.last_updated = time(NULL);
	world_instance.world = &world;
	pthread_mutex_init(&world_instance.lock, 0);

	pthread_t world_update_thread, stat_update_thread;

	pthread_create(&world_update_thread, 0, thread_background_world_update, &world_instance);
	pthread_create(&stat_update_thread, 0, thread_background_stat_update, &global_stats);

	int thread_id = 0;

	for (;;) {
		SOCKET client_socket = accept(server_socket, NULL, NULL);
		if (client_socket == -1)
			continue;

		int timeout = 15 * 1000; // 15 seconds
		int err;
		if (err = setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)))
			printf("Failed to set send timeout: %d\n", err);
		if (err = setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)))
			printf("Failed to set recv timeout: %d\n", err);

		Response_Thread_Data *thread_data = (Response_Thread_Data*)malloc(sizeof(Response_Thread_Data));
		thread_data->client_socket = client_socket;
		thread_data->world_instance = &world_instance;
		thread_data->body_storage = (char*)malloc(1024*1024);
		thread_data->thread_id = ++thread_id;

#if 1
		pthread_t response_thread;
		pthread_create(&response_thread, 0, thread_do_response, thread_data);
#else
		thread_do_response(thread_data);
#endif
	}
}

