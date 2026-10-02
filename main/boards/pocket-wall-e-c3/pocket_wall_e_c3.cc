#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "lvgl_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "tabymoji_assets.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "PocketWallEC3"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

class PocketOledDisplay : public LvglDisplay {
private:
    enum class DisplayState {
        Starting,
        Standby,
        Connecting,
        Listening,
        Speaking,
        Configuring,
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* anim_image_ = nullptr;

    DisplayState current_state_ = DisplayState::Starting;
    const TabymojiAnimation* current_anim_ = nullptr;
    uint16_t current_frame_index_ = 0;
    lv_timer_t* anim_timer_ = nullptr;
    lv_image_dsc_t img_dsc_{};

    virtual bool Lock(int timeout_ms = 0) override {
        return lvgl_port_lock(timeout_ms);
    }
    virtual void Unlock() override {
        lvgl_port_unlock();
    }

    void PlayAnimation(const TabymojiAnimation* anim) {
        if (anim == nullptr) {
            return;
        }
        if (anim == current_anim_ && current_anim_->loop) {
            return;
        }

        current_anim_ = anim;
        current_frame_index_ = 0;
        if (anim_timer_ != nullptr) {
            lv_timer_set_period(anim_timer_, current_anim_->frame_delay_ms);
            lv_timer_reset(anim_timer_);
        }
        img_dsc_.data = current_anim_->frames[0];
        if (anim_image_ != nullptr) {
            lv_image_set_src(anim_image_, &img_dsc_);
            lv_obj_invalidate(anim_image_);
        }
    }

    void OnAnimTimer() {
        if (!current_anim_ || current_anim_->frame_count == 0) {
            return;
        }

        current_frame_index_++;
        if (current_frame_index_ >= current_anim_->frame_count) {
            if (current_anim_->loop) {
                current_frame_index_ = 0;
            } else {
                // If we just finished listening_in (waking up into listening), chain directly into listening_loop!
                if (strcmp(current_anim_->id, "listening_in") == 0) {
                    PlayAnimation(Tabymoji_GetById("listening_loop"));
                    return;
                }

                // For other single-shot animations, return to the loop for the current state:
                const TabymojiAnimation* next_anim = nullptr;
                switch (current_state_) {
                    case DisplayState::Standby:
                        next_anim = Tabymoji_GetById("sleeping_loop");
                        break;
                    case DisplayState::Listening:
                        next_anim = Tabymoji_GetById("listening_loop");
                        break;
                    case DisplayState::Speaking:
                        next_anim = Tabymoji_GetById("talking_default_loop");
                        break;
                    case DisplayState::Connecting:
                        next_anim = Tabymoji_GetById("circle");
                        break;
                    case DisplayState::Configuring:
                        next_anim = Tabymoji_GetById("searching_loop");
                        break;
                    case DisplayState::Starting:
                    default:
                        next_anim = Tabymoji_GetById("sleeping_loop");
                        break;
                }
                PlayAnimation(next_anim);
                return;
            }
        }

        img_dsc_.data = current_anim_->frames[current_frame_index_];
        if (anim_image_) {
            lv_image_set_src(anim_image_, &img_dsc_);
            lv_obj_invalidate(anim_image_);
        }
    }

public:
    PocketOledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, bool mirror_x, bool mirror_y)
        : panel_io_(panel_io), panel_(panel) {
        width_ = width;
        height_ = height;

        auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
        auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);

        auto dark_theme = new LvglTheme("dark");
        dark_theme->set_text_font(text_font);
        dark_theme->set_icon_font(icon_font);

        auto& theme_manager = LvglThemeManager::GetInstance();
        theme_manager.RegisterTheme("dark", dark_theme);
        current_theme_ = dark_theme;

        ESP_LOGI(TAG, "Initialize LVGL for Pocket Wall-E OLED");
        lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        port_cfg.task_priority = 1;
        port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
        port_cfg.task_affinity = 1;
#endif
        lvgl_port_init(&port_cfg);

