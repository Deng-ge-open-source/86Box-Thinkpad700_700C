/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Software renderer module.
 *
 * Authors: Joakim L. Gilje <jgilje@jgilje.net>
 *          Cacodemon345
 *          Teemu Korhonen
 *
 *          Copyright 2021 Joakim L. Gilje
 *          Copyright 2021-2022 Cacodemon345
 *          Copyright 2021-2022 Teemu Korhonen
 */
#include "qt_softwarerenderer.hpp"
#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include "qt_util.hpp"

extern void standalone_scale(QRect &destination, int width, int height, QRect source, int scalemode);

extern "C" {
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/machine.h>
#include <86box/path.h>
#include <86box/plat.h>
#include <86box/video.h>
#include <86box/vid_lcd.h>
}

SoftwareRenderer::SoftwareRenderer(QWidget *parent)
    : QWidget(parent)
{
    RendererCommon::parentWidget = parent;

    images[0] = std::make_unique<QImage>(QSize(2048, 2048), QImage::Format_RGB32);
    images[1] = std::make_unique<QImage>(QSize(2048, 2048), QImage::Format_RGB32);

    buf_usage = std::vector<std::atomic_flag>(2);
    buf_usage[0].clear();
    buf_usage[1].clear();
    this->setMouseTracking(true);
}

void
SoftwareRenderer::paintEvent(QPaintEvent *event)
{
    (void) event;
    onPaint(this);
}

void
SoftwareRenderer::render()
{
#ifdef __HAIKU__
    if (!isExposed())
        return;
#endif

    QRect rect(0, 0, width(), height());
    m_backingStore->beginPaint(rect);

    QPaintDevice *device = m_backingStore->paintDevice();
    onPaint(device);

    m_backingStore->endPaint();
    m_backingStore->flush(rect);
}

void
SoftwareRenderer::exposeEvent(QExposeEvent *event)
{
    render();
}

extern void take_screenshot_clipboard_monitor(int sx, int sy, int sw, int sh, int i);

void
SoftwareRenderer::onBlit(int buf_idx, int x, int y, int w, int h)
{
    /* TODO: should look into deleteLater() */
    auto tval = this;
    if ((void *) tval == nullptr)
        return;
    auto origSource = source;

    cur_image = buf_idx;
    buf_usage[buf_idx ^ 1].clear();

    source.setRect(x, y, w, h);

    if (source != origSource)
        onResize(this->width(), this->height());

    update();

    if (monitors[r_monitor_index].mon_screenshots) {
        char path[1024];
        char fn[256];

        memset(fn, 0, sizeof(fn));
        memset(path, 0, sizeof(path));

        path_append_filename(path, usr_path, SCREENSHOT_PATH);

        if (!plat_dir_check(path))
            plat_dir_create(path);

        path_slash(path);
        strcat(path, "Monitor_");
        snprintf(&path[strlen(path)], 42, "%d_", r_monitor_index + 1);

        plat_tempfile(fn, NULL, (char *) ".png");
        strcat(path, fn);

        qreal win_scale = util::screenOfWidget(this)->devicePixelRatio();
        QSize qs = RendererCommon::parentWidget->size();
        QPixmap pixmap(RendererCommon::parentWidget->size());
        RendererCommon::parentWidget->render(&pixmap);
        QImage image;
        if (win_scale == 1.0)
            image = pixmap.toImage();
        else
            image = pixmap.toImage().scaled(qs * win_scale, Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation);
        image.save(path, "png");
        monitors[r_monitor_index].mon_screenshots--;
    }
    if (monitors[r_monitor_index].mon_screenshots_clipboard) {
        qreal win_scale = util::screenOfWidget(this)->devicePixelRatio();
        QSize qs = RendererCommon::parentWidget->size();
        QPixmap pixmap(RendererCommon::parentWidget->size());
        RendererCommon::parentWidget->render(&pixmap);
        QImage image;
        if (win_scale == 1.0)
            image = pixmap.toImage();
        else
            image = pixmap.toImage().scaled(qs * win_scale, Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation);
        QClipboard *clipboard = QApplication::clipboard();
        clipboard->setImage(image, QClipboard::Clipboard);
        monitors[r_monitor_index].mon_screenshots_clipboard--;
    }
    if (monitors[r_monitor_index].mon_screenshots_raw_clipboard) {
        take_screenshot_clipboard_monitor(x, y, w, h, r_monitor_index);
    }
}

