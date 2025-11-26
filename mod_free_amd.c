#include <switch.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load);
SWITCH_MODULE_DEFINITION(mod_free_amd, mod_amd_load, mod_amd_shutdown, NULL);

static switch_hash_t *bug_hash = NULL;
static switch_mutex_t *bug_hash_mutex = NULL;

SWITCH_STANDARD_APP(voice_start_function);
SWITCH_STANDARD_APP(voice_stop_function);
SWITCH_STANDARD_APP(waitforresult_function);

static struct {
	uint32_t silent_threshold;
	uint32_t silent_initial;
	uint32_t silent_after_intro;
	uint32_t silent_max_session;
	uint32_t noise_max_intro;
	uint32_t noise_min_length;
	uint32_t noise_inter_silence;
	uint32_t noise_max_count;
	uint32_t total_analysis_time;
	uint32_t debug;
} globals;

static switch_xml_config_item_t instructions[] = {
	SWITCH_CONFIG_ITEM(
		"silent_threshold",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silent_threshold,
		(void *) 256,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"silent_initial",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silent_initial,
		(void *) 4500,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"silent_after_intro",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silent_after_intro,
		(void *) 1000,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"silent_max_session",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.silent_max_session,
		(void *) 200,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"noise_max_intro",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.noise_max_intro,
		(void *) 1250,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"noise_min_length",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.noise_min_length,
		(void *) 120,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"noise_inter_silence",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.noise_inter_silence,
		(void *) 30,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"noise_max_count",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.noise_max_count,
		(void *) 6,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"total_analysis_time",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.total_analysis_time,
		(void *) 5000,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM(
		"debug",
		SWITCH_CONFIG_INT,
		CONFIG_RELOADABLE,
		&globals.debug,
		(void *) 0,
		NULL, NULL, NULL),

	SWITCH_CONFIG_ITEM_END()
};

static switch_status_t do_config(switch_bool_t reload)
{
	memset(&globals, 0, sizeof(globals));

	if (switch_xml_config_parse_module_settings("amd.conf", reload, instructions) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_amd_load)
{
	switch_application_interface_t *app_interface;

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	do_config(SWITCH_FALSE);

	switch_mutex_init(&bug_hash_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_core_hash_init(&bug_hash);

	SWITCH_ADD_APP(
		app_interface,
		"voice_start",
		"Start answering machine detection",
		"Start AMD on a channel",
		voice_start_function,
		NULL,
		SAF_NONE);

	SWITCH_ADD_APP(
		app_interface,
		"voice_stop",
		"Stop answering machine detection",
		"Stop AMD on a channel",
		voice_stop_function,
		NULL,
		SAF_NONE);

	SWITCH_ADD_APP(
		app_interface,
		"waitforresult",
		"Wait for AMD result",
		"Wait for AMD result [<file to play while waiting>]",
		waitforresult_function,
		NULL,
		SAF_NONE);

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_amd_shutdown)
{
	switch_xml_config_cleanup(instructions);
	if (bug_hash_mutex) {
		switch_mutex_destroy(bug_hash_mutex);
		bug_hash_mutex = NULL;
	}
	bug_hash = NULL;
	return SWITCH_STATUS_SUCCESS;
}

typedef enum {
	SILENCE,
	VOICED
} amd_frame_classifier;

typedef enum {
	VAD_STATE_IN_WORD,
	VAD_STATE_IN_SILENCE,
} amd_vad_state_t;

/* Helper structure to access media bug frame (matches internal layout) */
typedef struct {
	void *session;
	void *channel;
	switch_frame_t *read_frame;
	switch_frame_t *write_frame;
	/* ... other fields ... */
} amd_media_bug_helper_t;

typedef struct {
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_media_bug_t *bug;
	switch_codec_t raw_codec;  /* L16 codec for decoding frames */
	amd_vad_state_t state;
	uint32_t frame_ms;

	uint32_t silence_duration;
	uint32_t voice_duration;
	uint32_t words;  /* Total word count (throughout entire detection, including intro) */
	uint32_t intro_words;  /* Word count during intro period (first noise_max_intro ms) */
	uint32_t intro_voice_duration;
	uint32_t total_duration;
	uint32_t last_noise_start;
	uint32_t last_noise_end;
	uint32_t current_word_duration;
	uint32_t word_counted:1;

	/* Per-session configuration (0 means use global) */
	uint32_t silent_threshold;
	uint32_t silent_initial;
	uint32_t silent_after_intro;
	uint32_t silent_max_session;
	uint32_t noise_max_intro;
	uint32_t noise_min_length;
	uint32_t noise_inter_silence;
	uint32_t noise_max_count;
	uint32_t total_analysis_time;
	uint32_t debug;

	uint32_t in_initial_silence:1;
	uint32_t in_intro:1;
	uint32_t complete:1;
	uint32_t max_intro_checked:1;  /* Flag to track if max-intro has been checked (only once for first word) */
	uint32_t silent_after_intro_checked:1;  /* Flag to track if silent-after-intro has been checked (only once after first word) */
	uint32_t talking:1;
	uint32_t had_silence_break:1;  /* Track if we had a silence break during intro */
	uint32_t codec_initialized:1;  /* Track if L16 codec is initialized */
} amd_vad_t;

static void fire_custom_event(switch_core_session_t *session, const char *action)
{
	switch_event_t *event;
	if (switch_event_create(&event, SWITCH_EVENT_CUSTOM) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Subclass", "AMD::EVENT");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Action", action);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
		switch_event_fire(&event);
	}
}

static void fire_media_bug_event(switch_core_session_t *session, const char *event_name)
{
	switch_event_t *event;
	if (switch_event_create(&event, SWITCH_EVENT_MESSAGE) == SWITCH_STATUS_SUCCESS) {
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Event-Name", event_name);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
		switch_event_fire(&event);
	}
}

static uint32_t get_silent_threshold(amd_vad_t *vad)
{
	return vad->silent_threshold ? vad->silent_threshold : globals.silent_threshold;
}

static uint32_t get_silent_initial(amd_vad_t *vad)
{
	return vad->silent_initial ? vad->silent_initial : globals.silent_initial;
}

static uint32_t get_silent_after_intro(amd_vad_t *vad)
{
	return vad->silent_after_intro ? vad->silent_after_intro : globals.silent_after_intro;
}

static uint32_t get_silent_max_session(amd_vad_t *vad)
{
	return vad->silent_max_session ? vad->silent_max_session : globals.silent_max_session;
}

static uint32_t get_noise_max_intro(amd_vad_t *vad)
{
	return vad->noise_max_intro ? vad->noise_max_intro : globals.noise_max_intro;
}

static uint32_t get_noise_min_length(amd_vad_t *vad)
{
	return vad->noise_min_length ? vad->noise_min_length : globals.noise_min_length;
}

static uint32_t get_noise_inter_silence(amd_vad_t *vad)
{
	return vad->noise_inter_silence ? vad->noise_inter_silence : globals.noise_inter_silence;
}

static uint32_t get_noise_max_count(amd_vad_t *vad)
{
	return vad->noise_max_count ? vad->noise_max_count : globals.noise_max_count;
}

static uint32_t get_total_analysis_time(amd_vad_t *vad)
{
	return vad->total_analysis_time ? vad->total_analysis_time : globals.total_analysis_time;
}

static uint32_t get_debug(amd_vad_t *vad)
{
	return vad->debug ? vad->debug : globals.debug;
}

/* Classify L16 PCM frame - assumes frame is already decoded to L16 */
static amd_frame_classifier classify_frame(const switch_frame_t *f, const switch_codec_implementation_t *codec, uint32_t threshold)
{
	int16_t *audio = (int16_t *)f->data;
	uint32_t score, count, j;
	double energy;
	int divisor;
	uint32_t channels;

	/* Match original mod_amd exactly - simple and straightforward */
	/* Reference: https://raw.githubusercontent.com/seanbright/mod_amd/master/mod_amd.c */
	/* silent_threshold: The level of volume to consider talking or not talking */
	/* score represents volume/energy - compare against threshold to classify */
	/* Note: Frame must already be decoded to L16 PCM before calling this function */

	/* Use codec info if available, otherwise use frame info */
	if (codec->actual_samples_per_second > 0) {
		divisor = codec->actual_samples_per_second / 8000;
	} else if (f->rate > 0) {
		divisor = f->rate / 8000;
	} else {
		divisor = 1; /* Default to 8000Hz */
	}

	channels = (codec->number_of_channels > 0) ? codec->number_of_channels : (f->channels > 0 ? f->channels : 1);

	/* Process as L16 PCM */
	for (energy = 0, j = 0, count = 0; count < f->samples; count++) {
		energy += abs(audio[j++]);
		j += channels - 1;  /* Skip other channels to get next sample of same channel (for mono: no skip, for stereo: skip 1) */
	}

	score = (uint32_t) (energy / (f->samples / divisor));

	if (score >= threshold) {
		return VOICED;
	}

	return SILENCE;
}

static switch_bool_t amd_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	amd_vad_t *vad = (amd_vad_t *) user_data;
	switch_frame_t *frame = NULL;

	if (!vad) {
		return SWITCH_TRUE;
	}

	/* Get session from bug and refresh channel pointer */
	if (!vad->session && bug) {
		vad->session = switch_core_media_bug_get_session(bug);
	}

	if (vad->complete) {
		return SWITCH_TRUE;
	}

	if (vad->session) {
		vad->channel = switch_core_session_get_channel(vad->session);
	}

	if (!vad->channel) {
		return SWITCH_TRUE;
	}

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		if (get_debug(vad)) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(vad->session),
				SWITCH_LOG_DEBUG,
				"AMD: Callback INIT\n");
		}
		break;

	case SWITCH_ABC_TYPE_READ:
		/* Use switch_core_media_bug_read() to read frames from the media bug */
		/* Only read frames when channel is ready */
		if (!switch_channel_ready(vad->channel)) {
			break;
		}

		{
			uint8_t frame_data[SWITCH_RECOMMENDED_BUFFER_SIZE];
			switch_frame_t read_frame = { 0 };
			switch_codec_implementation_t read_impl = { 0 };

			read_frame.data = frame_data;
			read_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

			/* Read frame from media bug - SWITCH_FALSE means non-blocking */
			/* Process ONE frame per callback invocation - FreeSWITCH will call us for each frame */
			if (switch_core_media_bug_read(bug, &read_frame, SWITCH_FALSE) == SWITCH_STATUS_SUCCESS) {
				/* Check if it's a valid audio frame */
				if (!switch_test_flag((&read_frame), SFF_CNG) &&
				    read_frame.datalen > 0 &&
				    read_frame.samples > 0) {

					/* Get codec implementation from session */
					/* Frames should already be in L16 format if codec was initialized */
					switch_core_session_get_read_impl(vad->session, &read_impl);

					/* If read_impl is invalid, populate it from frame info */
					if (read_impl.actual_samples_per_second == 0) {
						if (read_frame.rate > 0) {
							read_impl.actual_samples_per_second = read_frame.rate;
						} else {
							read_impl.actual_samples_per_second = 8000; /* Default */
						}
					}
					if (read_impl.number_of_channels == 0) {
						if (read_frame.channels > 0) {
							read_impl.number_of_channels = read_frame.channels;
						} else {
							read_impl.number_of_channels = 1; /* Default mono */
						}
					}

					/* Frame should already be in L16 format (set via switch_core_session_set_read_codec) */
					frame = &read_frame;

					if (get_debug(vad)) {
						switch_log_printf(
							SWITCH_CHANNEL_SESSION_LOG(vad->session),
							SWITCH_LOG_DEBUG,
							"AMD: Callback READ - frame=%p, samples=%d, datalen=%d, rate=%d, codec=%s\n",
							frame, frame->samples, frame->datalen, frame->rate,
							read_impl.iananame ? read_impl.iananame : "unknown");
					}

					/* Process frame immediately - calculate frame_ms and classify */
					if (read_impl.actual_samples_per_second > 0) {
						vad->frame_ms = 1000 / (read_impl.actual_samples_per_second / frame->samples);
					} else if (frame->rate > 0) {
						vad->frame_ms = 1000 / (frame->rate / frame->samples);
					} else {
						vad->frame_ms = 20; /* Default 20ms */
					}
					vad->total_duration += vad->frame_ms;

					if (vad->total_duration >= get_total_analysis_time(vad)) {
						if (get_debug(vad)) {
							switch_log_printf(
								SWITCH_CHANNEL_SESSION_LOG(vad->session),
								SWITCH_LOG_DEBUG,
								"AMD: Timeout - total_analysis_time exceeded\n");
						}
						switch_channel_set_variable(vad->channel, "amd_status", "unsure");
						switch_channel_set_variable(vad->channel, "amd_result", "too-long");
						vad->complete = SWITCH_TRUE;
						break;
					}

					/* Classify every frame - use per-session threshold if available */
					amd_frame_classifier frame_type = classify_frame(frame, &read_impl, get_silent_threshold(vad));

					if (get_debug(vad)) {
						switch_log_printf(
							SWITCH_CHANNEL_SESSION_LOG(vad->session),
							SWITCH_LOG_DEBUG,
							"AMD: Frame processed - type=%s, total_duration=%d, intro_voice=%d, intro_words=%d, silence=%d, words=%d\n",
							frame_type == VOICED ? "VOICED" : "SILENCE",
							vad->total_duration,
							vad->intro_voice_duration,
							vad->intro_words,
							vad->silence_duration,
							vad->words);
					}

					if (frame_type == VOICED) {
						if (!vad->talking) {
							vad->talking = 1;
							fire_custom_event(vad->session, "Start Talking");
						}

						vad->voice_duration += vad->frame_ms;
						vad->current_word_duration += vad->frame_ms;
						vad->silence_duration = 0;

						if (vad->in_initial_silence) {
							vad->in_initial_silence = 0;
							vad->in_intro = 1;
							vad->intro_voice_duration = vad->frame_ms;  /* Start counting from first voice frame */
							vad->had_silence_break = 0;  /* Reset silence break flag */
						} else if (vad->in_intro) {
							/* Only increment if we haven't had a silence break */
							if (vad->had_silence_break) {
								/* We had silence, so this is a new voice segment - reset intro tracking */
								vad->intro_voice_duration = vad->frame_ms;
								vad->had_silence_break = 0;  /* Reset flag for new voice segment */
							} else {
								/* Continuous voice - increment */
								vad->intro_voice_duration += vad->frame_ms;
							}
						}

						/* Exit intro period when we've passed the intro time window */
						if (vad->in_intro && vad->total_duration >= get_noise_max_intro(vad)) {
							vad->in_intro = 0;
							vad->max_intro_checked = 1;
						}

						/* Count word when transitioning from silence to voice (silence -> voice) */
						/* Word is counted when we have enough voice duration after silence */
						if (vad->current_word_duration >= get_noise_min_length(vad) && !vad->word_counted) {
							/* We have enough voice to count as a word - count it now (silence -> voice transition) */
							vad->words++;  /* Total word count (throughout detection) */

							/* Also count to intro_words if we're still in intro period */
							if (vad->total_duration < get_noise_max_intro(vad)) {
								vad->intro_words++;
							}

							if (get_debug(vad)) {
								switch_log_printf(
									SWITCH_CHANNEL_SESSION_LOG(vad->session),
									SWITCH_LOG_DEBUG,
									"AMD: Word detected (silence->voice transition) - words: %d, intro_words: %d, word_duration: %d, total_duration: %d\n",
									vad->words, vad->intro_words, vad->current_word_duration, vad->total_duration);
							}

							vad->word_counted = 1;
							vad->state = VAD_STATE_IN_WORD;
							vad->last_noise_start = vad->total_duration;
						}

						/* Check for machine detection based on max-intro logic */
						/* During intro period, check if the first word's length (duration) >= noise_max_intro */
						/* Only check once for the first word, and only during intro period */
						if (!vad->max_intro_checked && vad->words == 1 && vad->in_intro && vad->current_word_duration >= get_noise_max_intro(vad)) {
							if (get_debug(vad)) {
								switch_log_printf(
									SWITCH_CHANNEL_SESSION_LOG(vad->session),
									SWITCH_LOG_DEBUG,
									"AMD: Machine detected - max-intro (first word duration: %d, max_intro: %d, total_duration: %d)\n",
									vad->current_word_duration, get_noise_max_intro(vad), vad->total_duration);
							}
							switch_channel_set_variable(vad->channel, "amd_status", "machine");
							switch_channel_set_variable(vad->channel, "amd_result", "max-intro");
							vad->max_intro_checked = 1;
							vad->complete = SWITCH_TRUE;
							break;
						}

						/* Check for machine detection based on max-count logic */
						/* If total word count reaches noise_max_count at any time (including during intro), detect as machine */
						if (vad->words >= get_noise_max_count(vad)) {
							if (get_debug(vad)) {
								switch_log_printf(
									SWITCH_CHANNEL_SESSION_LOG(vad->session),
									SWITCH_LOG_DEBUG,
									"AMD: Machine detected - max-count (words: %d, max: %d, total_duration: %d)\n",
									vad->words, get_noise_max_count(vad), vad->total_duration);
							}
							switch_channel_set_variable(vad->channel, "amd_status", "machine");
							switch_channel_set_variable(vad->channel, "amd_result", "max-count");
							vad->complete = SWITCH_TRUE;
							break;
						}

						vad->last_noise_end = vad->total_duration;
					} else {
					if (vad->talking) {
						vad->talking = 0;
						fire_custom_event(vad->session, "Stop Talking");
					}

					vad->silence_duration += vad->frame_ms;
					vad->voice_duration = 0;

					/* Check if intro period has ended during silence */
					if (vad->in_intro && vad->total_duration >= get_noise_max_intro(vad)) {
						vad->in_intro = 0;
						vad->max_intro_checked = 1;
					}

					/* When silence is detected during intro, mark that we had a silence break */
					/* This ensures max-intro only counts continuous voice, not voice with breaks */
					if (vad->in_intro) {
						if (vad->silence_duration >= get_noise_inter_silence(vad)) {
							/* Word break detected - mark that we had silence */
							/* This will prevent max-intro from triggering on non-continuous voice */
							vad->had_silence_break = 1;
							vad->intro_voice_duration = 0;  /* Reset since we had a break */
						}
					}

					/* When we have silence >= noise_inter_silence, it's a word break */
					/* Reset word tracking for next potential word */
					if (vad->silence_duration >= get_noise_inter_silence(vad)) {
						vad->state = VAD_STATE_IN_SILENCE;
						/* Reset word duration and counting flag for next word */
						vad->current_word_duration = 0;
						vad->word_counted = 0;
					}

					if (vad->in_initial_silence && vad->silence_duration >= get_silent_initial(vad)) {
						if (get_debug(vad)) {
							switch_log_printf(
								SWITCH_CHANNEL_SESSION_LOG(vad->session),
								SWITCH_LOG_DEBUG,
								"AMD: Person detected - silent-initial (silence_duration: %d)\n",
								vad->silence_duration);
						}
						switch_channel_set_variable(vad->channel, "amd_status", "person");
						switch_channel_set_variable(vad->channel, "amd_result", "silent-initial");
						vad->complete = SWITCH_TRUE;
						break;
					}

					/* Check for person detection based on silent-after-intro logic */
					/* After the first word ends, check if silence length >= silent_after_intro */
					/* Only check once after the first word ends */
					if (!vad->silent_after_intro_checked && vad->words == 1 && vad->silence_duration >= get_silent_after_intro(vad)) {
						if (get_debug(vad)) {
							switch_log_printf(
								SWITCH_CHANNEL_SESSION_LOG(vad->session),
								SWITCH_LOG_DEBUG,
								"AMD: Person detected - silent-after-intro (silence_duration: %d, after first word)\n",
								vad->silence_duration);
						}
						switch_channel_set_variable(vad->channel, "amd_status", "person");
						switch_channel_set_variable(vad->channel, "amd_result", "silent-after-intro");
						vad->silent_after_intro_checked = 1;
						vad->complete = SWITCH_TRUE;
						break;
					}

						if (vad->silence_duration >= get_silent_max_session(vad) && vad->words > 0) {
							if (get_debug(vad)) {
								switch_log_printf(
									SWITCH_CHANNEL_SESSION_LOG(vad->session),
									SWITCH_LOG_DEBUG,
									"AMD: Person detected - silent_max_session reached\n");
							}
							switch_channel_set_variable(vad->channel, "amd_status", "person");
							switch_channel_set_variable(vad->channel, "amd_result", "silent-after-intro");
							vad->complete = SWITCH_TRUE;
							break;
						}
					}
				}
			}
		}
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		/* Cleanup on bug removal */
		if (vad) {
			if (get_debug(vad)) {
				switch_log_printf(
					SWITCH_CHANNEL_SESSION_LOG(vad->session),
					SWITCH_LOG_DEBUG,
					"AMD: Callback CLOSE\n");
			}
			if (vad->session) {
				vad->channel = switch_core_session_get_channel(vad->session);
				if (vad->channel) {
					switch_channel_set_variable(vad->channel, "amd_active", NULL);
				}
				const char *uuid = switch_core_session_get_uuid(vad->session);
				if (uuid && bug_hash_mutex) {
					switch_mutex_lock(bug_hash_mutex);
					switch_core_hash_delete(bug_hash, uuid);
					switch_mutex_unlock(bug_hash_mutex);
				}
			}
		}
		break;

	default:
		break;
	}

	return SWITCH_TRUE;
}


