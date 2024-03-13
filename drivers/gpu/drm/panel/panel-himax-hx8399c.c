#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/delay.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

/* panel display mode */
#define PANEL_HRES  1080        /* panel horizontal resolution in pixels*/
#define PANEL_VRES  1920        /* panel vertical resolution in pixels*/
#define PANEL_HFP   16          /* horizontal front porch in pixels*/
#define PANEL_HBP   25          /* horizontal back porch in pixels*/
#define PANEL_HS    25          /* horizontal sync length in pixels */
#define PANEL_VFP   9           /* vertical front porch in lines*/
#define PANEL_VBP   5           /* vertical back porch in lines*/
#define PANEL_VS    2           /* vertical sync length in lines*/
#define PANEL_WIDTH_MM  68      /* panel width in mm */
#define PANEL_HEIGH_MM  121     /* panel heigh in mm */
#define PANEL_REFRESH_RATE  60  /* panel refresh rate. no more than 60Hz */
#define PANEL_PIXCLOCK ((PANEL_HRES + PANEL_HFP + PANEL_HS + PANEL_HBP) * \
                        (PANEL_VRES + PANEL_VFP + PANEL_VS + PANEL_VBP) * \
                        PANEL_REFRESH_RATE)

/* user define dsi command set refer to DS */
#define UD_CMD_SETPOWER         0xB1 /* Set power related register */
#define UD_CMD_SETDISP          0xB2 /* Set display related register */
#define UD_CMD_SETCYC           0xB4 /* Set display waveform cycles */
#define UD_CMD_SETVCOM          0xB6 /* Set VCOM voltage */
#define UD_CMD_SETTE            0xB7 /* Set internal TE function */
#define UD_CMD_SETEXTC          0xB9 /* Set extension command */
#define UD_CMD_SETMIPI          0xBA /* Set MIPI control */
#define UD_CMD_SETOTP           0xBB /* Set OTP */
#define UD_CMD_SET_BANK         0xBD /* Set register bank */
#define UD_CMD_SETDGCLUT_P      0xC1 /* Set DGC LUT_P */
#define UD_CMD_SETDISMO         0xC2 /* Set display mode */
#define UD_CMD_SETID            0xC3 /* Set ID */
#define UD_CMD_SETDDB           0xC4 /* Set DDB */
#define UD_CMD_SETCABC          0xC9 /* Set CABC control */
#define UD_CMD_SETCLOCK         0xCB
#define UD_CMD_SETPANEL         0xCC /* Set panel related register */
#define UD_CMD_SETOFFSET        0xD2
#define UD_CMD_SETGIP0          0xD3 /* Set GIP Option0 */
#define UD_CMD_SETGIP1          0xD5 /* Set GIP Option1 */
#define UD_CMD_SETGIP2          0xD6 /* Set Gip Option2 */
#define UD_CMD_SETGIP3          0xD8 /* Set Gip Option3 */
#define UD_CMD_SETGPO           0xD9
#define UD_CMD_SETSCALING       0xDD
#define UD_CMD_SETDGCLUT_N      0xDE /* Set DGC LUT_N */
#define UD_CMD_SETIDLE          0xDF
#define UD_CMD_SETGAMMA         0xE0 /* Set gamma curve related setting */
#define UD_CMD_SETCHEMODE_DYN   0xE4
#define UD_CMD_SET_I2C_SA       0xE8 /* Set I2C slave address */
#define UD_CMD_SET_SP_CMD       0xE9
#define UD_CMD_SETCNCD_GETCNCD  0xFD
#define UD_CMD_SETREADINDEX     0xFE /* Set SPI Read Index */
#define UD_CMD_GETSPIREAD       0xFF /* SPI Read Command Data */

/* supported bus formats of the hx8399c */
static const u32 hx8399c_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
	MEDIA_BUS_FMT_RGB666_1X18,
	MEDIA_BUS_FMT_RGB565_1X16,
};

/* according to the RM of hx8399c
 * data enable is active high
 * data is smapled on the negetive edge
 */
static const u32 hx8399c_bus_flags = DRM_BUS_FLAG_DE_HIGH |
				 DRM_BUS_FLAG_PIXDATA_SAMPLE_NEGEDGE;

struct himax_panel {
    struct drm_panel panel;
    struct mipi_dsi_device *dsi;

    struct gpio_desc *reset;
    struct gpio_desc *backlight;
    //struct backlight_device *backlight;

    const struct himax_platform_data *pdata;

    bool prepared;
    bool enabled;
};

struct himax_platform_data {
    int (*enable)(struct himax_panel *panel);
};

