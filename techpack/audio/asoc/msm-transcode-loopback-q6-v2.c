/* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/math64.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/timer.h>
#include <sound/tlv.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>
#include <sound/compress_driver.h>
#include <dsp/msm_audio_ion.h>
#include <dsp/apr_audio-v2.h>
#include <dsp/q6asm-v2.h>

#include "msm-pcm-routing-v2.h"
#include "msm-qti-pp-config.h"

#define LOOPBACK_SESSION_MAX_NUM_STREAMS 2

static DEFINE_MUTEX(transcode_loopback_session_lock);

struct trans_loopback_pdata {
	struct snd_compr_stream *cstream[MSM_FRONTEND_DAI_MAX];
};

struct loopback_stream {
	struct snd_compr_stream *cstream;
	uint32_t codec_format;
	bool start;
};

enum loopback_session_state {
	/* One or both streams not opened */
	LOOPBACK_SESSION_CLOSE = 0,
	/* Loopback streams opened */
	LOOPBACK_SESSION_READY,
	/* Loopback streams opened and formats configured */
	LOOPBACK_SESSION_START,
	/* Trigger issued on either of streams when in START state */
	LOOPBACK_SESSION_RUN
};

struct msm_transcode_loopback {
	struct loopback_stream source;
	struct loopback_stream sink;

	struct snd_compr_caps source_compr_cap;
	struct snd_compr_caps sink_compr_cap;

	uint32_t instance;
	uint32_t num_streams;
	int session_state;

	struct mutex lock;

	int session_id;
	struct audio_client *audio_client;
};

/* Transcode loopback global info struct */
static struct msm_transcode_loopback transcode_info;

static void loopback_event_handler(uint32_t opcode,
		uint32_t token, uint32_t *payload, void *priv)
{
	struct msm_transcode_loopback *trans =
			(struct msm_transcode_loopback *)priv;
	struct snd_soc_pcm_runtime *rtd;
	struct snd_compr_stream *cstream;
	struct audio_client *ac;
	int stream_id;
	int ret;

	if (!trans || !payload) {
		pr_err("%s: rtd or payload is NULL\n", __func__);
		return;
	}

	cstream = trans->source.cstream;
	ac = trans->audio_client;

	/*
	 * Token for rest of the compressed commands use to set
	 * session id, stream id, dir etc.
	 */
	stream_id = q6asm_get_stream_id_from_token(token);

	switch (opcode) {
	case ASM_STREAM_CMD_ENCDEC_EVENTS:
	case ASM_IEC_61937_MEDIA_FMT_EVENT:
		pr_debug("%s: ASM_IEC_61937_MEDIA_FMT_EVENT\n", __func__);
		rtd = cstream->private_data;
		if (!rtd) {
			pr_err("%s: rtd is NULL\n", __func__);
			return;
		}

		ret = msm_adsp_inform_mixer_ctl(rtd, payload);
		if (ret) {
			pr_err("%s: failed to inform mixer ctrl. err = %d\n",
				__func__, ret);
			return;
		}
		break;
	case APR_BASIC_RSP_RESULT: {
		switch (payload[0]) {
		case ASM_SESSION_CMD_RUN_V2:
			pr_debug("%s: ASM_SESSION_CMD_RUN_V2:", __func__);
			pr_debug("token 0x%x, stream id %d\n", token,
				  stream_id);
			break;
		case ASM_STREAM_CMD_CLOSE:
			pr_debug("%s: ASM_DATA_CMD_CLOSE:", __func__);
			pr_debug("token 0x%x, stream id %d\n", token,
				  stream_id);
			break;
		default:
			break;
		}
		break;
	}
	default:
		pr_debug("%s: Not Supported Event opcode[0x%x]\n",
			  __func__, opcode);
		break;
	}
}

static void populate_codec_list(struct msm_transcode_loopback *trans,
				struct snd_compr_stream *cstream)
{
	struct snd_compr_caps compr_cap;

	pr_debug("%s\n", __func__);

	memset(&compr_cap, 0, sizeof(struct snd_compr_caps));