static void parse_amd_params(amd_vad_t *vad, const char *data)
{
	char *data_copy = NULL;
	char *p, *key, *val, *next;

	if (!data || !strlen(data)) {
		return;
	}

	data_copy = strdup(data);
	p = data_copy;

	while (p && *p) {
		/* Find the next comma or end of string */
		next = strchr(p, ',');
		if (next) {
			*next++ = '\0';
		}

		/* Find the equals sign */
		val = strchr(p, '=');
		if (val) {
			*val++ = '\0';
			key = p;

			/* Trim whitespace from key */
			while (*key == ' ' || *key == '\t' || *key == '\n' || *key == '\r') key++;
			{
				char *end = key + strlen(key) - 1;
				while (end > key && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) *end-- = '\0';
			}

			/* Trim whitespace from val */
			while (*val == ' ' || *val == '\t' || *val == '\n' || *val == '\r') val++;
			{
				char *end = val + strlen(val) - 1;
				while (end > val && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) *end-- = '\0';
			}

			if (!strcasecmp(key, "silent_threshold")) {
				vad->silent_threshold = atoi(val);
			} else if (!strcasecmp(key, "silent_initial")) {
				vad->silent_initial = atoi(val);
			} else if (!strcasecmp(key, "silent_after_intro")) {
				vad->silent_after_intro = atoi(val);
			} else if (!strcasecmp(key, "silent_max_session")) {
				vad->silent_max_session = atoi(val);
			} else if (!strcasecmp(key, "noise_max_intro")) {
				vad->noise_max_intro = atoi(val);
			} else if (!strcasecmp(key, "noise_min_length")) {
				vad->noise_min_length = atoi(val);
			} else if (!strcasecmp(key, "noise_inter_silence")) {
				vad->noise_inter_silence = atoi(val);
			} else if (!strcasecmp(key, "noise_max_count")) {
				vad->noise_max_count = atoi(val);
			} else if (!strcasecmp(key, "total_analysis_time")) {
				vad->total_analysis_time = atoi(val);
			} else if (!strcasecmp(key, "debug")) {
				vad->debug = atoi(val);
			}
		}

		p = next;
	}

	free(data_copy);
}

