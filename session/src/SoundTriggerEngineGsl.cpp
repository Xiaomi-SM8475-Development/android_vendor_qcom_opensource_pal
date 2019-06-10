/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "SoundTriggerEngineGsl"

#include "SoundTriggerEngineGsl.h"
#include "Session.h"
#include "SessionGsl.h"
#include "Stream.h"
#include "StreamSoundTrigger.h"
#include "kvh2xml.h"

#define HIST_BUFFER_DURATION_MS 1750
#define PRE_ROLL_DURATION_IN_MS 250
#define DWNSTRM_SETUP_DURATION_MS 300

std::shared_ptr<SoundTriggerEngineGsl> SoundTriggerEngineGsl::sndEngGsl_ = NULL;

void SoundTriggerEngineGsl::buffer_thread_loop()
{
    QAL_INFO(LOG_TAG, "start thread loop");
    std::unique_lock<std::mutex> lck(sndEngGsl_->mutex_);
    while (!sndEngGsl_->exit_thread_)
    {
        sndEngGsl_->exit_buffering_ = false;
        QAL_VERBOSE(LOG_TAG,"%s: waiting on cond", __func__);
        /* Wait for keyword buffer data from DSP */
        if (!sndEngGsl_->eventDetected)
            sndEngGsl_->cv_.wait(lck);
        QAL_INFO(LOG_TAG,"%s: done waiting on cond, exit_buffering = %d", __func__,
            sndEngGsl_->exit_buffering_);

        if (sndEngGsl_->exit_thread_) {
            break;
        }

        if (sndEngGsl_->exit_buffering_)
        {
            continue; /* skip over processing if we want to exit already*/
        }

        if (sndEngGsl_->start_buffering()) {
            break;
        }
    }
}

int32_t SoundTriggerEngineGsl::prepare_sound_engine()
{
    int32_t status = 0;
    return status;
}

int32_t SoundTriggerEngineGsl::start_sound_engine()
{
    int32_t status = 0;
    bufferThreadHandler_ = std::thread(SoundTriggerEngineGsl::buffer_thread_loop);

    if (!bufferThreadHandler_.joinable())
    {
        status = -EINVAL;
        QAL_ERR(LOG_TAG, "%s: failed to create buffer thread = %d", __func__, status);
    }
    return status;
}

int32_t SoundTriggerEngineGsl::stop_sound_engine()
{
    int32_t status = 0;
    std::lock_guard<std::mutex> lck(sndEngGsl_->mutex_);
    exit_thread_ = true;
    exit_buffering_ = true;
    cv_.notify_one();
    bufferThreadHandler_.join();
    return status;
}

int32_t SoundTriggerEngineGsl::start_buffering()
{
    QAL_INFO(LOG_TAG, "Enter");
    int32_t status = 0;
    int32_t size;
    struct qal_buffer buf;
    size_t inputBufSize;
    size_t inputBufNum;
    size_t outputBufSize;
    size_t outputBufNum;
    size_t toWrite = 0;

    streamHandle->getBufInfo(&inputBufSize, &inputBufNum, &outputBufSize, &outputBufNum);
    memset(&buf, 0, sizeof(struct qal_buffer));
    buf.size = inputBufSize * inputBufNum;
    buf.buffer = (uint8_t *)calloc(1, buf.size);

    /*TODO: add max retry num to avoid dead lock*/
    QAL_VERBOSE(LOG_TAG, "trying to read %u from gsl", buf.size);

    // read data from session
    if (!toWrite && buffer_->getFreeSize() >= buf.size)
    {
        status = session->read(streamHandle, SHMEM_ENDPOINT, &buf, &size);
        QAL_INFO(LOG_TAG, "%d read from session, %u to be read", size, buf.size);
        toWrite = size;
    }

    // write data to ring buffer
    if (toWrite)
    {
        size_t ret = buffer_->write(buf.buffer, toWrite);
        toWrite -= ret;
        QAL_INFO(LOG_TAG, "%u written to ring buffer", ret);
    }

    if (buf.buffer)
        free(buf.buffer);
    return status;
}

