/*! \file   janus_lua_extra.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus Lua plugin extra hooks
 * \details  The Janus Lua plugin implements all the mandatory hooks to
 * allow the C code to interact with a custom Lua script, and viceversa.
 * Anyway, Lua developers may want to have the C code do more than what
 * is provided out of the box, e.g., by exposing additional Lua methods
 * from C for further low level processing or native integration. This
 * "extra" implementation provides a mechanism to do just that, as
 * developers can just add their own custom hooks in the C extra code,
 * and the Lua plugin will register the new methods along the stock ones.
 *
 * More specifically, the Janus Lua plugin will always invoke the
 * janus_lua_register_extra_functions() method when initializing. This
 * means that all developers will need to do to register a new function
 * is adding new \c lua_register calls to register their own functions
 * there, and they'll be added to the stack.
 *
\endverbatim
 *
 * \ingroup luapapi
 * \ref luapapi
 */

#include <sys/time.h>

#include "janus_lua_data.h"
#include "janus_lua_extra.h"



#define JANUS_LUA_EXTRA_PLAY_OK	0
#define JANUS_LUA_EXTRA_PLAY_ERROR_WRONG_NUMBER_ARUMENTS	1000
#define JANUS_LUA_EXTRA_PLAY_ERROR_SESSION_NOT_FOUND	1001
#define JANUS_LUA_EXTRA_PLAY_ERROR_INVALID_RECORDING	1002
#define JANUS_LUA_EXTRA_PLAY_ERROR_THREAD_START	1003


/* This is where you can add your custom extra functions */

static void janus_play_recording_free(const janus_refcount *recording_ref);
janus_play_frame_packet *janus_play_get_frames(const char *dir, const char *filename);
static void *janus_play_playout_thread(void *data);
void lua_push_event(lua_State *state, guint32 id, const char *tr, const char *json);
void janus_rtp_header_update2(janus_rtp_header *header, janus_rtp_switching_context *context, gboolean video, int step);


static int janus_lua_method_startplaying(lua_State *lua_state) {
	JANUS_LOG(LOG_INFO, "Start playing\n");

	/* Get the arguments from the provided state */
	int arg_number = lua_gettop(lua_state);
	if(arg_number != 4 && arg_number != 6) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 4 or 6)\n", arg_number);
		lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_ERROR_WRONG_NUMBER_ARUMENTS);
		return 1;
	}
	guint32 id = lua_tonumber(lua_state, 1);
	const char *tr = lua_tostring(lua_state, 2);

	JANUS_LOG(LOG_INFO, "Start playing %d %s\n", id, tr);

	/* Find the session */
	janus_mutex_lock(&lua_sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(lua_ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&lua_sessions_mutex);
		lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_ERROR_SESSION_NOT_FOUND);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_lock(&session->rec_mutex);
	janus_mutex_unlock(&lua_sessions_mutex);

	session->recording = (janus_play_recording *)g_malloc0(sizeof(janus_play_recording));
	janus_refcount_init(&session->recording->ref, janus_play_recording_free);
	session->recording->stop_playing = FALSE;
	/* Access the frames */
	if(arg_number == 4 || arg_number == 6) {
		session->aframes = janus_play_get_frames(lua_tostring(lua_state, 3), lua_tostring(lua_state, 4));
		if(session->aframes == NULL) {
			JANUS_LOG(LOG_WARN, "Error opening audio recording, trying to go on anyway\n");
		} else {
			session->recording->arc_path = lua_tostring(lua_state, 3);
			session->recording->arc_file = lua_tostring(lua_state, 4);
		}
	}

	if(arg_number == 6) {
		session->vframes = janus_play_get_frames(lua_tostring(lua_state, 5), lua_tostring(lua_state, 6));
		if(session->vframes == NULL) {
			JANUS_LOG(LOG_WARN, "Error opening video recording, trying to go on anyway\n");
		} else {
			session->recording->vrc_path = lua_tostring(lua_state, 5);
			session->recording->vrc_file = lua_tostring(lua_state, 6);
		}
	}

	if(session->aframes == NULL && session->vframes == NULL) {
		JANUS_LOG(LOG_ERR, "Error opening recording files\n");
		lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_ERROR_INVALID_RECORDING);
		return 1;
	}

	// TODO stopping && initialized
	// if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
	// 	return;
	/* Take note of the fact that the session is now active */
	session->active = TRUE;


	session->lua_state = lua_state;
	session->transaction_id = tr;

	GError *error = NULL;
	janus_refcount_increase(&session->ref);
	janus_refcount_increase(&session->recording->ref);
	g_thread_try_new("play playout thread", &janus_play_playout_thread, session, &error);
	if(error != NULL) {
		janus_refcount_decrease(&session->ref);
		janus_refcount_decrease(&session->recording->ref);
		/* FIXME Should we notify this back to the user somehow? */
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the Record&Play playout thread...\n", error->code, error->message ? error->message : "??");
		if(janus_core) {
			janus_core->close_pc(session->handle);
		}
		lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_ERROR_THREAD_START);
		return 1;
	}

	janus_refcount_decrease(&session->ref);
	janus_refcount_decrease(&session->recording->ref);

	lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_OK);
	return 1;
}