        const lvgl_port_display_cfg_t display_cfg = {
            .io_handle = panel_io_,
            .panel_handle = panel_,
            .control_handle = nullptr,
            .buffer_size = static_cast<uint32_t>(width_ * height_),
            .double_buffer = false,
            .trans_size = 0,
            .hres = static_cast<uint32_t>(width_),
            .vres = static_cast<uint32_t>(height_),
            .monochrome = true,
            .rotation =
                {
                    .swap_xy = false,
                    .mirror_x = mirror_x,
                    .mirror_y = mirror_y,
                },
            .flags =
                {
                    .buff_dma = 1,
                    .buff_spiram = 0,
                    .sw_rotate = 0,
                    .full_refresh = 0,
                    .direct_mode = 0,
                },
        };

        display_ = lvgl_port_add_disp(&display_cfg);
        if (display_ == nullptr) {
            ESP_LOGE(TAG, "Failed to add display");
            return;
        }

        // Initialize Tabymoji image descriptor
        img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        img_dsc_.header.cf = LV_COLOR_FORMAT_A1;
        img_dsc_.header.flags = 0;
        img_dsc_.header.w = TABYMOJI_WIDTH;
        img_dsc_.header.h = TABYMOJI_HEIGHT;
        img_dsc_.header.stride = TABYMOJI_STRIDE;
        img_dsc_.data_size = TABYMOJI_FRAME_BYTES;