	if (cstream->direction == SND_COMPRESS_CAPTURE) {
		compr_cap.direction = SND_COMPRESS_CAPTURE;
		compr_cap.num_codecs = 3;
		compr_cap.codecs[0] = SND_AUDIOCODEC_PCM;
		compr_cap.codecs[1] = SND_AUDIOCODEC_AC3;
		compr_cap.codecs[2] = SND_AUDIOCODEC_EAC3;
		memcpy(&trans->source_compr_cap, &compr_cap,
				sizeof(struct snd_compr_caps));
	}

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		compr_cap.direction = SND_COMPRESS_PLAYBACK;
		compr_cap.num_codecs = 1;
		compr_cap.codecs[0] = SND_AUDIOCODEC_PCM;
		memcpy(&trans->sink_compr_cap, &compr_cap,
				sizeof(struct snd_compr_caps));
	}
}

static int msm_transcode_loopback_open(struct snd_compr_stream *cstream)
{
	int ret = 0;
	struct snd_compr_runtime *runtime;
	struct snd_soc_pcm_runtime *rtd;
	struct msm_transcode_loopback *trans = &transcode_info;
	struct trans_loopback_pdata *pdata;

	if (cstream == NULL) {
		pr_err("%s: Invalid substream\n", __func__);
		return -EINVAL;
	}
	runtime = cstream->runtime;
	rtd = snd_pcm_substream_chip(cstream);
	pdata = snd_soc_platform_get_drvdata(rtd->platform);
	pdata->cstream[rtd->dai_link->id] = cstream;

	mutex_lock(&trans->lock);
	if (trans->num_streams > LOOPBACK_SESSION_MAX_NUM_STREAMS) {
		pr_err("msm_transcode_open failed..invalid stream\n");
		ret = -EINVAL;
		goto exit;
	}

	if (cstream->direction == SND_COMPRESS_CAPTURE) {
		if (trans->source.cstream == NULL) {
			trans->source.cstream = cstream;
			trans->num_streams++;
		} else {
			pr_err("%s: capture stream already opened\n",
				__func__);
			ret = -EINVAL;
			goto exit;
		}
	} else if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		if (trans->sink.cstream == NULL) {
			trans->sink.cstream = cstream;
			trans->num_streams++;
		} else {
			pr_debug("%s: playback stream already opened\n",
				__func__);
			ret = -EINVAL;
			goto exit;
		}
	}

	pr_debug("%s: num stream%d, stream name %s\n", __func__,
		 trans->num_streams, cstream->name);

	populate_codec_list(trans, cstream);

	if (trans->num_streams == LOOPBACK_SESSION_MAX_NUM_STREAMS)	{
		pr_debug("%s: Moving loopback session to READY state %d\n",
			 __func__, trans->session_state);
		trans->session_state = LOOPBACK_SESSION_READY;
	}

	runtime->private_data = trans;
	if (trans->num_streams == 1)
		msm_adsp_init_mixer_ctl_pp_event_queue(rtd);
exit:
	mutex_unlock(&trans->lock);
	return ret;
}

static void stop_transcoding(struct msm_transcode_loopback *trans)
{
	struct snd_soc_pcm_runtime *soc_pcm_rx;
	struct snd_soc_pcm_runtime *soc_pcm_tx;

	if (trans->audio_client != NULL) {
		q6asm_cmd(trans->audio_client, CMD_CLOSE);

		if (trans->sink.cstream != NULL) {
			soc_pcm_rx = trans->sink.cstream->private_data;
			msm_pcm_routing_dereg_phy_stream(
					soc_pcm_rx->dai_link->id,
					SND_COMPRESS_PLAYBACK);
		}
		if (trans->source.cstream != NULL) {
			soc_pcm_tx = trans->source.cstream->private_data;
			msm_pcm_routing_dereg_phy_stream(
					soc_pcm_tx->dai_link->id,
					SND_COMPRESS_CAPTURE);
		}
		q6asm_audio_client_free(trans->audio_client);
		trans->audio_client = NULL;
	}
}