SWITCH_STANDARD_APP(voice_start_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	amd_vad_t *vad;
	switch_status_t status;

	if (!session) {
		return;
	}

	vad = (amd_vad_t *) switch_core_session_alloc(session, sizeof(amd_vad_t));
	memset(vad, 0, sizeof(amd_vad_t));

	vad->session = session;
	vad->channel = channel;
	vad->state = VAD_STATE_IN_SILENCE;
	vad->in_initial_silence = 1;
	vad->in_intro = 0;
	vad->complete = 0;
	vad->talking = 0;
	vad->current_word_duration = 0;
	vad->word_counted = 0;
	vad->had_silence_break = 0;
	vad->intro_words = 0;
	vad->words = 0;

	/* Parse per-session parameters if provided */
	if (data && strlen(data)) {
		parse_amd_params(vad, data);
	}

	/* Initialize L16 codec for receiving decoded frames */
	/* We create a new L16 (raw 16-bit samples) codec for the read end */
	/* This ensures we always receive frames in L16 format, regardless of the channel's codec */
	switch_codec_implementation_t read_impl = { 0 };
	switch_core_session_get_read_impl(session, &read_impl);

	if (read_impl.actual_samples_per_second > 0) {
		status = switch_core_codec_init(
			&vad->raw_codec,
			"L16",
			NULL,
			NULL,
			read_impl.actual_samples_per_second,
			read_impl.microseconds_per_packet / 1000,
			1,
			SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
			NULL,
			switch_core_session_get_pool(session));

		if (status == SWITCH_STATUS_SUCCESS) {
			/* Set the read codec to L16 so media bug receives L16 frames */
			switch_core_session_set_read_codec(session, &vad->raw_codec);
			vad->codec_initialized = 1;
		} else {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_WARNING,
				"AMD: Failed to initialize L16 codec, will process frames as-is\n");
		}
	}

	/* Media bug operations handle session locking internally */
	/* Use SMBF_READ_STREAM to get frames in callback */
	/* Frames will be in L16 format if codec was initialized successfully */
	status = switch_core_media_bug_add(session, "amd", NULL, amd_callback, vad, 0, SMBF_READ_STREAM, &bug);

	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_ERROR,
			"Failed to add media bug\n");
		return;
	}

	vad->bug = bug;
	/* Store destroy callback in user_data - cleanup will be handled in CLOSE */
	switch_channel_set_variable(channel, "amd_active", "true");

	/* Store bug pointer in hash table */
	{
		const char *uuid = switch_core_session_get_uuid(session);
		if (uuid && bug_hash_mutex) {
			switch_mutex_lock(bug_hash_mutex);
			switch_core_hash_insert(bug_hash, uuid, bug);
			switch_mutex_unlock(bug_hash_mutex);
		}
	}

	fire_media_bug_event(session, "SWITCH_MEDIA_BUG_ADD");

	if (get_debug(vad)) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_DEBUG,
			"AMD: Detection started\n");
	}
}