static int janus_lua_method_stopplaying(lua_State *lua_state) {
	/* Let's do nothing, and return 1234 */
	JANUS_LOG(LOG_INFO, "Stop playing\n");

	int arg_number = lua_gettop(lua_state);
	if(arg_number != 1) {
		JANUS_LOG(LOG_ERR, "Wrong number of arguments: %d (expected 1)\n", arg_number);
		lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_ERROR_WRONG_NUMBER_ARUMENTS);
		return 1;
	}
	guint32 id = lua_tonumber(lua_state, 1);

	/* Find the session */
	janus_mutex_lock(&lua_sessions_mutex);
	janus_lua_session *session = g_hash_table_lookup(lua_ids, GUINT_TO_POINTER(id));
	if(session == NULL || g_atomic_int_get(&session->destroyed)) {
		janus_mutex_unlock(&lua_sessions_mutex);
		lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_ERROR_SESSION_NOT_FOUND);
		return 1;
	}
	janus_refcount_increase(&session->ref);
	janus_mutex_lock(&session->rec_mutex);
	janus_mutex_unlock(&lua_sessions_mutex);

	if (session->recording != NULL) {
		session->recording->stop_playing = TRUE;
	}

	janus_refcount_decrease(&session->ref);
	lua_pushnumber(lua_state, JANUS_LUA_EXTRA_PLAY_OK);
	return 1;
}

/* Public method to register all custom extra functions */
void janus_lua_register_extra_functions(lua_State *state) {
	if(state == NULL)
		return;
	/* Register all extra functions here */
	// lua_register(state, "testExtraFunction", janus_lua_extra_sample);
	lua_register(lua_state, "startPlaying", janus_lua_method_startplaying);
	lua_register(lua_state, "stopPlaying", janus_lua_method_stopplaying);

}

static void janus_play_recording_free(const janus_refcount *recording_ref) {
	janus_play_recording *recording = janus_refcount_containerof(recording_ref, janus_play_recording, ref);
	/* This recording can be destroyed, free all the resources */
	// g_free(recording->name);
	// g_free(recording->date);
	// g_free(recording->arc_file);
	// g_free(recording->arc_path);
	// g_free(recording->vrc_file);
	// g_free(recording->vrc_path);
	// g_free(recording->offer);
	g_free(recording);
}

void lua_push_event(lua_State *state, guint32 id, const char *tr, const char *json) {
	/* Can't call pushEvent straight from janus_lua_extra.c. So have to call it via lua */
	/* Notify the Lua script */
	lua_State *t = lua_newthread(lua_state);
	lua_getglobal(t, "luaPushEvent");
	lua_pushnumber(t, id);
	lua_pushstring(t, tr);
	lua_pushstring(t, json);
	lua_call(t, 3, 0);
}