/* drm display mode used for crtc */
static const struct drm_display_mode default_mode = {
    .clock = PANEL_PIXCLOCK/1000, //pixel clock in kHz
    .hdisplay = PANEL_HRES,
    .hsync_start = PANEL_HRES + PANEL_HFP,
    .hsync_end = PANEL_HRES + PANEL_HFP + PANEL_HS,
    .htotal = PANEL_HRES + PANEL_HFP + PANEL_HS + PANEL_HBP,
    .vdisplay = PANEL_VRES,
    .vsync_start = PANEL_VRES + PANEL_VFP,
    .vsync_end = PANEL_VRES + PANEL_VFP + PANEL_VS,
    .vtotal = PANEL_VRES + PANEL_VFP + PANEL_VS + PANEL_VBP,
    .width_mm = PANEL_WIDTH_MM,
    .height_mm = PANEL_HEIGH_MM,
    .flags = DRM_MODE_FLAG_NHSYNC |
         DRM_MODE_FLAG_NVSYNC,
};

/*!
 * @brief find the container address of drm_panel structure
 *
 * @param panel pointer to the dram panel structure
 *
 * @return container address
 */
static inline struct himax_panel *to_himax_panel(struct drm_panel *panel)
{
    return container_of(panel, struct himax_panel, panel);
}

static int himax_panel_prepare(struct drm_panel *panel)
{
    struct himax_panel *ctx = to_himax_panel(panel);

    if (ctx->prepared)
        return 0;

    /* At least 1000+10us between power-on and reset-out as RM specifies */
    usleep_range(1010, 2010);

    if (ctx->reset) {
        gpiod_set_value_cansleep(ctx->reset, 0);
        /* 50ms reset active time is needed */
        msleep(50);
    }

    ctx->prepared = true;

    return 0;
}

static int himax_panel_unprepare(struct drm_panel *panel)
{
    struct himax_panel *ctx = to_himax_panel(panel);

    if (!ctx->prepared) {
        gpiod_set_value_cansleep(ctx->reset, 1);
        usleep_range(15000,17000);
        gpiod_set_value_cansleep(ctx->reset, 0);
    }

    ctx->prepared = false;

    return 0;
}

/*!
 * @brief dsi command set write in sequence
 *
 * @param ctx pointer to the panel context structure
 * @param seq data code to be written in sequence
 *
 */
#define dcs_write_seq(ctx, seq...)      \
({                                      \
    static const u8 d[] = { seq };      \
    int ret;                            \
    ret = mipi_dsi_dcs_write_buffer(ctx->dsi, d, sizeof(d));    \
})

/*!
 * @brief enable hx8399c chip
 *
 * @param ctx pointer to the panel context structure
 *
 * @return 0 for success or error code
 */