SWITCH_STANDARD_APP(voice_stop_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = NULL;
	const char *uuid = NULL;

	if (!session) {
		return;
	}

	uuid = switch_core_session_get_uuid(session);
	if (uuid && bug_hash_mutex) {
		switch_mutex_lock(bug_hash_mutex);
		bug = (switch_media_bug_t *)switch_core_hash_find(bug_hash, uuid);
		switch_mutex_unlock(bug_hash_mutex);
	}

	if (bug) {
		if (globals.debug) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"AMD: Removing media bug\n");
		}
		switch_core_media_bug_remove(session, &bug);
		if (bug_hash_mutex) {
			switch_mutex_lock(bug_hash_mutex);
			switch_core_hash_delete(bug_hash, uuid);
			switch_mutex_unlock(bug_hash_mutex);
		}
		switch_channel_set_variable(channel, "amd_active", NULL);
		fire_media_bug_event(session, "SWITCH_MEDIA_BUG_REMOVE");
		if (globals.debug) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session),
				SWITCH_LOG_DEBUG,
				"AMD: Detection stopped\n");
		}
	}
}

SWITCH_STANDARD_APP(waitforresult_function)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = NULL;
	const char *uuid = NULL;
	const char *var = NULL;

	if (!session) {
		return;
	}

	uuid = switch_core_session_get_uuid(session);
	if (uuid && bug_hash_mutex) {
		switch_mutex_lock(bug_hash_mutex);
		bug = (switch_media_bug_t *)switch_core_hash_find(bug_hash, uuid);
		switch_mutex_unlock(bug_hash_mutex);
	}

	if (!bug) {
		switch_log_printf(
			SWITCH_CHANNEL_SESSION_LOG(session),
			SWITCH_LOG_WARNING,
			"AMD: No active detection found\n");
		return;
	}

	while (switch_channel_ready(channel) && bug) {
		var = switch_channel_get_variable(channel, "amd_status");
		if (var && strlen(var)) {
			break;
		}
		switch_yield(10000);
		if (uuid && bug_hash_mutex) {
			switch_mutex_lock(bug_hash_mutex);
			bug = (switch_media_bug_t *)switch_core_hash_find(bug_hash, uuid);
			switch_mutex_unlock(bug_hash_mutex);
		} else {
			bug = NULL;
		}
	}

	var = switch_channel_get_variable(channel, "amd_status");
	if (var && !strcmp(var, "machine")) {
		const char *app = switch_channel_get_variable(channel, "execute_on_machine_app");
		const char *arg = switch_channel_get_variable(channel, "execute_on_machine_arg");
		if (app) {
			const char *debug_var = switch_channel_get_variable(channel, "amd_debug");
			uint32_t debug_val = debug_var ? atoi(debug_var) : globals.debug;
			if (debug_val) {
				switch_log_printf(
					SWITCH_CHANNEL_SESSION_LOG(session),
					SWITCH_LOG_DEBUG,
					"AMD: Executing %s %s\n",
					app, arg ? arg : "");
			}
			switch_core_session_execute_application(session, app, arg);
		}
	}
}