int32_t SoundTriggerEngineGsl::start_keyword_detection()
{
    int32_t status = 0;
    return status;
}

SoundTriggerEngineGsl::SoundTriggerEngineGsl(Stream *s, uint32_t id, uint32_t stage_id, QalRingBufferReader **reader, std::shared_ptr<QalRingBuffer> buffer)
{
    struct qal_stream_attributes sAttr;
    uint32_t sampleRate;
    uint32_t bitWidth;
    uint32_t channels;
    uint32_t bufferSize = DEFAULT_QAL_RING_BUFFER_SIZE;
    engineId = id;
    stageId = stage_id;
    eventDetected = false;
    exit_thread_ = false;
    exit_buffering_ = false;
    sndEngGsl_ = (std::shared_ptr<SoundTriggerEngineGsl>)this;
    s->getAssociatedSession(&session);
    streamHandle = s;
    sm_data = NULL;
    pSetupDuration = NULL;
    pSoundModel = NULL;

    // Create ring buffer when reader passed is not specified
    if (!buffer)
    {
        QAL_INFO(LOG_TAG, "creating new ring buffer");
        struct qal_stream_attributes sAttr;
        s->getStreamAttributes(&sAttr);
        if (sAttr.direction == QAL_AUDIO_INPUT)
        {
            sampleRate = sAttr.in_media_config.sample_rate;
            bitWidth = sAttr.in_media_config.bit_width;
            channels = sAttr.in_media_config.ch_info->channels;
            // ring buffer size equals to 3s' audio data
            // as second stage may need 2-2.5s data to detect
            bufferSize = sampleRate * bitWidth * channels *
                         RING_BUFFER_DURATION / BITS_PER_BYTE;
        }

        buffer_ = new QalRingBuffer(bufferSize);
        reader_ = NULL;
        *reader = buffer_->newReader();
    }
    else
    {
        // Avoid this engine write data to existing ring buffer
        buffer_ = NULL;
        reader_ = buffer->newReader();
    }
}

SoundTriggerEngineGsl::~SoundTriggerEngineGsl()
{
    if (sm_data)
        free(sm_data);

    if (pSetupDuration)
        free(pSetupDuration);
}