static int msm_transcode_loopback_free(struct snd_compr_stream *cstream)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_transcode_loopback *trans = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(cstream);
	int ret = 0;

	mutex_lock(&trans->lock);

	pr_debug("%s: Transcode loopback end:%d, streams %d\n", __func__,
		  cstream->direction, trans->num_streams);
	trans->num_streams--;
	stop_transcoding(trans);

	if (cstream->direction == SND_COMPRESS_PLAYBACK)
		memset(&trans->sink, 0, sizeof(struct loopback_stream));
	else if (cstream->direction == SND_COMPRESS_CAPTURE)
		memset(&trans->source, 0, sizeof(struct loopback_stream));

	trans->session_state = LOOPBACK_SESSION_CLOSE;
	if (trans->num_streams == 1)
		msm_adsp_clean_mixer_ctl_pp_event_queue(rtd);
	mutex_unlock(&trans->lock);
	return ret;
}

static int msm_transcode_loopback_trigger(struct snd_compr_stream *cstream,
					  int cmd)
{
	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_transcode_loopback *trans = runtime->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:

		if (trans->session_state == LOOPBACK_SESSION_START) {
			pr_debug("%s: Issue Loopback session %d RUN\n",
				  __func__, trans->instance);
			q6asm_run_nowait(trans->audio_client, 0, 0, 0);
			trans->session_state = LOOPBACK_SESSION_RUN;
		}
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("%s: Issue Loopback session %d STOP\n", __func__,
			  trans->instance);
		if (trans->session_state == LOOPBACK_SESSION_RUN)
			q6asm_cmd_nowait(trans->audio_client, CMD_PAUSE);
		trans->session_state = LOOPBACK_SESSION_START;
		break;

	default:
		break;
	}
	return 0;
}

static int msm_transcode_loopback_set_params(struct snd_compr_stream *cstream,
				struct snd_compr_params *codec_param)
{

	struct snd_compr_runtime *runtime = cstream->runtime;
	struct msm_transcode_loopback *trans = runtime->private_data;
	struct snd_soc_pcm_runtime *soc_pcm_rx;
	struct snd_soc_pcm_runtime *soc_pcm_tx;
	uint32_t bit_width = 16;
	int ret = 0;