static int hx8399c_enable(struct himax_panel *ctx)
{
    struct mipi_dsi_device *dsi = ctx->dsi;
    struct device* dev = &dsi->dev;
    int ret;

    /* check whether the panel has been enabled */
    if (ctx->enabled)
        return 0;

    /* set extc */
    ret = dcs_write_seq(ctx, UD_CMD_SETEXTC,
                        0xFF, 0x83, 0x99);
    if (ret < 0) {
        dev_err(dev, "Failed to set extc (%d)\n", ret);
        goto fail;
    }

    /* set d2 */
    ret = dcs_write_seq(ctx, UD_CMD_SETOFFSET,
                        0x77);
    if (ret < 0) {
        dev_err(dev, "Failed to set d2 (%d)\n", ret);
        goto fail;
    }

    /* set power */
    ret = dcs_write_seq(ctx, UD_CMD_SETPOWER,
                        0x02, 0x04, 0x74, 0x94, 0x01, 0x32, 0x33, 0x11,
                        0x11, 0xAB, 0x4D, 0x56, 0x73, 0x02, 0x02);
    if (ret < 0) {
        dev_err(dev, "Failed to set power (%d)\n", ret);
        goto fail;
    }

    /* set display */
    ret = dcs_write_seq(ctx, UD_CMD_SETDISP,
                        0x00, 0x80, 0x80, 0xAE, 0x05, 0x07, 0x5A, 0x11,
                        0x00, 0x00, 0x10, 0x1E, 0x70, 0x03, 0xD4);
    if (ret < 0) {
        dev_err(dev, "Failed to set display (%d)\n", ret);
        goto fail;
    }

    /* set cycles */
    ret = dcs_write_seq(ctx, UD_CMD_SETCYC,
                        0x00, 0xFF, 0x02, 0xC0, 0x02, 0xC0, 0x00, 0x00,
                        0x08, 0x00, 0x04, 0x06, 0x00, 0x32, 0x04, 0x0A,
                        0x08, 0x21, 0x03, 0x01, 0x00, 0x0F, 0xB8, 0x8B,
                        0x02, 0xC0, 0x02, 0xC0, 0x00, 0x00, 0x08, 0x00,
                        0x04, 0x06, 0x00, 0x32, 0x04, 0x0A, 0x08, 0x01,
                        0x00, 0x0F, 0xB8, 0x01);
    if (ret < 0) {
        dev_err(dev, "Failed to set cycles (%d)\n", ret);
        goto fail;
    }

    /* set gip 0 */
    ret = dcs_write_seq(ctx, UD_CMD_SETGIP0,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
                        0x00, 0x10, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                        0x00, 0x05, 0x05, 0x07, 0x00, 0x00, 0x00, 0x05,
                        0x40);
    if (ret < 0) {
        dev_err(dev, "Failed to set gip0 (%d)\n", ret);
        goto fail;
    }

    usleep_range(10000, 11000);

    /* set gip fmapping*/
    ret = dcs_write_seq(ctx, UD_CMD_SETGIP1,
                        0x18, 0x18, 0x19, 0x19, 0x18, 0x18, 0x21, 0x20,
                        0x01, 0x00, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
                        0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x2F, 0x2F,
                        0x30, 0x30, 0x31, 0x31, 0x18, 0x18, 0x18, 0x18);
    if (ret < 0) {
        dev_err(dev, "Failed to set gip fmapping (%d)\n", ret);
        goto fail;
    }

    usleep_range(10000, 11000);

    /* set gip bmapping*/
    ret = dcs_write_seq(ctx, UD_CMD_SETGIP2,
                        0x18, 0x18, 0x19, 0x19, 0x40, 0x40, 0x20, 0x21,
                        0x06, 0x07, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                        0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x2F, 0x2F,
                        0x30, 0x30, 0x31, 0x31, 0x40, 0x40, 0x40, 0x40);
    if (ret < 0) {
        dev_err(dev, "Failed to set gip bmapping (%d)\n", ret);
        goto fail;
    }

    /* set d8 */
    ret = dcs_write_seq(ctx, UD_CMD_SETGIP3,
                        0xA2, 0xAA, 0x02, 0xA0, 0xA2, 0xA8, 0x02, 0xA0,
                        0xB0, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00);
    if (ret < 0) {
        dev_err(dev, "Failed to set d8 (%d)\n", ret);
        goto fail;
    }

    /* set bank1 */
    ret = dcs_write_seq(ctx, UD_CMD_SET_BANK, 0x01);
    if (ret < 0) {
        dev_err(dev, "Failed to set bank1 (%d)\n", ret);
        goto fail;
    }

    /* set d81 */
    ret = dcs_write_seq(ctx, UD_CMD_SETGIP3,
                        0xB0, 0x00, 0x00, 0x00, 0xB0, 0x00, 0x00, 0x00,
                        0xE2, 0xAA, 0x03, 0xF0, 0xE2, 0xAA, 0x03, 0xF0);
    if (ret < 0) {
        dev_err(dev, "Failed to set d81 (%d)\n", ret);
        goto fail;
    }

    /* set bank2 */
    ret = dcs_write_seq(ctx, UD_CMD_SET_BANK, 0x02);
    if (ret < 0) {
        dev_err(dev, "Failed to set bank2 (%d)\n", ret);
        goto fail;
    }

    /* set d82 */
    ret = dcs_write_seq(ctx, UD_CMD_SETGIP3,
                        0xE2, 0xAA, 0x03, 0xF0, 0xE2, 0xAA, 0x03, 0xF0);
    if (ret < 0) {
        dev_err(dev, "Failed to set d82 (%d)\n", ret);
        goto fail;
    }

    /* set bank0 */
    ret = dcs_write_seq(ctx, UD_CMD_SET_BANK, 0x00);
    if (ret < 0) {
        dev_err(dev, "Failed to set bank0 (%d)\n", ret);
        goto fail;
    }

    /* set vcom */
    ret = dcs_write_seq(ctx, UD_CMD_SETVCOM, 0x8D, 0x8D);
    if (ret < 0) {
        dev_err(dev, "Failed to set vcom (%d)\n", ret);
        goto fail;
    }

    /* set gamma */
    ret = dcs_write_seq(ctx, UD_CMD_SETGAMMA,
                        0x00, 0x0E, 0x19, 0x13, 0x2E, 0x39, 0x48, 0x44,
                        0x4D, 0x57, 0x5F, 0x66, 0x6C, 0x76, 0x7F, 0x85,
                        0x8A, 0x95, 0x9A, 0xA4, 0x9B, 0xAB, 0xB0, 0x5C,
                        0x58, 0x64, 0x77, 0x00, 0x0E, 0x19, 0x13, 0x2E,
                        0x39, 0x48, 0x44, 0x4D, 0x57, 0x5F, 0x66, 0x6C,
                        0x76, 0x7F, 0x85, 0x8A, 0x95, 0x9A, 0xA4, 0x9B,
                        0xAB, 0xB0, 0x5C, 0x58, 0x64, 0x77);
    if (ret < 0) {
        dev_err(dev, "Failed to set gamma (%d)\n", ret);
        goto fail;
    }

    usleep_range(10000, 11000);

    /* set panel */
    ret = dcs_write_seq(ctx, UD_CMD_SETPANEL, 0x08);
    if (ret < 0) {
        dev_err(dev, "Failed to set panel (%d)\n", ret);
        goto fail;
    }

    /* set sleep out */
    ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
    if (ret < 0) {
        dev_err(dev, "Failed to set sleep out (%d)\n", ret);
        goto fail;
    }

    usleep_range(120000, 121000);

    /* set display on */
    ret = mipi_dsi_dcs_set_display_on(dsi);
    if (ret < 0) {
        dev_err(dev, "Failed to set display ON (%d)\n", ret);
        goto fail;
    }

    usleep_range(20000, 21000);

    ctx->enabled = true;

    return 0;

fail:
    gpiod_set_value_cansleep(ctx->reset, 1);
    return ret;
}