int32_t SoundTriggerEngineGsl::load_sound_model(Stream *s, uint8_t *data, uint32_t num_models)
{
    int32_t status = 0;
    struct qal_st_phrase_sound_model *phrase_sm = NULL;
    struct qal_st_sound_model *common_sm = NULL;
    SML_BigSoundModelTypeV3 *big_sm;
    uint8_t *sm_payload = NULL;

    if (!data)
    {
        QAL_ERR(LOG_TAG, "%s: Invalid sound model data", __func__);
        status = -EINVAL;
        goto exit;
    }

    common_sm = (struct qal_st_sound_model *)data;
    if (common_sm->type == QAL_SOUND_MODEL_TYPE_KEYPHRASE)
    {
        phrase_sm = (struct qal_st_phrase_sound_model *)data;
        if (num_models > 1)
        {
            sm_payload = (uint8_t *)common_sm + common_sm->data_offset;
            for (int i = 0; i < num_models; i++)
            {
                big_sm = (SML_BigSoundModelTypeV3 *)(sm_payload + sizeof(SML_GlobalHeaderType) +
                    sizeof(SML_HeaderTypeV3) + (i * sizeof(SML_BigSoundModelTypeV3)));

                if (big_sm->type == ST_SM_ID_SVA_GMM)
                {
                    sm_data_size = big_sm->size + sizeof(struct qal_st_phrase_sound_model);
                    sm_data = (uint8_t *)malloc(sm_data_size);
                    memcpy(sm_data, (char *)phrase_sm, sizeof(*phrase_sm));
                    common_sm = (struct qal_st_sound_model *)sm_data;
                    common_sm->data_size = big_sm->size;
                    common_sm->data_offset += sizeof(SML_GlobalHeaderType) + sizeof(SML_HeaderTypeV3) +
                        (num_models * sizeof(SML_BigSoundModelTypeV3)) + big_sm->offset;
                    memcpy(sm_data + sizeof(*phrase_sm),
                           (char *)phrase_sm + common_sm->data_offset,
                           common_sm->data_size);
                    common_sm->data_offset = sizeof(struct qal_st_phrase_sound_model);
                    common_sm = (struct qal_st_sound_model *)data;
                    break;
                }
            }
        }
        else
        {
            sm_data_size = common_sm->data_size +
                       sizeof(struct qal_st_phrase_sound_model);
            sm_data = (uint8_t *)malloc(sm_data_size);
            memcpy(sm_data, (char *)phrase_sm, sizeof(*phrase_sm));
            memcpy(sm_data + sizeof(*phrase_sm),
                   (char *)phrase_sm + phrase_sm->common.data_offset,
                   phrase_sm->common.data_size);
        }
    }
    else if (common_sm->type == QAL_SOUND_MODEL_TYPE_GENERIC)
    {
        sm_data_size = common_sm->data_size +
                       sizeof(struct qal_st_sound_model);
        sm_data = (uint8_t *)malloc(sm_data_size);
        memcpy(sm_data, (char *)common_sm, sizeof(*common_sm));
        memcpy(sm_data + sizeof(*common_sm),
               (char *)common_sm + common_sm->data_offset,
               common_sm->data_size);
    }
    pSoundModel = (struct qal_st_sound_model *)sm_data;

    status = session->setParameters(streamHandle, PARAM_ID_DETECTION_ENGINE_SOUND_MODEL, (void *)pSoundModel);
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to load sound model, status = %d", __func__, status);
        goto exit;
    }

    QAL_VERBOSE(LOG_TAG, "%s: Load sound model success", __func__);
    return status;

exit:
    QAL_ERR(LOG_TAG, "%s: Failed to load sound model, status = %d", __func__, status);
    return status;
}

int32_t SoundTriggerEngineGsl::unload_sound_model(Stream *s)
{
    int32_t status = 0;

    if (!sm_data)
    {
        QAL_ERR(LOG_TAG, "%s: No sound model can be unloaded", __func__);
        status = -EINVAL;
        goto exit;
    }

    free(sm_data);
    sm_data_size = 0;

    QAL_VERBOSE(LOG_TAG, "%s: Unload sound model success", __func__);
    return status;

exit:
    QAL_ERR(LOG_TAG, "%s: Failed to unload sound model, status = %d", __func__, status);
    return status;
}

int32_t SoundTriggerEngineGsl::start_recognition(Stream *s)
{
    int32_t status = 0;

    status = session->setParameters(streamHandle, PARAM_ID_DETECTION_ENGINE_CONFIG_VOICE_WAKEUP, &pWakeUpConfig);
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to set wake up config, status = %d", __func__, status);
        goto exit;
    }

    status = session->setParameters(streamHandle, PARAM_ID_DETECTION_ENGINE_GENERIC_EVENT_CFG, &pEventConfig);
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to set event config, status = %d", __func__, status);
        goto exit;
    }

    status = session->setParameters(streamHandle, PARAM_ID_VOICE_WAKEUP_BUFFERING_CONFIG, &pBufConfig);
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to set wake-up buffer config, status = %d", __func__, status);
        goto exit;
    }

    status = session->setParameters(streamHandle, PARAM_ID_AUDIO_DAM_DOWNSTREAM_SETUP_DURATION, pSetupDuration);
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to set downstream setup duration, status = %d", __func__, status);
        goto exit;
    }

    status = start_sound_engine();
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to start sound engine, status = %d", __func__, status);
        goto exit;
    }

    QAL_VERBOSE(LOG_TAG, "%s: start recognition success", __func__);
    return status;