janus_play_frame_packet *janus_play_get_frames(const char *dir, const char *filename) {
	if(!dir || !filename)
		return NULL;
	/* Open the file */
	char source[1024];
	if(strstr(filename, ".mjr"))
		g_snprintf(source, 1024, "%s/%s", dir, filename);
	else
		g_snprintf(source, 1024, "%s/%s.mjr", dir, filename);
	FILE *file = fopen(source, "rb");
	if(file == NULL) {
		JANUS_LOG(LOG_ERR, "Could not open file %s\n", source);
		return NULL;
	}
	fseek(file, 0L, SEEK_END);
	long fsize = ftell(file);
	fseek(file, 0L, SEEK_SET);
	JANUS_LOG(LOG_VERB, "File is %zu bytes\n", fsize);

	/* Pre-parse */
	JANUS_LOG(LOG_VERB, "Pre-parsing file %s to generate ordered index...\n", source);
	gboolean parsed_header = FALSE;
	int bytes = 0;
	long offset = 0;
	uint16_t len = 0, count = 0;
	uint32_t first_ts = 0, last_ts = 0, reset = 0;	/* To handle whether there's a timestamp reset in the recording */
	char prebuffer[1500];
	memset(prebuffer, 0, 1500);
	/* Let's look for timestamp resets first */
	while(offset < fsize) {
		/* Read frame header */
		fseek(file, offset, SEEK_SET);
		bytes = fread(prebuffer, sizeof(char), 8, file);
		if(bytes != 8 || prebuffer[0] != 'M') {
			JANUS_LOG(LOG_ERR, "Invalid header...\n");
			fclose(file);
			return NULL;
		}
		if(prebuffer[1] == 'E') {
			/* Either the old .mjr format header ('MEETECHO' header followed by 'audio' or 'video'), or a frame */
			offset += 8;
			bytes = fread(&len, sizeof(uint16_t), 1, file);
			len = ntohs(len);
			offset += 2;
			if(len == 5 && !parsed_header) {
				/* This is the main header */
				parsed_header = TRUE;
				JANUS_LOG(LOG_VERB, "Old .mjr header format\n");
				bytes = fread(prebuffer, sizeof(char), 5, file);
				if(prebuffer[0] == 'v') {
					JANUS_LOG(LOG_INFO, "This is an old video recording, assuming VP8\n");
				} else if(prebuffer[0] == 'a') {
					JANUS_LOG(LOG_INFO, "This is an old audio recording, assuming Opus\n");
				} else {
					JANUS_LOG(LOG_WARN, "Unsupported recording media type...\n");
					fclose(file);
					return NULL;
				}
				offset += len;
				continue;
			} else if(len < 12) {
				/* Not RTP, skip */
				JANUS_LOG(LOG_VERB, "Skipping packet (not RTP?)\n");
				offset += len;
				continue;
			}
		} else if(prebuffer[1] == 'J') {
			/* New .mjr format, the header may contain useful info */
			offset += 8;
			bytes = fread(&len, sizeof(uint16_t), 1, file);
			len = ntohs(len);
			offset += 2;
			if(len > 0 && !parsed_header) {
				/* This is the info header */
				JANUS_LOG(LOG_VERB, "New .mjr header format\n");
				bytes = fread(prebuffer, sizeof(char), len, file);
				if(bytes < 0) {
					JANUS_LOG(LOG_ERR, "Error reading from file... %s\n", strerror(errno));
					fclose(file);
					return NULL;
				}
				parsed_header = TRUE;
				prebuffer[len] = '\0';
				json_error_t error;
				json_t *info = json_loads(prebuffer, 0, &error);
				if(!info) {
					JANUS_LOG(LOG_ERR, "JSON error: on line %d: %s\n", error.line, error.text);
					JANUS_LOG(LOG_WARN, "Error parsing info header...\n");
					fclose(file);
					return NULL;
				}
				/* Is it audio or video? */
				json_t *type = json_object_get(info, "t");
				if(!type || !json_is_string(type)) {
					JANUS_LOG(LOG_WARN, "Missing/invalid recording type in info header...\n");
					json_decref(info);
					fclose(file);
					return NULL;
				}
				const char *t = json_string_value(type);
				int video = 0;
				gint64 c_time = 0, w_time = 0;
				if(!strcasecmp(t, "v")) {
					video = 1;
				} else if(!strcasecmp(t, "a")) {
					video = 0;
				} else {
					JANUS_LOG(LOG_WARN, "Unsupported recording type '%s' in info header...\n", t);
					json_decref(info);
					fclose(file);
					return NULL;
				}
				/* What codec was used? */
				json_t *codec = json_object_get(info, "c");
				if(!codec || !json_is_string(codec)) {
					JANUS_LOG(LOG_WARN, "Missing recording codec in info header...\n");
					json_decref(info);
					fclose(file);
					return NULL;
				}
				const char *c = json_string_value(codec);
				/* When was the file created? */
				json_t *created = json_object_get(info, "s");
				if(!created || !json_is_integer(created)) {
					JANUS_LOG(LOG_WARN, "Missing recording created time in info header...\n");
					json_decref(info);
					fclose(file);
					return NULL;
				}
				c_time = json_integer_value(created);
				/* When was the first frame written? */
				json_t *written = json_object_get(info, "u");
				if(!written || !json_is_integer(written)) {
					JANUS_LOG(LOG_WARN, "Missing recording written time in info header...\n");
					json_decref(info);
					fclose(file);
					return NULL;
				}
				w_time = json_integer_value(created);
				/* Summary */
				JANUS_LOG(LOG_VERB, "This is %s recording:\n", video ? "a video" : "an audio");
				JANUS_LOG(LOG_VERB, "  -- Codec:   %s\n", c);
				JANUS_LOG(LOG_VERB, "  -- Created: %"SCNi64"\n", c_time);
				JANUS_LOG(LOG_VERB, "  -- Written: %"SCNi64"\n", w_time);
				json_decref(info);
			}
		} else {
			JANUS_LOG(LOG_ERR, "Invalid header...\n");
			fclose(file);
			return NULL;
		}
		/* Only read RTP header */
		bytes = fread(prebuffer, sizeof(char), 16, file);
		janus_rtp_header *rtp = (janus_rtp_header *)prebuffer;
		if(last_ts == 0) {
			first_ts = ntohl(rtp->timestamp);
			if(first_ts > 1000*1000)	/* Just used to check whether a packet is pre- or post-reset */
				first_ts -= 1000*1000;
		} else {
			if(ntohl(rtp->timestamp) < last_ts) {
				/* The new timestamp is smaller than the next one, is it a timestamp reset or simply out of order? */
				if(last_ts-ntohl(rtp->timestamp) > 2*1000*1000*1000) {
					reset = ntohl(rtp->timestamp);
					JANUS_LOG(LOG_VERB, "Timestamp reset: %"SCNu32"\n", reset);
				}
			} else if(ntohl(rtp->timestamp) < reset) {
				JANUS_LOG(LOG_VERB, "Updating timestamp reset: %"SCNu32" (was %"SCNu32")\n", ntohl(rtp->timestamp), reset);
				reset = ntohl(rtp->timestamp);
			}
		}
		last_ts = ntohl(rtp->timestamp);
		/* Skip data for now */
		offset += len;
	}
	/* Now let's parse the frames and order them */
	offset = 0;
	janus_play_frame_packet *list = NULL, *last = NULL;
	while(offset < fsize) {
		/* Read frame header */
		fseek(file, offset, SEEK_SET);
		bytes = fread(prebuffer, sizeof(char), 8, file);
		prebuffer[8] = '\0';
		JANUS_LOG(LOG_HUGE, "Header: %s\n", prebuffer);
		offset += 8;
		bytes = fread(&len, sizeof(uint16_t), 1, file);
		len = ntohs(len);
		JANUS_LOG(LOG_HUGE, "  -- Length: %"SCNu16"\n", len);
		offset += 2;
		if(prebuffer[1] == 'J' || len < 12) {
			/* Not RTP, skip */
			JANUS_LOG(LOG_HUGE, "  -- Not RTP, skipping\n");
			offset += len;
			continue;
		}
		/* Only read RTP header */
		bytes = fread(prebuffer, sizeof(char), 16, file);
		if(bytes < 0) {
			JANUS_LOG(LOG_WARN, "Error reading RTP header, stopping here...\n");
			break;
		}
		janus_rtp_header *rtp = (janus_rtp_header *)prebuffer;
		JANUS_LOG(LOG_HUGE, "  -- RTP packet (ssrc=%"SCNu32", pt=%"SCNu16", ext=%"SCNu16", seq=%"SCNu16", ts=%"SCNu32")\n",
				ntohl(rtp->ssrc), rtp->type, rtp->extension, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
		/* Generate frame packet and insert in the ordered list */
		janus_play_frame_packet *p = g_malloc0(sizeof(janus_play_frame_packet));
		p->seq = ntohs(rtp->seq_number);
		if(reset == 0) {
			/* Simple enough... */
			p->ts = ntohl(rtp->timestamp);
		} else {
			/* Is this packet pre- or post-reset? */
			if(ntohl(rtp->timestamp) > first_ts) {
				/* Pre-reset... */
				p->ts = ntohl(rtp->timestamp);
			} else {
				/* Post-reset... */
				uint64_t max32 = UINT32_MAX;
				max32++;
				p->ts = max32+ntohl(rtp->timestamp);
			}
		}
		p->len = len;
		p->offset = offset;
		p->next = NULL;
		p->prev = NULL;
		if(list == NULL) {
			/* First element becomes the list itself (and the last item), at least for now */
			list = p;
			last = p;
		} else {
			/* Check where we should insert this, starting from the end */
			int added = 0;
			janus_play_frame_packet *tmp = last;
			while(tmp) {
				if(tmp->ts < p->ts) {
					/* The new timestamp is greater than the last one we have, append */
					added = 1;
					if(tmp->next != NULL) {
						/* We're inserting */
						tmp->next->prev = p;
						p->next = tmp->next;
					} else {
						/* Update the last packet */
						last = p;
					}
					tmp->next = p;
					p->prev = tmp;
					break;
				} else if(tmp->ts == p->ts) {
					/* Same timestamp, check the sequence number */
					if(tmp->seq < p->seq && (abs(tmp->seq - p->seq) < 10000)) {
						/* The new sequence number is greater than the last one we have, append */
						added = 1;
						if(tmp->next != NULL) {
							/* We're inserting */
							tmp->next->prev = p;
							p->next = tmp->next;
						} else {
							/* Update the last packet */
							last = p;
						}
						tmp->next = p;
						p->prev = tmp;
						break;
					} else if(tmp->seq > p->seq && (abs(tmp->seq - p->seq) > 10000)) {
						/* The new sequence number (resetted) is greater than the last one we have, append */
						added = 1;
						if(tmp->next != NULL) {
							/* We're inserting */
							tmp->next->prev = p;
							p->next = tmp->next;
						} else {
							/* Update the last packet */
							last = p;
						}
						tmp->next = p;
						p->prev = tmp;
						break;
					}
				}
				/* If either the timestamp ot the sequence number we just got is smaller, keep going back */
				tmp = tmp->prev;
			}
			if(!added) {
				/* We reached the start */
				p->next = list;
				list->prev = p;
				list = p;
			}
		}
		/* Skip data for now */
		offset += len;
		count++;
	}

	JANUS_LOG(LOG_VERB, "Counted %"SCNu16" RTP packets\n", count);
	janus_play_frame_packet *tmp = list;
	count = 0;
	while(tmp) {
		count++;
		JANUS_LOG(LOG_HUGE, "[%10lu][%4d] seq=%"SCNu16", ts=%"SCNu64"\n", tmp->offset, tmp->len, tmp->seq, tmp->ts);
		tmp = tmp->next;
	}
	JANUS_LOG(LOG_VERB, "Counted %"SCNu16" frame packets\n", count);

	/* Done! */
	fclose(file);
	return list;
}

static void *janus_play_playout_thread(void *data) {
	janus_lua_session *session = (janus_lua_session *)data;
	if(!session) {
		JANUS_LOG(LOG_ERR, "Invalid session, can't start playout thread...\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	if(!session->recording) {
		janus_refcount_decrease(&session->ref);
		JANUS_LOG(LOG_ERR, "No recording object, can't start playout thread...\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	// janus_refcount_increase(&session->recording->ref);
	janus_play_recording *rec = session->recording;
	if(session->recorder) {
		janus_refcount_decrease(&rec->ref);
		janus_refcount_decrease(&session->ref);
		JANUS_LOG(LOG_ERR, "This is a recorder, can't start playout thread...\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	if(!session->aframes && !session->vframes) {
		janus_refcount_decrease(&rec->ref);
		janus_refcount_decrease(&session->ref);
		JANUS_LOG(LOG_ERR, "No audio and no video frames, can't start playout thread...\n");
		g_thread_unref(g_thread_self());
		return NULL;
	}
	JANUS_LOG(LOG_INFO, "Joining playout thread\n");
	/* Open the files */
	FILE *afile = NULL, *vfile = NULL;
	if(session->aframes) {
		char source[1024];
		if(strstr(rec->arc_file, ".mjr"))
			g_snprintf(source, 1024, "%s/%s", rec->arc_path, rec->arc_file);
		else
			g_snprintf(source, 1024, "%s/%s.mjr", rec->arc_path, rec->arc_file);
		afile = fopen(source, "rb");
		if(afile == NULL) {
			janus_refcount_decrease(&rec->ref);
			janus_refcount_decrease(&session->ref);
			JANUS_LOG(LOG_ERR, "Could not open audio file %s, can't start playout thread...\n", source);
			g_thread_unref(g_thread_self());
			return NULL;
		}
	}
	if(session->vframes) {
		char source[1024];
		if(strstr(rec->vrc_file, ".mjr"))
			g_snprintf(source, 1024, "%s/%s", rec->vrc_path, rec->vrc_file);
		else
			g_snprintf(source, 1024, "%s/%s.mjr", rec->vrc_path, rec->vrc_file);
		vfile = fopen(source, "rb");
		if(vfile == NULL) {
			janus_refcount_decrease(&rec->ref);
			janus_refcount_decrease(&session->ref);
			JANUS_LOG(LOG_ERR, "Could not open video file %s, can't start playout thread...\n", source);
			if(afile)
				fclose(afile);
			afile = NULL;
			g_thread_unref(g_thread_self());
			return NULL;
		}
	}
	/* Timer */
	gboolean asent = FALSE, vsent = FALSE;
	struct timeval now, abefore, vbefore;
	time_t d_s, d_us;
	gettimeofday(&now, NULL);
	gettimeofday(&abefore, NULL);
	gettimeofday(&vbefore, NULL);
	janus_play_frame_packet *audio = session->aframes, *video = session->vframes;
	char *buffer = (char *)g_malloc0(1500);
	memset(buffer, 0, 1500);
	int bytes = 0;
	int64_t ts_diff = 0, passed = 0;

	// TODO auto handeling of akhz & vkhz????????
	// int audio_pt = session->recording->audio_pt;
	// int video_pt = session->recording->video_pt;

	int akhz = 48;
	// if(audio_pt == 0 || audio_pt == 8 || audio_pt == 9)
	// 	akhz = 8;
	int vkhz = 90;

	session->rtpctx.a_seq_reset = TRUE;
	session->rtpctx.v_seq_reset = TRUE;

	const char *json = "{\"play\": \"start\"}";
	lua_push_event(session->lua_state, session->id, session->transaction_id, json);

	while(!g_atomic_int_get(&session->destroyed) && session->active
			&& !g_atomic_int_get(&rec->destroyed) && (audio || video) && session->recording->stop_playing == FALSE) {
		if(!asent && !vsent) {
			/* We skipped the last round, so sleep a bit (5ms) */
			usleep(5000);
		}
		asent = FALSE;
		vsent = FALSE;
		if(audio) {
			if(audio == session->aframes) {
				/* First packet, send now */
				fseek(afile, audio->offset, SEEK_SET);
				bytes = fread(buffer, sizeof(char), audio->len, afile);
				if(bytes != audio->len)
					JANUS_LOG(LOG_WARN, "Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, audio->len);
				/* Update payload type */
				janus_rtp_header *rtp = (janus_rtp_header *)buffer;

				janus_rtp_header_update2(rtp, &session->rtpctx, 0, 960);
				// rtp->type = audio_pt;

				if(janus_core != NULL)
					janus_core->relay_rtp(session->handle, 0, (char *)buffer, bytes);
				gettimeofday(&now, NULL);
				abefore.tv_sec = now.tv_sec;
				abefore.tv_usec = now.tv_usec;
				asent = TRUE;
				audio = audio->next;
			} else {
				/* What's the timestamp skip from the previous packet? */
				ts_diff = audio->ts - audio->prev->ts;
				ts_diff = (ts_diff*1000)/akhz;
				/* Check if it's time to send */
				gettimeofday(&now, NULL);
				d_s = now.tv_sec - abefore.tv_sec;
				d_us = now.tv_usec - abefore.tv_usec;
				if(d_us < 0) {
					d_us += 1000000;
					--d_s;
				}
				passed = d_s*1000000 + d_us;
				if(passed < (ts_diff-5000)) {
					asent = FALSE;
				} else {
					/* Update the reference time */
					abefore.tv_usec += ts_diff%1000000;
					if(abefore.tv_usec > 1000000) {
						abefore.tv_sec++;
						abefore.tv_usec -= 1000000;
					}
					if(ts_diff/1000000 > 0) {
						abefore.tv_sec += ts_diff/1000000;
						abefore.tv_usec -= ts_diff/1000000;
					}
					/* Send now */
					fseek(afile, audio->offset, SEEK_SET);
					bytes = fread(buffer, sizeof(char), audio->len, afile);
					if(bytes != audio->len)
						JANUS_LOG(LOG_WARN, "Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, audio->len);
					/* Update payload type */
					janus_rtp_header *rtp = (janus_rtp_header *)buffer;

					janus_rtp_header_update2(rtp, &session->rtpctx, 0, 960);

					// rtp->type = audio_pt;
					if(janus_core != NULL)
						janus_core->relay_rtp(session->handle, 0, (char *)buffer, bytes);
					asent = TRUE;
					audio = audio->next;
				}
			}
		}
		if(video) {
			if(video == session->vframes) {
				/* First packets: there may be many of them with the same timestamp, send them all */
				uint64_t ts = video->ts;
				while(video && video->ts == ts) {
					fseek(vfile, video->offset, SEEK_SET);
					bytes = fread(buffer, sizeof(char), video->len, vfile);
					if(bytes != video->len)
						JANUS_LOG(LOG_WARN, "Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, video->len);
					/* Update payload type */
					janus_rtp_header *rtp = (janus_rtp_header *)buffer;

					janus_rtp_header_update2(rtp, &session->rtpctx, 1, 4500);

					// rtp->type = video_pt;
					if(janus_core != NULL)
						janus_core->relay_rtp(session->handle, 1, (char *)buffer, bytes);
					video = video->next;
				}
				vsent = TRUE;
				gettimeofday(&now, NULL);
				vbefore.tv_sec = now.tv_sec;
				vbefore.tv_usec = now.tv_usec;
			} else {
				/* What's the timestamp skip from the previous packet? */
				ts_diff = video->ts - video->prev->ts;
				ts_diff = (ts_diff*1000)/vkhz;
				/* Check if it's time to send */
				gettimeofday(&now, NULL);
				d_s = now.tv_sec - vbefore.tv_sec;
				d_us = now.tv_usec - vbefore.tv_usec;
				if(d_us < 0) {
					d_us += 1000000;
					--d_s;
				}
				passed = d_s*1000000 + d_us;
				if(passed < (ts_diff-5000)) {
					vsent = FALSE;
				} else {
					/* Update the reference time */
					vbefore.tv_usec += ts_diff%1000000;
					if(vbefore.tv_usec > 1000000) {
						vbefore.tv_sec++;
						vbefore.tv_usec -= 1000000;
					}
					if(ts_diff/1000000 > 0) {
						vbefore.tv_sec += ts_diff/1000000;
						vbefore.tv_usec -= ts_diff/1000000;
					}
					/* There may be multiple packets with the same timestamp, send them all */
					uint64_t ts = video->ts;
					while(video && video->ts == ts) {
						/* Send now */
						fseek(vfile, video->offset, SEEK_SET);
						bytes = fread(buffer, sizeof(char), video->len, vfile);
						if(bytes != video->len)
							JANUS_LOG(LOG_WARN, "Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, video->len);
						/* Update payload type */
						janus_rtp_header *rtp = (janus_rtp_header *)buffer;

						janus_rtp_header_update2(rtp, &session->rtpctx, 1, 4500);

						// rtp->type = video_pt;
						if(janus_core != NULL)
							janus_core->relay_rtp(session->handle, 1, (char *)buffer, bytes);
						video = video->next;
					}
					vsent = TRUE;
				}
			}
		}
	}
	g_free(buffer);

	if (session->recording->stop_playing == TRUE) {
		json = "{\"play\": \"stopped\"}";
	} else {
		json = "{\"play\": \"ended\"}";
	}
	lua_push_event(session->lua_state, session->id, session->transaction_id, json);

	/* Get rid of the indexes */
	janus_play_frame_packet *tmp = NULL;
	audio = session->aframes;
	while(audio) {
		tmp = audio->next;
		g_free(audio);
		audio = tmp;
	}
	session->aframes = NULL;
	video = session->vframes;
	while(video) {
		tmp = video->next;
		g_free(video);
		video = tmp;
	}
	session->vframes = NULL;
	if(afile)
		fclose(afile);
	afile = NULL;
	if(vfile)
		fclose(vfile);
	vfile = NULL;
	/* Remove from the list of viewers */
// 	janus_mutex_lock(&rec->mutex);
// 	rec->viewers = g_list_remove(rec->viewers, session);
// 	janus_mutex_unlock(&rec->mutex);
// 	/* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
// 	janus_core->close_pc(session->handle);

	janus_refcount_decrease(&rec->ref);
	janus_refcount_decrease(&session->ref);
	// janus_refcount_decrease(&session->recording->ref);
	JANUS_LOG(LOG_INFO, "Leaving playout thread\n");
	g_thread_unref(g_thread_self());
	return NULL;
}


void janus_rtp_header_update2(janus_rtp_header *header, janus_rtp_switching_context *context, gboolean video, int step) {
	if(header == NULL || context == NULL)
		return;
	/* Note: while the step property is still there for compatibility reasons, to
	 * keep the signature as it was before, it's ignored: whenever there's a switch
	 * to take into account, we compute how much time passed between the last RTP
	 * packet with the old SSRC and this new one, and prepare a timestamp accordingly */
	uint32_t ssrc = ntohl(header->ssrc);
	uint32_t timestamp = ntohl(header->timestamp);
	uint16_t seq = ntohs(header->seq_number);
	if(video) {
		if(ssrc != context->v_last_ssrc) {
			/* Video SSRC changed: update both sequence number and timestamp */
			JANUS_LOG(LOG_VERB, "Video SSRC changed, %"SCNu32" --> %"SCNu32"\n",
				context->v_last_ssrc, ssrc);
			context->v_last_ssrc = ssrc;
			context->v_base_ts_prev = context->v_last_ts;
			context->v_base_ts = timestamp;
			context->v_base_seq_prev = context->v_last_seq;
			context->v_base_seq = seq;
			/* How much time since the last video RTP packet? We compute an offset accordingly */
			if(context->v_last_time > 0) {
				gint64 time_diff = janus_get_monotonic_time() - context->v_last_time;
				time_diff = (time_diff*90)/1000; 	/* We're assuming 90khz here */
				if(time_diff == 0)
					time_diff = 1;
				context->v_base_ts_prev += (guint32)time_diff;
				context->v_last_ts += (guint32)time_diff;
				JANUS_LOG(LOG_VERB, "Computed offset for video RTP timestamp: %"SCNu32"\n", (guint32)time_diff);
			}
			/* Reset skew compensation data */
			context->v_new_ssrc = TRUE;
		}
		if(context->v_seq_reset) {
			/* Video sequence number was paused for a while: just update that */
			context->v_seq_reset = FALSE;
			context->v_base_seq_prev = context->v_last_seq;
			context->v_base_seq = seq;
			// fix timestamp for playback
			context->v_base_ts_prev = context->v_last_ts + 2000;
		}
		/* Compute a coherent timestamp and sequence number */
		context->v_prev_ts = context->v_last_ts;
		context->v_last_ts = (timestamp-context->v_base_ts) + context->v_base_ts_prev;
		context->v_prev_seq = context->v_last_seq;
		context->v_last_seq = (seq-context->v_base_seq)+context->v_base_seq_prev+1;
		/* Update the timestamp and sequence number in the RTP packet */
		header->timestamp = htonl(context->v_last_ts);
		header->seq_number = htons(context->v_last_seq);
		/* Take note of when we last handled this RTP packet */
		context->v_last_time = janus_get_monotonic_time();
	} else {
		if(ssrc != context->a_last_ssrc) {
			/* Audio SSRC changed: update both sequence number and timestamp */
			JANUS_LOG(LOG_VERB, "Audio SSRC changed, %"SCNu32" --> %"SCNu32"\n",
				context->a_last_ssrc, ssrc);
			context->a_last_ssrc = ssrc;
			context->a_base_ts_prev = context->a_last_ts;
			context->a_base_ts = timestamp;
			context->a_base_seq_prev = context->a_last_seq;
			context->a_base_seq = seq;
			/* How much time since the last audio RTP packet? We compute an offset accordingly */
			if(context->a_last_time > 0) {
				gint64 time_diff = janus_get_monotonic_time() - context->a_last_time;
				int akhz = 48;
				if(header->type == 0 || header->type == 8 || header->type == 9)
					akhz = 8;	/* We're assuming 48khz here (Opus), unless it's G.711/G.722 (8khz) */
				time_diff = (time_diff*akhz)/1000;
				if(time_diff == 0)
					time_diff = 1;
				context->a_base_ts_prev += (guint32)time_diff;
				context->a_prev_ts += (guint32)time_diff;
				context->a_last_ts += (guint32)time_diff;
				JANUS_LOG(LOG_VERB, "Computed offset for audio RTP timestamp: %"SCNu32"\n", (guint32)time_diff);
			}
			/* Reset skew compensation data */
			context->a_new_ssrc = TRUE;
		}
		if(context->a_seq_reset) {
			/* Audio sequence number was paused for a while: just update that */
			context->a_seq_reset = FALSE;
			context->a_base_seq_prev = context->a_last_seq;
			context->a_base_seq = seq;
			// fix timestamp for playback
			context->v_base_ts_prev = context->v_last_ts + 2000;
		}
		/* Compute a coherent timestamp and sequence number */
		context->a_prev_ts = context->a_last_ts;
		context->a_last_ts = (timestamp-context->a_base_ts) + context->a_base_ts_prev;
		context->a_prev_seq = context->a_last_seq;
		context->a_last_seq = (seq-context->a_base_seq)+context->a_base_seq_prev+1;
		/* Update the timestamp and sequence number in the RTP packet */
		header->timestamp = htonl(context->a_last_ts);
		header->seq_number = htons(context->a_last_seq);
		/* Take note of when we last handled this RTP packet */
		context->a_last_time = janus_get_monotonic_time();
	}
}