        current_state_ = DisplayState::Starting;
        current_anim_ = Tabymoji_GetById("startup");
        if (current_anim_ && current_anim_->frame_count > 0) {
            img_dsc_.data = current_anim_->frames[0];
        }
    }

    ~PocketOledDisplay() {
        if (anim_timer_ != nullptr) {
            lv_timer_delete(anim_timer_);
            anim_timer_ = nullptr;
        }
        if (anim_image_ != nullptr) {
            lv_obj_del(anim_image_);
            anim_image_ = nullptr;
        }
        if (panel_ != nullptr) {
            esp_lcd_panel_del(panel_);
            panel_ = nullptr;
        }
        if (panel_io_ != nullptr) {
            esp_lcd_panel_io_del(panel_io_);
            panel_io_ = nullptr;
        }
        lvgl_port_deinit();
    }

    virtual void SetupUI() override {
        if (setup_ui_called_) {
            return;
        }
        Display::SetupUI();

        DisplayLockGuard lock(this);

        auto screen = lv_screen_active();
        lv_obj_set_style_pad_all(screen, 0, 0);

        anim_image_ = lv_image_create(screen);
        lv_obj_set_size(anim_image_, TABYMOJI_WIDTH, TABYMOJI_HEIGHT);
        lv_obj_center(anim_image_);
        if (img_dsc_.data != nullptr) {
            lv_image_set_src(anim_image_, &img_dsc_);
        }

        /* Animation Timer */
        anim_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* display = static_cast<PocketOledDisplay*>(lv_timer_get_user_data(timer));
                if (display != nullptr) {
                    display->OnAnimTimer();
                }
            },
            current_anim_ ? current_anim_->frame_delay_ms : 83, this);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        // Big face emoji only - no chat text overlay
    }

    virtual void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (!emotion || emotion[0] == '\0') {
            return;
        }

        // When neutral / robot_2 / default is requested, restore the default animation for current state:
        if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "robot_2") == 0 || strcmp(emotion, "default") == 0) {
            switch (current_state_) {
                case DisplayState::Standby:
                    PlayAnimation(Tabymoji_GetById("sleeping_loop"));
                    break;
                case DisplayState::Listening:
                    PlayAnimation(Tabymoji_GetById("listening_loop"));
                    break;
                case DisplayState::Speaking:
                    PlayAnimation(Tabymoji_GetById("talking_default_loop"));
                    break;
                case DisplayState::Connecting:
                    PlayAnimation(Tabymoji_GetById("circle"));
                    break;
                case DisplayState::Configuring:
                    PlayAnimation(Tabymoji_GetById("searching_loop"));
                    break;
                case DisplayState::Starting:
                    PlayAnimation(Tabymoji_GetById("startup"));
                    break;
            }
            return;
        }

        // Specific emotion from server/LLM:
        const TabymojiAnimation* anim = Tabymoji_GetByEmotion(emotion);
        if (anim != nullptr) {
            PlayAnimation(anim);
        }
    }

    virtual void SetStatus(const char* status) override {
        DisplayLockGuard lock(this);
        if (!status || status[0] == '\0') {
            return;
        }

        if (strstr(status, "待命") || strstr(status, "Standby") || strstr(status, "standby") || strstr(status, "Idle") || strstr(status, "idle")) {
            current_state_ = DisplayState::Standby;
            PlayAnimation(Tabymoji_GetById("sleeping_loop"));
        } else if (strstr(status, "聆听") || strstr(status, "Listen") || strstr(status, "listen")) {
            DisplayState prev = current_state_;
            current_state_ = DisplayState::Listening;
            // If waking up from Standby/Sleep, play waking up transition (listening_in)
            if (prev == DisplayState::Standby || prev == DisplayState::Starting) {
                PlayAnimation(Tabymoji_GetById("listening_in"));
            } else {
                PlayAnimation(Tabymoji_GetById("listening_loop"));
            }
        } else if (strstr(status, "说话") || strstr(status, "Speak") || strstr(status, "speak") || strstr(status, "Talking") || strstr(status, "talking")) {
            current_state_ = DisplayState::Speaking;
            PlayAnimation(Tabymoji_GetById("talking_default_loop"));
        } else if (strstr(status, "连接") || strstr(status, "Connect") || strstr(status, "connect") || strstr(status, "Wait") || strstr(status, "wait") || strstr(status, "Logging") || strstr(status, "Checking")) {
            current_state_ = DisplayState::Connecting;
            PlayAnimation(Tabymoji_GetById("circle"));
        } else if (strstr(status, "Starting") || strstr(status, "Initializing") || strstr(status, "Version") || strstr(status, "Ver ")) {
            current_state_ = DisplayState::Starting;
            PlayAnimation(Tabymoji_GetById("startup"));
        } else if (strstr(status, "Config") || strstr(status, "Scan") || strstr(status, "Wi-Fi") || strstr(status, "wifi") || strstr(status, "4G") || strstr(status, "Modem")) {
            current_state_ = DisplayState::Configuring;
            PlayAnimation(Tabymoji_GetById("searching_loop"));
        } else if (strstr(status, "Upgrade") || strstr(status, "upgrading") || strstr(status, "OTA") || strstr(status, "Loading")) {
            current_state_ = DisplayState::Configuring;
            PlayAnimation(Tabymoji_GetById("working_loop"));
        }
    }

    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        // Big face emoji only - no text notification overlay
    }

    virtual void ShowNotification(const std::string& notification, int duration_ms = 3000) override {
        // Big face emoji only - no text notification overlay
    }

    virtual void UpdateStatusBar(bool update_all = false) override {
        // Big face emoji only - no status bar / clock / battery / wifi text
    }

    virtual void SetTheme(Theme* theme) override {
        DisplayLockGuard lock(this);
        auto lvgl_theme = static_cast<LvglTheme*>(theme);
        auto text_font = lvgl_theme->text_font()->font();
        auto screen = lv_screen_active();
        lv_obj_set_style_text_font(screen, text_font, 0);
    }

    virtual bool IsMonochrome() const override { return true; }

    void SetPowerSaveMode(bool on) override {
        if (panel_) {
            Settings settings("wifi", false);
            if (settings.GetBool("power_save_display_off", false)) {
                esp_lcd_panel_disp_on_off(panel_, !on);
            }
        }
        LvglDisplay::SetPowerSaveMode(on);
    }
};

class PocketWallEC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = DISPLAY_I2C_ADDR,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new PocketOledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButton() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            EnterWifiConfigMode();
        });
    }

public:
    PocketWallEC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializeButton();
        ESP_LOGI(TAG, "Pocket Wall-E C3 initialized with Tabymoji animation engine");
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(PocketWallEC3Board);