static int himax_panel_enable(struct drm_panel *panel)
{
    struct himax_panel *ctx = to_himax_panel(panel);

    return ctx->pdata->enable(ctx);
}

static int himax_panel_disable(struct drm_panel *panel)
{
    struct himax_panel *ctx = to_himax_panel(panel);
    struct mipi_dsi_device *dsi = ctx->dsi;
    struct device *dev = &dsi->dev;
    int ret;

    /* return unless the panel has been enabled */
    if (!ctx->enabled)
        return 0;

    /* sent the command in low power mode */
    dsi->mode_flags |= MIPI_DSI_MODE_LPM;

    /* set the display off */
    ret = mipi_dsi_dcs_set_display_off(dsi);
    if (ret < 0) {
        dev_err(dev, "Failed to set display OFF (%d)\n", ret);
        return ret;
    }

    /* enter sleep mode */
    ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
    if (ret < 0) {
        dev_err(dev, "Failed to enter sleep mode (%d)\n", ret);
        return ret;
    }

    ctx->enabled = false;

    return 0;
}

static int himax_panel_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
    struct drm_display_mode *mode;

    /* allocate a memeory and duplicate the default timing */
    mode = drm_mode_duplicate(connector->dev, &default_mode);
    if (!mode) {
        dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
                default_mode.hdisplay,
                default_mode.vdisplay,
                drm_mode_vrefresh(&default_mode));
        return -ENOMEM;
    }

    /* set a name on mode */
    drm_mode_set_name(mode);

    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    /* add a mode to connector probed list */
    drm_mode_probed_add(connector, mode);

    connector->display_info.width_mm = mode->width_mm;
    connector->display_info.height_mm = mode->height_mm;
    connector->display_info.bus_flags = hx8399c_bus_flags;

    /* set bus formats */
    drm_display_info_set_bus_formats(&connector->display_info,
                                    hx8399c_bus_formats,
                                    ARRAY_SIZE(hx8399c_bus_formats));

    return 1;
}

static const struct drm_panel_funcs himax_panel_funcs = {
    .prepare = himax_panel_prepare,
    .unprepare = himax_panel_unprepare,
    .enable = himax_panel_enable,
    .disable = himax_panel_disable,
    .get_modes = himax_panel_get_modes,
};

static const struct himax_platform_data himax_hx8399c = {
    .enable = &hx8399c_enable,
};

static const struct of_device_id himax_of_match[] = {
    { .compatible = "himax,hx8399c", .data = &himax_hx8399c },
    { /* sentinel */}
};
MODULE_DEVICE_TABLE(of, himax_of_match);