exit:
    QAL_ERR(LOG_TAG, "%s: Failed to start recognition, status = %d", __func__, status);
    return status;
}

int32_t SoundTriggerEngineGsl::stop_recognition(Stream *s)
{
    int32_t status = 0;

    status = session->setParameters(streamHandle, PARAM_ID_DETECTION_ENGINE_RESET, NULL);
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to reset detection engine, status = %d", __func__, status);
        goto exit;
    }

    status = stop_sound_engine();
    if (status)
    {
        QAL_ERR(LOG_TAG, "%s: Failed to stop sound engine, status = %d", __func__, status);
        goto exit;
    }

    QAL_VERBOSE(LOG_TAG, "%s: stop recognition success", __func__);
    return status;

exit:
    QAL_ERR(LOG_TAG, "%s: Failed to stop recognition, status = %d", __func__, status);
    return status;
}

int32_t SoundTriggerEngineGsl::update_config(Stream *s, struct qal_st_recognition_config *config)
{
    int32_t status = 0;
    size_t config_size;

    if (!config)
    {
        QAL_ERR(LOG_TAG, "%s: Invalid config", __func__);
        return -EINVAL;
    }

    status = generate_wakeup_config(config);
    if (status)
    {
        QAL_ERR(LOG_TAG, "Failed to generate wakeup config");
        return -EINVAL;
    }

    // event mode indicates info provided by DSP when event detected
    // TODO: set this from StreamSoundTrigger
    pEventConfig.event_mode = CONFIDENCE_LEVEL_INFO |
                              KEYWORD_INDICES_INFO |
                              TIME_STAMP_INFO;/* |
                              FTRT_INFO;*/

    pBufConfig.hist_buffer_duration_in_ms = HIST_BUFFER_DURATION_MS;
    pBufConfig.pre_roll_duration_in_ms = PRE_ROLL_DURATION_IN_MS;

    size_t num_output_ports = 1;
    uint32_t size = sizeof(struct audio_dam_downstream_setup_duration) +
                    num_output_ports * sizeof(struct audio_dam_downstream_setup_duration_t);
    pSetupDuration = (struct audio_dam_downstream_setup_duration *)calloc(1, size);
    pSetupDuration->num_output_ports = num_output_ports;

    for (int i = 0; i < pSetupDuration->num_output_ports; i++)
    {
        pSetupDuration->port_cfgs[i].output_port_id = 1;
        pSetupDuration->port_cfgs[i].dwnstrm_setup_duration_ms = DWNSTRM_SETUP_DURATION_MS;
    }

    QAL_VERBOSE(LOG_TAG, "%s: Update config success", __func__);
    return status;
}

void SoundTriggerEngineGsl::setDetected(bool detected)
{
    QAL_INFO(LOG_TAG, "setDetected %d", detected);
    std::lock_guard<std::mutex> lck(sndEngGsl_->mutex_);
    if (detected != eventDetected)
    {
        eventDetected = detected;
        QAL_INFO(LOG_TAG, "eventDetected set to %d", detected);
        cv_.notify_one();
    }
    else
        QAL_VERBOSE(LOG_TAG, "eventDetected unchanged");
}