	if (trans == NULL) {
		pr_err("%s: Invalid param\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&trans->lock);

	if (cstream->direction == SND_COMPRESS_PLAYBACK) {
		if (codec_param->codec.id == SND_AUDIOCODEC_PCM) {
			trans->sink.codec_format =
				FORMAT_LINEAR_PCM;
			switch (codec_param->codec.format) {
			case SNDRV_PCM_FORMAT_S32_LE:
				bit_width = 32;
				break;
			case SNDRV_PCM_FORMAT_S24_LE:
				bit_width = 24;
				break;
			case SNDRV_PCM_FORMAT_S24_3LE:
				bit_width = 24;
				break;
			case SNDRV_PCM_FORMAT_S16_LE:
			default:
				bit_width = 16;
				break;
			}
		} else {
			pr_debug("%s: unknown sink codec\n", __func__);
			ret = -EINVAL;
			goto exit;
		}
		trans->sink.start = true;
	}

	if (cstream->direction == SND_COMPRESS_CAPTURE) {
		switch (codec_param->codec.id) {
		case SND_AUDIOCODEC_PCM:
			pr_debug("Source SND_AUDIOCODEC_PCM\n");
			trans->source.codec_format =
				FORMAT_LINEAR_PCM;
			break;
		case SND_AUDIOCODEC_AC3:
			pr_debug("Source SND_AUDIOCODEC_AC3\n");
			trans->source.codec_format =
				FORMAT_AC3;
			break;
		case SND_AUDIOCODEC_EAC3:
			pr_debug("Source SND_AUDIOCODEC_EAC3\n");
			trans->source.codec_format =
				FORMAT_EAC3;
			break;
		default:
			pr_debug("%s: unknown source codec\n", __func__);
			ret = -EINVAL;
			goto exit;
		}
		trans->source.start = true;
	}

	pr_debug("%s: trans->source.start %d trans->sink.start %d trans->source.cstream %pK trans->sink.cstream %pK trans->session_state %d\n",
			__func__, trans->source.start, trans->sink.start,
			trans->source.cstream, trans->sink.cstream,
			trans->session_state);

	if ((trans->session_state == LOOPBACK_SESSION_READY) &&
			trans->source.start && trans->sink.start) {
		pr_debug("%s: Moving loopback session to start state\n",
			  __func__);
		trans->session_state = LOOPBACK_SESSION_START;
	}

	if (trans->session_state == LOOPBACK_SESSION_START) {
		if (trans->audio_client != NULL) {
			pr_debug("%s: ASM client already opened, closing\n",
				 __func__);
			stop_transcoding(trans);
		}

		trans->audio_client = q6asm_audio_client_alloc(
				(app_cb)loopback_event_handler, trans);
		if (!trans->audio_client) {
			pr_err("%s: Could not allocate memory\n", __func__);
			ret = -EINVAL;
			goto exit;
		}
		pr_debug("%s: ASM client allocated, callback %pK\n", __func__,
						loopback_event_handler);
		trans->session_id = trans->audio_client->session;
		trans->audio_client->perf_mode = false;
		ret = q6asm_open_transcode_loopback(trans->audio_client,
					bit_width,
					trans->source.codec_format,
					trans->sink.codec_format);
		if (ret < 0) {
			pr_err("%s: Session transcode loopback open failed\n",
				__func__);
			q6asm_audio_client_free(trans->audio_client);
			trans->audio_client = NULL;
			goto exit;
		}

		pr_debug("%s: Starting ADM open for loopback\n", __func__);
		soc_pcm_rx = trans->sink.cstream->private_data;
		soc_pcm_tx = trans->source.cstream->private_data;
		if (trans->source.codec_format != FORMAT_LINEAR_PCM)
			msm_pcm_routing_reg_phy_compr_stream(
					soc_pcm_tx->dai_link->id,
					trans->audio_client->perf_mode,
					trans->session_id,
					SNDRV_PCM_STREAM_CAPTURE,
					true);
		else
			msm_pcm_routing_reg_phy_stream(
					soc_pcm_tx->dai_link->id,
					trans->audio_client->perf_mode,
					trans->session_id,
					SNDRV_PCM_STREAM_CAPTURE);

		msm_pcm_routing_reg_phy_stream(
					soc_pcm_rx->dai_link->id,
					trans->audio_client->perf_mode,
					trans->session_id,
					SNDRV_PCM_STREAM_PLAYBACK);
		pr_debug("%s: Successfully opened ADM sessions\n", __func__);
	}
exit:
	mutex_unlock(&trans->lock);
	return ret;
}

static int msm_transcode_loopback_get_caps(struct snd_compr_stream *cstream,
				struct snd_compr_caps *arg)
{
	struct snd_compr_runtime *runtime;
	struct msm_transcode_loopback *trans;

	if (!arg || !cstream) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -EINVAL;
	}

	runtime = cstream->runtime;
	trans = runtime->private_data;
	pr_debug("%s\n", __func__);
	if (cstream->direction == SND_COMPRESS_CAPTURE)
		memcpy(arg, &trans->source_compr_cap,
		       sizeof(struct snd_compr_caps));
	else
		memcpy(arg, &trans->sink_compr_cap,
		       sizeof(struct snd_compr_caps));
	return 0;
}

static int msm_transcode_stream_cmd_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	unsigned long fe_id = kcontrol->private_value;
	struct trans_loopback_pdata *pdata = (struct trans_loopback_pdata *)
				snd_soc_component_get_drvdata(comp);
	struct snd_compr_stream *cstream = NULL;
	struct msm_transcode_loopback *prtd;
	int ret = 0;
	struct msm_adsp_event_data *event_data = NULL;
	uint64_t actual_payload_len = 0;

	if (fe_id >= MSM_FRONTEND_DAI_MAX) {
		pr_err("%s Received invalid fe_id %lu\n",
			__func__, fe_id);
		ret = -EINVAL;
		goto done;
	}

