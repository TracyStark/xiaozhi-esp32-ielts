#include "wake_word_detect.h"
#include "application.h"

#include <esp_log.h>
#include <model_path.h>
#include <arpa/inet.h>
#include <sstream>

#define DETECTION_RUNNING_EVENT 1

static const char* TAG = "WakeWordDetect";

WakeWordDetect::WakeWordDetect()
    : afe_detection_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

WakeWordDetect::~WakeWordDetect() {
    if (afe_detection_data_ != nullptr) {
        esp_afe_sr_v1.destroy(afe_detection_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    vEventGroupDelete(event_group_);
}

void WakeWordDetect::Initialize(int channels, bool reference) {
    channels_ = channels;
    reference_ = reference;
    int ref_num = reference_ ? 1 : 0;

    srmodel_list_t *models = esp_srmodel_init("model");
    for (int i = 0; i < models->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models->model_name[i]);
        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models->model_name[i];
            auto words = esp_srmodel_get_wake_words(models, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    afe_config_t afe_config = {
        .aec_init = reference_,
        .se_init = true,
        .vad_init = true,
        .wakenet_init = true,
        .voice_communication_init = false,
        .voice_communication_agc_init = false,
        .voice_communication_agc_gain = 15,
        .vad_mode = VAD_MODE_4,
        .wakenet_model_name = wakenet_model_,
        .wakenet_model_name_2 = NULL,
        .wakenet_mode = DET_MODE_95,
        .afe_mode = SR_MODE_HIGH_PERF,
        .afe_perferred_core = 1,
        .afe_perferred_priority = 1,
        .afe_ringbuf_size = 100,
        .memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM,
        .afe_linear_gain = 1.2,
        .agc_mode = AFE_MN_PEAK_AGC_MODE_2,
        .pcm_config = {
            .total_ch_num = channels_,
            .mic_num = channels_ - ref_num,
            .ref_num = ref_num,
            .sample_rate = 16000
        },
        .debug_init = false,
        .debug_hook = {{ AFE_DEBUG_HOOK_MASE_TASK_IN, NULL }, { AFE_DEBUG_HOOK_FETCH_TASK_IN, NULL }},
        .afe_ns_mode = NS_MODE_SSP,
        .afe_ns_model_name = NULL,
        .fixed_first_channel = true,
    };

    afe_detection_data_ = esp_afe_sr_v1.create_from_config(&afe_config);

    xTaskCreate([](void* arg) {
        auto this_ = (WakeWordDetect*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 4096 * 2, this, 2, nullptr);
}

void WakeWordDetect::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void WakeWordDetect::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void WakeWordDetect::StartDetection() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void WakeWordDetect::StopDetection() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
}

bool WakeWordDetect::IsDetectionRunning() {
    return xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT;
}

void WakeWordDetect::Feed(const std::vector<int16_t>& data) {
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());

    auto feed_size = esp_afe_sr_v1.get_feed_chunksize(afe_detection_data_) * channels_;
    while (input_buffer_.size() >= feed_size) {
        esp_afe_sr_v1.feed(afe_detection_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + feed_size);
    }
}

void WakeWordDetect::AudioDetectionTask() {
    auto fetch_size = esp_afe_sr_v1.get_fetch_chunksize(afe_detection_data_);
    auto feed_size = esp_afe_sr_v1.get_feed_chunksize(afe_detection_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = esp_afe_sr_v1.fetch(afe_detection_data_);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;;
        }

        // Store the wake word data for voice recognition, like who is speaking
        StoreWakeWordData((uint16_t*)res->data, res->data_size / sizeof(uint16_t));

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == AFE_VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == AFE_VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            StopDetection();
            last_detected_wake_word_ = wake_words_[res->wake_word_index - 1];

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        }
    }
}

void WakeWordDetect::StoreWakeWordData(uint16_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(wake_word_mutex_);
    
    // 检查是否超过PCM缓冲区限制
    if (wake_word_pcm_.size() >= MAX_PCM_BUFFER_SIZE) {
        // 移除最旧的PCM数据
        if (!wake_word_pcm_.empty()) {
            total_pcm_size_ -= wake_word_pcm_.front().size() * sizeof(int16_t);
            wake_word_pcm_.pop_front();
        }
    }
    
    // 添加新的PCM数据
    std::vector<int16_t> pcm_data(data, data + size);
    total_pcm_size_ += pcm_data.size() * sizeof(int16_t);
    wake_word_pcm_.push_back(std::move(pcm_data));
}

void WakeWordDetect::EncodeWakeWordData() {
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(4096 * 8, MALLOC_CAP_SPIRAM);
    }
    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (WakeWordDetect*)arg;
        {
            auto start_time = esp_timer_get_time();
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
            encoder->SetComplexity(0); // 0 is the fastest

            for (auto& pcm: this_->wake_word_pcm_) {
                encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(std::move(opus));
                    this_->wake_word_cv_.notify_all();
                });
            }
            this_->wake_word_pcm_.clear();

            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %zu packets in %lld ms",
                this_->wake_word_opus_.size(), (end_time - start_time) / 1000);

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_detect_packets", 4096 * 8, this, 2, wake_word_encode_task_stack_, &wake_word_encode_task_buffer_);
}

bool WakeWordDetect::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    
    // 检查是否超过Opus缓冲区限制
    if (wake_word_opus_.size() > MAX_OPUS_BUFFER_SIZE) {
        // 移除最旧的Opus数据
        if (!wake_word_opus_.empty()) {
            total_opus_size_ -= wake_word_opus_.front().size();
            wake_word_opus_.pop_front();
        }
    }
    
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    if (!opus.empty()) {
        total_opus_size_ -= opus.size();
    }
    return !opus.empty();
}