int32_t SoundTriggerEngineGsl::generate_wakeup_config(struct qal_st_recognition_config *config)
{
    int32_t status = 0;
    unsigned int num_conf_levels = 0;
    unsigned int user_level, user_id;
    unsigned int i = 0, j = 0;
    unsigned char *conf_levels = NULL;
    unsigned char *user_id_tracker;
    struct qal_st_phrase_sound_model *phrase_sm = NULL;

    phrase_sm = (struct qal_st_phrase_sound_model *)pSoundModel;

    QAL_VERBOSE(LOG_TAG, "%s: start", __func__);

    if (!phrase_sm || !config){
        QAL_ERR(LOG_TAG, "%s: invalid input", __func__);
        status = -EINVAL;
        goto exit;
    }

    if ((config->num_phrases == 0) ||
        (config->num_phrases > phrase_sm->num_phrases)){
        status = -EINVAL;
        QAL_ERR(LOG_TAG, "%s: Invalid phrase data", __func__);
        goto exit;
    }

    for (i = 0; i < config->num_phrases; i++) {
        num_conf_levels++;
        for (j = 0; j < config->phrases[i].num_levels; j++)
            num_conf_levels++;
    }

    conf_levels = (unsigned char*)calloc(1, num_conf_levels);

    user_id_tracker = (unsigned char *) calloc(1, num_conf_levels);
    if (!user_id_tracker) {
        QAL_ERR(LOG_TAG,"%s: failed to allocate user_id_tracker", __func__);
        return -ENOMEM;
    }

    /* for debug */
    for (i = 0; i < config->num_phrases; i++) {
        QAL_VERBOSE(LOG_TAG, "%s: [%d] kw level %d", __func__, i,
        config->phrases[i].confidence_level);
        for (j = 0; j < config->phrases[i].num_levels; j++) {
            QAL_VERBOSE(LOG_TAG, "%s: [%d] user_id %d level %d ", __func__, i,
                        config->phrases[i].levels[j].user_id,
                        config->phrases[i].levels[j].level);
        }
    }

/* Example: Say the recognition structure has 3 keywords with users
        [0] k1 |uid|
                [0] u1 - 1st trainer
                [1] u2 - 4th trainer
                [3] u3 - 3rd trainer
        [1] k2
                [2] u2 - 2nd trainer
                [4] u3 - 5th trainer
        [2] k3
                [5] u4 - 6th trainer

      Output confidence level array will be
      [k1, k2, k3, u1k1, u2k1, u2k2, u3k1, u3k2, u4k3]
*/
    for (i = 0; i < config->num_phrases; i++) {
        conf_levels[i] = config->phrases[i].confidence_level;
        for (j = 0; j < config->phrases[i].num_levels; j++) {
            user_level = config->phrases[i].levels[j].level;
            user_id = config->phrases[i].levels[j].user_id;
            if ((user_id < config->num_phrases) ||
                (user_id >= num_conf_levels)) {
                QAL_ERR(LOG_TAG, "%s: ERROR. Invalid params user id %d>%d",
                        __func__, user_id);
                status = -EINVAL;
                goto exit;
            }
            else {
                if (user_id_tracker[user_id] == 1) {
                    QAL_ERR(LOG_TAG, "%s: ERROR. Duplicate user id %d",
                            __func__, user_id);
                    status = -EINVAL;
                    goto exit;
                }
                conf_levels[user_id] = (user_level < 100) ? user_level : 100;
                user_id_tracker[user_id] = 1;
                QAL_VERBOSE(LOG_TAG, "%s: user_conf_levels[%d] = %d", __func__,
                            user_id, conf_levels[user_id]);
            }
        }
    }

    pWakeUpConfig.mode = config->phrases[0].recognition_modes;
    pWakeUpConfig.custom_payload_size = config->data_size;
    pWakeUpConfig.num_active_models = num_conf_levels;
    pWakeUpConfig.reserved = 0;
    for (int i = 0; i < pWakeUpConfig.num_active_models; i++)
    {
        pWakeUpConfig.confidence_levels[i] = conf_levels[i];
        pWakeUpConfig.keyword_user_enables[i] = 1;
    }
exit:
    QAL_VERBOSE(LOG_TAG, "%s: end, status - %d", __func__, status);
    if (conf_levels)
        free(conf_levels);
    if (user_id_tracker)
        free(user_id_tracker);
    return status;
}

