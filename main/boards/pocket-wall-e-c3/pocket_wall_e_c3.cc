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
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_obj_t* container_ = nullptr;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* anim_image_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

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

    void OnAnimTimer() {
        if (!current_anim_ || current_anim_->frame_count == 0) {
            return;
        }

        current_frame_index_++;
        if (current_frame_index_ >= current_anim_->frame_count) {
            if (current_anim_->loop) {
                current_frame_index_ = 0;
            } else {
                // Return to idle loop after single-shot animation finishes
                current_anim_ = Tabymoji_GetById("idle_01_loop");
                current_frame_index_ = 0;
                if (anim_timer_) {
                    lv_timer_set_period(anim_timer_, current_anim_->frame_delay_ms);
                }
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

        current_anim_ = Tabymoji_GetById("idle_01_loop");
        if (current_anim_ && current_anim_->frame_count > 0) {
            img_dsc_.data = current_anim_->frames[0];
        }
    }

    ~PocketOledDisplay() {
        if (anim_timer_ != nullptr) {
            lv_timer_delete(anim_timer_);
            anim_timer_ = nullptr;
        }
        if (content_ != nullptr) {
            lv_obj_del(content_);
            content_ = nullptr;
            content_left_ = nullptr;
            content_right_ = nullptr;
            anim_image_ = nullptr;
            chat_message_label_ = nullptr;
        }
        if (status_bar_ != nullptr) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            lv_obj_del(status_bar_);
            status_bar_ = nullptr;
        }
        if (top_bar_ != nullptr) {
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
            lv_obj_del(top_bar_);
            top_bar_ = nullptr;
        }
        if (container_ != nullptr) {
            lv_obj_del(container_);
            container_ = nullptr;
        }
        if (low_battery_popup_ != nullptr) {
            low_battery_label_ = nullptr;
            lv_obj_del(low_battery_popup_);
            low_battery_popup_ = nullptr;
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

        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        auto icon_font = lvgl_theme->icon_font()->font();

        auto screen = lv_screen_active();
        lv_obj_set_style_text_font(screen, text_font, 0);
        lv_obj_set_style_text_color(screen, lv_color_black(), 0);

        /* Container */
        container_ = lv_obj_create(screen);
        lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(container_, 0, 0);
        lv_obj_set_style_border_width(container_, 0, 0);
        lv_obj_set_style_pad_row(container_, 0, 0);

        /* Layer 1: Top bar - status icons */
        top_bar_ = lv_obj_create(container_);
        lv_obj_set_size(top_bar_, LV_HOR_RES, 14);
        lv_obj_set_style_radius(top_bar_, 0, 0);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(top_bar_, 0, 0);
        lv_obj_set_style_pad_all(top_bar_, 0, 0);
        lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

        network_label_ = lv_label_create(top_bar_);
        lv_label_set_text(network_label_, "");
        lv_obj_set_style_text_font(network_label_, icon_font, 0);

        lv_obj_t* right_icons = lv_obj_create(top_bar_);
        lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(right_icons, 0, 0);
        lv_obj_set_style_pad_all(right_icons, 0, 0);
        lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        mute_label_ = lv_label_create(right_icons);
        lv_label_set_text(mute_label_, "");
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);

        battery_label_ = lv_label_create(right_icons);
        lv_label_set_text(battery_label_, "");
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);

        /* Layer 2: Status bar - center text labels */
        status_bar_ = lv_obj_create(screen);
        lv_obj_set_size(status_bar_, LV_HOR_RES, 14);
        lv_obj_set_style_radius(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(status_bar_, 0, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
        lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

        notification_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(notification_label_, LV_HOR_RES);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(notification_label_, "");
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        status_label_ = lv_label_create(status_bar_);
        lv_obj_set_width(status_label_, LV_HOR_RES);
        lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
        lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

        /* Content Area */
        content_ = lv_obj_create(container_);
        lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_radius(content_, 0, 0);
        lv_obj_set_style_pad_all(content_, 0, 0);
        lv_obj_set_style_border_width(content_, 0, 0);
        lv_obj_set_size(content_, LV_HOR_RES, 50);
        lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);

        /* Left: Animated Tabymoji Image */
        content_left_ = lv_obj_create(content_);
        lv_obj_set_size(content_left_, TABYMOJI_WIDTH, TABYMOJI_HEIGHT);
        lv_obj_set_style_pad_all(content_left_, 0, 0);
        lv_obj_set_style_border_width(content_left_, 0, 0);
        lv_obj_set_scrollbar_mode(content_left_, LV_SCROLLBAR_MODE_OFF);

        anim_image_ = lv_image_create(content_left_);
        lv_obj_set_size(anim_image_, TABYMOJI_WIDTH, TABYMOJI_HEIGHT);
        lv_obj_center(anim_image_);
        if (img_dsc_.data != nullptr) {
            lv_image_set_src(anim_image_, &img_dsc_);
        }

        /* Right: Subtitle Text */
        content_right_ = lv_obj_create(content_);
        lv_obj_set_size(content_right_, LV_SIZE_CONTENT, TABYMOJI_HEIGHT);
        lv_obj_set_style_pad_all(content_right_, 0, 0);
        lv_obj_set_style_border_width(content_right_, 0, 0);
        lv_obj_set_flex_grow(content_right_, 1);
        lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

        chat_message_label_ = lv_label_create(content_right_);
        lv_label_set_text(chat_message_label_, "");
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(chat_message_label_, width_ - TABYMOJI_WIDTH - 4);
        lv_obj_set_style_pad_top(chat_message_label_, 12, 0);

        static lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_delay(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
        lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000),
                                       LV_PART_MAIN);

        /* Low Battery Popup */
        low_battery_popup_ = lv_obj_create(screen);
        lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
        lv_obj_set_style_radius(low_battery_popup_, 10, 0);
        low_battery_label_ = lv_label_create(low_battery_popup_);
        lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
        lv_obj_center(low_battery_label_);
        lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

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
        DisplayLockGuard lock(this);
        if (chat_message_label_ == nullptr) {
            return;
        }

        std::string content_str = content ? content : "";
        std::replace(content_str.begin(), content_str.end(), '\n', ' ');

        lv_anim_delete(chat_message_label_, nullptr);
        if (content_right_ == nullptr) {
            lv_label_set_text(chat_message_label_, content_str.c_str());
        } else {
            if (content == nullptr || content[0] == '\0') {
                lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
                if (content_) {
                    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                          LV_FLEX_ALIGN_CENTER);
                }
            } else {
                lv_label_set_text(chat_message_label_, content_str.c_str());
                lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
                if (content_) {
                    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                          LV_FLEX_ALIGN_CENTER);
                }
            }
        }
    }

    virtual void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        const TabymojiAnimation* anim = Tabymoji_GetByEmotion(emotion);
        if (anim != nullptr && anim != current_anim_) {
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
    }

    virtual void SetStatus(const char* status) override {
        LvglDisplay::SetStatus(status);
        DisplayLockGuard lock(this);
        const TabymojiAnimation* anim = Tabymoji_GetByStatus(status);
        if (anim != nullptr && anim != current_anim_) {
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