	cstream = pdata->cstream[fe_id];
	if (cstream == NULL) {
		pr_err("%s cstream is null.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	prtd = cstream->runtime->private_data;
	if (!prtd) {
		pr_err("%s: prtd is null.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	if (prtd->audio_client == NULL) {
		pr_err("%s: audio_client is null.\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	event_data = (struct msm_adsp_event_data *)ucontrol->value.bytes.data;
	if ((event_data->event_type < ADSP_STREAM_PP_EVENT) ||
	    (event_data->event_type >= ADSP_STREAM_EVENT_MAX)) {
		pr_err("%s: invalid event_type=%d",
			 __func__, event_data->event_type);
		ret = -EINVAL;
		goto done;
	}

	actual_payload_len = sizeof(struct msm_adsp_event_data) +
		event_data->payload_len;
	if (actual_payload_len >= U32_MAX) {
		pr_err("%s payload length 0x%X  exceeds limit",
				__func__, event_data->payload_len);
		ret = -EINVAL;
		goto done;
	}


	if (event_data->payload_len > sizeof(ucontrol->value.bytes.data)
					- sizeof(struct msm_adsp_event_data)) {
		pr_err("%s param length=%d  exceeds limit",
			 __func__, event_data->payload_len);
		ret = -EINVAL;
		goto done;
	}

	ret = q6asm_send_stream_cmd(prtd->audio_client, event_data);
	if (ret < 0)
		pr_err("%s: failed to send stream event cmd, err = %d\n",
			__func__, ret);
done:
	return ret;
}

static int msm_transcode_ion_fd_map_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	unsigned long fe_id = kcontrol->private_value;
	struct trans_loopback_pdata *pdata = (struct trans_loopback_pdata *)
				snd_soc_component_get_drvdata(comp);
	struct snd_compr_stream *cstream = NULL;
	struct msm_transcode_loopback *prtd;
	int fd;
	int ret = 0;

	if (fe_id >= MSM_FRONTEND_DAI_MAX) {
		pr_err("%s Received out of bounds invalid fe_id %lu\n",
			__func__, fe_id);
		ret = -EINVAL;
		goto done;
	}

	cstream = pdata->cstream[fe_id];
	if (cstream == NULL) {
		pr_err("%s cstream is null\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	prtd = cstream->runtime->private_data;
	if (!prtd) {
		pr_err("%s: prtd is null\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	if (prtd->audio_client == NULL) {
		pr_err("%s: audio_client is null\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	memcpy(&fd, ucontrol->value.bytes.data, sizeof(fd));
	ret = q6asm_send_ion_fd(prtd->audio_client, fd);
	if (ret < 0)
		pr_err("%s: failed to register ion fd\n", __func__);
done:
	return ret;
}

static int msm_transcode_rtic_event_ack_put(struct snd_kcontrol *kcontrol,
					struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kcontrol);
	unsigned long fe_id = kcontrol->private_value;
	struct trans_loopback_pdata *pdata = (struct trans_loopback_pdata *)
					snd_soc_component_get_drvdata(comp);
	struct snd_compr_stream *cstream = NULL;
	struct msm_transcode_loopback *prtd;
	int ret = 0;
	int param_length = 0;

	if (fe_id >= MSM_FRONTEND_DAI_MAX) {
		pr_err("%s Received invalid fe_id %lu\n",
			__func__, fe_id);
		ret = -EINVAL;
		goto done;
	}

	cstream = pdata->cstream[fe_id];
	if (cstream == NULL) {
		pr_err("%s cstream is null\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	prtd = cstream->runtime->private_data;
	if (!prtd) {
		pr_err("%s: prtd is null\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	if (prtd->audio_client == NULL) {
		pr_err("%s: audio_client is null\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	memcpy(&param_length, ucontrol->value.bytes.data,
		sizeof(param_length));
	if ((param_length + sizeof(param_length))
		>= sizeof(ucontrol->value.bytes.data)) {
		pr_err("%s param length=%d  exceeds limit",
			__func__, param_length);
		ret = -EINVAL;
		goto done;
	}

	ret = q6asm_send_rtic_event_ack(prtd->audio_client,
			ucontrol->value.bytes.data + sizeof(param_length),
			param_length);
	if (ret < 0)
		pr_err("%s: failed to send rtic event ack, err = %d\n",
			__func__, ret);
done:
	return ret;
}

static int msm_transcode_stream_cmd_control(
			struct snd_soc_pcm_runtime *rtd)
{
	const char *mixer_ctl_name = DSP_STREAM_CMD;
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct snd_kcontrol_new fe_loopback_stream_cmd_config_control[1] = {
		{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "?",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = msm_adsp_stream_cmd_info,
		.put = msm_transcode_stream_cmd_put,
		.private_value = 0,
		}
	};

	if (!rtd) {
		pr_err("%s NULL rtd\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -ENOMEM;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name, rtd->pcm->device);
	fe_loopback_stream_cmd_config_control[0].name = mixer_str;
	fe_loopback_stream_cmd_config_control[0].private_value =
				rtd->dai_link->id;
	pr_debug("%s: Registering new mixer ctl %s\n", __func__, mixer_str);
	ret = snd_soc_add_platform_controls(rtd->platform,
		fe_loopback_stream_cmd_config_control,
		ARRAY_SIZE(fe_loopback_stream_cmd_config_control));
	if (ret < 0)
		pr_err("%s: failed to add ctl %s. err = %d\n",
			__func__, mixer_str, ret);

	kfree(mixer_str);
done:
	return ret;
}

static int msm_transcode_stream_callback_control(
			struct snd_soc_pcm_runtime *rtd)
{
	const char *mixer_ctl_name = DSP_STREAM_CALLBACK;
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct snd_kcontrol *kctl;

	struct snd_kcontrol_new fe_loopback_callback_config_control[1] = {
		{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "?",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = msm_adsp_stream_callback_info,
		.get = msm_adsp_stream_callback_get,
		.private_value = 0,
		}
	};

	if (!rtd) {
		pr_err("%s: rtd is  NULL\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -ENOMEM;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name, rtd->pcm->device);
	fe_loopback_callback_config_control[0].name = mixer_str;
	fe_loopback_callback_config_control[0].private_value =
					rtd->dai_link->id;
	pr_debug("%s: Registering new mixer ctl %s\n", __func__, mixer_str);
	ret = snd_soc_add_platform_controls(rtd->platform,
			fe_loopback_callback_config_control,
			ARRAY_SIZE(fe_loopback_callback_config_control));
	if (ret < 0) {
		pr_err("%s: failed to add ctl %s. err = %d\n",
			__func__, mixer_str, ret);
		ret = -EINVAL;
		goto free_mixer_str;
	}

	kctl = snd_soc_card_get_kcontrol(rtd->card, mixer_str);
	if (!kctl) {
		pr_err("%s: failed to get kctl %s.\n", __func__, mixer_str);
		ret = -EINVAL;
		goto free_mixer_str;
	}

	kctl->private_data = NULL;
free_mixer_str:
	kfree(mixer_str);
done:
	return ret;
}

static int msm_transcode_add_ion_fd_cmd_control(struct snd_soc_pcm_runtime *rtd)
{
	const char *mixer_ctl_name = "Playback ION FD";
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct snd_kcontrol_new fe_ion_fd_config_control[1] = {
		{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "?",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = msm_adsp_stream_cmd_info,
		.put = msm_transcode_ion_fd_map_put,
		.private_value = 0,
		}
	};

	if (!rtd) {
		pr_err("%s NULL rtd\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -ENOMEM;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name, rtd->pcm->device);
	fe_ion_fd_config_control[0].name = mixer_str;
	fe_ion_fd_config_control[0].private_value = rtd->dai_link->id;
	pr_debug("%s: Registering new mixer ctl %s\n", __func__, mixer_str);
	ret = snd_soc_add_platform_controls(rtd->platform,
				fe_ion_fd_config_control,
				ARRAY_SIZE(fe_ion_fd_config_control));
	if (ret < 0)
		pr_err("%s: failed to add ctl %s\n", __func__, mixer_str);

	kfree(mixer_str);
done:
	return ret;
}

static int msm_transcode_add_event_ack_cmd_control(
					struct snd_soc_pcm_runtime *rtd)
{
	const char *mixer_ctl_name = "Playback Event Ack";
	const char *deviceNo = "NN";
	char *mixer_str = NULL;
	int ctl_len = 0, ret = 0;
	struct snd_kcontrol_new fe_event_ack_config_control[1] = {
		{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "?",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
		.info = msm_adsp_stream_cmd_info,
		.put = msm_transcode_rtic_event_ack_put,
		.private_value = 0,
		}
	};

	if (!rtd) {
		pr_err("%s NULL rtd\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	ctl_len = strlen(mixer_ctl_name) + 1 + strlen(deviceNo) + 1;
	mixer_str = kzalloc(ctl_len, GFP_KERNEL);
	if (!mixer_str) {
		ret = -ENOMEM;
		goto done;
	}

	snprintf(mixer_str, ctl_len, "%s %d", mixer_ctl_name, rtd->pcm->device);
	fe_event_ack_config_control[0].name = mixer_str;
	fe_event_ack_config_control[0].private_value = rtd->dai_link->id;
	pr_debug("%s: Registering new mixer ctl %s\n", __func__, mixer_str);
	ret = snd_soc_add_platform_controls(rtd->platform,
				fe_event_ack_config_control,
				ARRAY_SIZE(fe_event_ack_config_control));
	if (ret < 0)
		pr_err("%s: failed to add ctl %s\n", __func__, mixer_str);

	kfree(mixer_str);
done:
	return ret;
}

static int msm_transcode_loopback_new(struct snd_soc_pcm_runtime *rtd)
{
	int rc;

	rc = msm_transcode_stream_cmd_control(rtd);
	if (rc)
		pr_err("%s: ADSP Stream Cmd Control open failed\n", __func__);

	rc = msm_transcode_stream_callback_control(rtd);
	if (rc)
		pr_err("%s: ADSP Stream callback Control open failed\n",
			__func__);

	rc = msm_transcode_add_ion_fd_cmd_control(rtd);
	if (rc)
		pr_err("%s: Could not add transcode ion fd Control\n",
			__func__);

	rc = msm_transcode_add_event_ack_cmd_control(rtd);
	if (rc)
		pr_err("%s: Could not add transcode event ack Control\n",
			__func__);

	return 0;
}

static struct snd_compr_ops msm_transcode_loopback_ops = {
	.open			= msm_transcode_loopback_open,
	.free			= msm_transcode_loopback_free,
	.trigger		= msm_transcode_loopback_trigger,
	.set_params		= msm_transcode_loopback_set_params,
	.get_caps		= msm_transcode_loopback_get_caps,
};


static int msm_transcode_loopback_probe(struct snd_soc_platform *platform)
{
	struct trans_loopback_pdata *pdata = NULL;

	pr_debug("%s\n", __func__);
	pdata = (struct trans_loopback_pdata *)
			kzalloc(sizeof(struct trans_loopback_pdata),
			GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	snd_soc_platform_set_drvdata(platform, pdata);
	return 0;
}

static struct snd_soc_platform_driver msm_soc_platform = {
	.probe		= msm_transcode_loopback_probe,
	.compr_ops	= &msm_transcode_loopback_ops,
	.pcm_new	= msm_transcode_loopback_new,
};

static int msm_transcode_dev_probe(struct platform_device *pdev)
{

	pr_debug("%s: dev name %s\n", __func__, dev_name(&pdev->dev));
	if (pdev->dev.of_node)
		dev_set_name(&pdev->dev, "%s", "msm-transcode-loopback");

	return snd_soc_register_platform(&pdev->dev,
					&msm_soc_platform);
}

static int msm_transcode_remove(struct platform_device *pdev)
{
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static const struct of_device_id msm_transcode_loopback_dt_match[] = {
	{.compatible = "qcom,msm-transcode-loopback"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_transcode_loopback_dt_match);

static struct platform_driver msm_transcode_loopback_driver = {
	.driver = {
		.name = "msm-transcode-loopback",
		.owner = THIS_MODULE,
		.of_match_table = msm_transcode_loopback_dt_match,
	},
	.probe = msm_transcode_dev_probe,
	.remove = msm_transcode_remove,
};

static int __init msm_soc_platform_init(void)
{
	memset(&transcode_info, 0, sizeof(struct msm_transcode_loopback));
	mutex_init(&transcode_info.lock);
	return platform_driver_register(&msm_transcode_loopback_driver);
}
module_init(msm_soc_platform_init);

static void __exit msm_soc_platform_exit(void)
{
	mutex_destroy(&transcode_info.lock);
	platform_driver_unregister(&msm_transcode_loopback_driver);
}
module_exit(msm_soc_platform_exit);

MODULE_DESCRIPTION("Transcode loopback platform driver");
MODULE_LICENSE("GPL v2");