void
SoftwareRenderer::resizeEvent(QResizeEvent *event)
{
    onResize(width(), height());
    QWidget::resizeEvent(event);
}

bool
SoftwareRenderer::event(QEvent *event)
{
    bool res = false;
    if (!eventDelegate(event, res))
        return QWidget::event(event);
    return res;
}

void
SoftwareRenderer::onPaint(QPaintDevice *device)
{
    if (cur_image == -1)
        return;

    QPainter painter(device);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, video_filter_method > 0 ? true : false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    painter.fillRect(0, 0, device->width(), device->height(), QColorConstants::Black);
#else
    painter.fillRect(0, 0, device->width(), device->height(), Qt::black);
#endif
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    QRect lcdDestination = destination;

    /*
     * The ThinkPad 700 reference programs use a 400-line VGA image, while
     * the notebook's 640x480 STN panel centers that image vertically.  A
     * generic PS/1 VGA stretches it to all 480 lines, making menu cards and
     * fonts visibly taller than on the real 9552.  Reproduce the internal
     * LCD's 400/480 mapping for this machine only.
     */
    if (!strcmp(machine_get_internal_name(), "ibmtp700")) {
        const int lcdHeight = (destination.height() * 5) / 6;
        lcdDestination.setTop(destination.top() + ((destination.height() - lcdHeight) / 2));
        lcdDestination.setHeight(lcdHeight);
    } else if (!strcmp(machine_get_internal_name(), "ibmtp700c")) {
        char expansionOption[] = "lcd_vertical_expansion";

        /* The physical TFT is always 640x480.  With vertical expansion off,
           an 80x25, 8x16 text screen occupies 400 centered panel rows.
           Expanding every text mode to 480 makes the IBM logo and glyphs
           20 percent too tall.  True 480-line graphics uses the full panel. */
        standalone_scale(lcdDestination, device->width(), device->height(), source, FULLSCR_SCALE_43);
        if ((source.height() > 0) && (source.height() < 480) &&
            !machine_get_config_int(expansionOption)) {
            const int lcdHeight = qMax(1, (lcdDestination.height() * source.height()) / 480);
            lcdDestination.setTop(lcdDestination.top() + ((lcdDestination.height() - lcdHeight) / 2));
            lcdDestination.setHeight(lcdHeight);
        }
    }

    if (machine_is_thinkpad_700() &&
        (tp700_lcd_brightness != 100 || tp700_lcd_contrast != 100)) {
        if (lcdBrightness != tp700_lcd_brightness || lcdContrast != tp700_lcd_contrast) {
            lcdBrightness = tp700_lcd_brightness;
            lcdContrast = tp700_lcd_contrast;
            tp700_lcd_build_lut(lcdLut.data(), lcdBrightness, lcdContrast);
        }
        if (lcdImage.size() != source.size())
            lcdImage = QImage(source.size(), QImage::Format_RGB32);
        if (!lcdImage.isNull()) {
            for (int y = 0; y < source.height(); ++y) {
                const auto *src = reinterpret_cast<const uint32_t *>(images[cur_image]->constScanLine(source.y() + y)) + source.x();
                auto *dst = reinterpret_cast<uint32_t *>(lcdImage.scanLine(y));
                for (int x = 0; x < source.width(); ++x)
                    dst[x] = tp700_lcd_pixel(src[x], lcdLut.data());
            }
            painter.drawImage(lcdDestination, lcdImage, lcdImage.rect());
        } else
            painter.drawImage(lcdDestination, *images[cur_image], source);
    } else
        painter.drawImage(lcdDestination, *images[cur_image], source);
    painter.end();
}

std::vector<std::tuple<uint8_t *, std::atomic_flag *>>
SoftwareRenderer::getBuffers()
{
    std::vector<std::tuple<uint8_t *, std::atomic_flag *>> buffers;

    buffers.push_back(std::make_tuple(images[0]->bits(), &buf_usage[0]));
    buffers.push_back(std::make_tuple(images[1]->bits(), &buf_usage[1]));

    return buffers;
}