static int himax_panel_probe(struct mipi_dsi_device *dsi)
{
    struct device *dev = &dsi->dev;
    const struct of_device_id *of_id = of_match_device(himax_of_match, dev);
    struct device_node *np = dev->of_node;
    struct himax_panel *ctx;
    int ret = 0;
    u32 video_mode;

    printk("Mackie");

    /* check device node existence */
    if (!of_id || !of_id->data)
        return -ENODEV;

    /* allocate memory for panel data */
    ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    /* bind panel data to dsi driver data */
    mipi_dsi_set_drvdata(dsi, ctx);

    ctx->dsi = dsi;
    ctx->pdata = of_id->data;

    dsi->format = MIPI_DSI_FMT_RGB888;
    /* enable hsync-end packets in vsync-pulse and v-porch area */
    /* disable end-of-transmission in hse mode */
    dsi->mode_flags = MIPI_DSI_MODE_VIDEO_HSE | MIPI_DSI_MODE_NO_EOT_PACKET;

    /* read video mode from device tree */
    ret = of_property_read_u32(np, "video-mode", &video_mode);
    if (!ret) {
        switch (video_mode) {
        case 0:
            /* burst mode */
            dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST |
                       MIPI_DSI_MODE_VIDEO;
            break;
        case 1:
            /* non-burst mode with sync event */
            dsi->mode_flags |= MIPI_DSI_MODE_VIDEO;
            break;
        case 2:
            /* non-burst mode with sync pulse */
            dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
                       MIPI_DSI_MODE_VIDEO;
            break;
        case 3:
            /* command mode */
            dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS |
                       MIPI_DSI_MODE_VSYNC_FLUSH;
            break;
        default:
            dev_err(dev, "invalid video mode %d\n", video_mode);
            break;
        }
    }

    /* read number of data lanes from device tree */
    ret = of_property_read_u32(np, "dsi-lanes", &dsi->lanes);
    if (ret) {
        dev_err(dev, "Failed to get dsi-lanes property (%d)\n", ret);
        return ret;
    }

    /* get reset gpio from device tree */
    ctx->reset = devm_gpiod_get_optional(dev, "reset",
                           GPIOD_OUT_LOW |
                           GPIOD_FLAGS_BIT_NONEXCLUSIVE);
    if (IS_ERR(ctx->reset)) {
        ret = PTR_ERR(ctx->reset);
        dev_err(dev, "Failed to get reset gpio (%d)\n", ret);
        return ret;
    }

    /* get backlight gpio from device tree */
    printk("Mackie: get backlight\n");
    ctx->backlight = devm_gpiod_get_optional(dev, "backlight",
                           GPIOD_OUT_HIGH |
                           GPIOD_FLAGS_BIT_NONEXCLUSIVE);
    if (IS_ERR(ctx->backlight)) {
        ret = PTR_ERR(ctx->backlight);
        dev_err(dev, "Failed to get backlight gpio (%d)\n", ret);
        printk("Mackie: Failed to get backlight gpio\n");
        return ret;
    }
    printk("Mackie: get backlight end\n");

    gpiod_set_value_cansleep(ctx->reset, 1);

    drm_panel_init(&ctx->panel, dev, &himax_panel_funcs, DRM_MODE_CONNECTOR_DSI);

    dev_set_drvdata(dev, ctx);

    drm_panel_add(&ctx->panel);

    ret = mipi_dsi_attach(dsi);

    if (ret) {
        drm_panel_remove(&ctx->panel);
        dev_err(dev, "Failed to attach mipi dsi\n");
    }

    printk("Exit himax_panel_probe\n");

    return 0;
}

static int himax_panel_remove(struct mipi_dsi_device *dsi)
{
    struct himax_panel *panel = mipi_dsi_get_drvdata(dsi);
    struct device *dev = &dsi->dev;
    int ret;

    ret = mipi_dsi_detach(dsi);
    if (ret)
        dev_err(dev, "Failed to detach from host (%d)\n", ret);

    drm_panel_remove(&panel->panel);

    return 0;
}

static void himax_panel_shutdown(struct mipi_dsi_device *dsi)
{
    struct himax_panel *panel = mipi_dsi_get_drvdata(dsi);

    himax_panel_disable(&panel->panel);
    himax_panel_unprepare(&panel->panel);
}

static struct mipi_dsi_driver himax_panel_driver = {
    .driver = {
        .name = "panel-himax-hx8399c",
        .of_match_table = himax_of_match,
    },
    .probe = himax_panel_probe,
    .remove = himax_panel_remove,
    .shutdown = himax_panel_shutdown,
};
module_mipi_dsi_driver(himax_panel_driver);

MODULE_AUTHOR("Mackie Xiao <mackiexiao@foxmail.com>");
MODULE_DESCRIPTION("DRM Driver for Himax HX8399C MIPI DSI panel");
MODULE_LICENSE("GPL v2");