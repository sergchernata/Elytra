/*
 * Small widgets for the Arc sidebar: a centered symbol button and the downloads shelf.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>
#include <UI/Qt/Arc/ArcSidebar.h>
#include <UI/Qt/Arc/SidebarWidgets.h>
#include <UI/Qt/Arc/SymbolIcon.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QEnterEvent>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

namespace Ladybird::Arc {

// --- SymbolButton -----------------------------------------------------------------------------

SymbolButton::SymbolButton(QWidget* parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setFixedSize(sizeHint());
}

void SymbolButton::set_icon_text(QString const& icon)
{
    if (is_symbol_icon(icon)) {
        set_symbol(symbol_name(icon));
        return;
    }
    m_symbol.clear();
    m_text = icon;
    update();
}

void SymbolButton::set_symbol(QString const& name)
{
    m_symbol = name;
    m_text.clear();
    update();
}

void SymbolButton::set_dimmed(bool dimmed)
{
    m_dimmed = dimmed;
    update();
}

void SymbolButton::set_symbol_point_size(int size)
{
    m_point_size = size;
    update();
}

void SymbolButton::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void SymbolButton::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void SymbolButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    auto dark = ChromeStyle::is_dark(palette());

    if (m_hovered || isDown() || isChecked()) {
        painter.setPen(Qt::NoPen);
        auto alpha = isDown() ? 40 : (isChecked() ? 34 : 24);
        painter.setBrush(dark ? QColor(255, 255, 255, alpha) : QColor(0, 0, 0, alpha / 2));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
    }

    auto color = ChromeStyle::chrome_text(palette());
    painter.setOpacity(m_dimmed && !m_hovered ? 0.42 : 0.92);

    if (!m_symbol.isEmpty()) {
        auto pixmap = fitted_symbol_pixmap(m_symbol, m_point_size, color, devicePixelRatioF());
        if (!pixmap.isNull()) {
            auto logical = pixmap.deviceIndependentSize();
            auto top_left = QPointF((width() - logical.width()) / 2.0, (height() - logical.height()) / 2.0);
            painter.drawPixmap(top_left, pixmap);
            return;
        }
    }

    // No symbol (or not on macOS): a centered letter or emoji.
    auto text = m_text.isEmpty() ? m_symbol.left(1).toUpper() : m_text;
    auto font = painter.font();
    font.setPixelSize(m_point_size - 1);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(rect(), Qt::AlignCenter, text);
}

// --- DownloadsShelf ---------------------------------------------------------------------------

static constexpr int SHELF_ROW_HEIGHT = 56;
static constexpr int SHELF_FADE_HEIGHT = 96;
static constexpr int SHELF_MAX_ITEMS = 6;
static constexpr int SHELF_ICON_SIZE = 34;

static QString relative_time(QDateTime const& time)
{
    auto seconds = time.secsTo(QDateTime::currentDateTime());
    if (seconds < 60)
        return "Just now";
    if (seconds < 3600)
        return seconds < 120 ? "1 minute ago" : QString("%1 minutes ago").arg(seconds / 60);
    if (seconds < 86400)
        return seconds < 7200 ? "1 hour ago" : QString("%1 hours ago").arg(seconds / 3600);
    auto days = seconds / 86400;
    if (days < 30)
        return days == 1 ? "1 day ago" : QString("%1 days ago").arg(days);
    return time.date().toString("MMM d");
}

static bool is_partial_download(QString const& name)
{
    static QStringList const suffixes { ".download", ".part", ".crdownload", ".partial", ".tmp" };
    for (auto const& suffix : suffixes) {
        if (name.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    }
    return name.startsWith('.');
}

DownloadsShelf::DownloadsShelf(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
}

void DownloadsShelf::refresh()
{
    QList<Item> items;
    QSet<QString> seen;

    // Ladybird's own downloads from this session, including ones still running.
    for (auto const& download : WebView::Application::the().file_downloader().downloads()) {
        if (download.is_private == WebView::IsPrivate::Yes)
            continue;
        using Status = WebView::FileDownloader::DownloadStatus;
        if (download.status == Status::Canceled || download.status == Status::Failed)
            continue;

        Item item;
        auto const& path = download.destination.string();
        item.path = QString::fromUtf8(path.characters(), static_cast<qsizetype>(path.length()));
        item.name = QFileInfo(item.path).fileName();
        item.in_progress = WebView::FileDownloader::status_is_active(download.status);
        if (item.in_progress) {
            if (auto progress = download.progress(); progress.has_value())
                item.percent = static_cast<int>(*progress * 100);
            item.time = QDateTime::fromSecsSinceEpoch(download.created_time.seconds_since_epoch());
        } else {
            QFileInfo info(item.path);
            if (!info.exists())
                continue;
            item.time = info.lastModified();
        }
        seen.insert(item.path);
        items.append(item);
    }

    // Plus the newest files in the Downloads folder, so earlier sessions (and other apps) show up too.
    QDir downloads(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    auto entries = downloads.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (auto const& info : entries) {
        if (items.size() >= SHELF_MAX_ITEMS * 2)
            break;
        if (is_partial_download(info.fileName()) || seen.contains(info.absoluteFilePath()))
            continue;
        items.append({ info.absoluteFilePath(), info.fileName(), info.lastModified(), -1, false });
    }

    std::stable_sort(items.begin(), items.end(), [](Item const& a, Item const& b) {
        if (a.in_progress != b.in_progress)
            return a.in_progress;
        return a.time > b.time;
    });
    if (items.size() > SHELF_MAX_ITEMS)
        items.resize(SHELF_MAX_ITEMS);

    m_items = items;
    m_hovered_row = -1;
    update();
}

int DownloadsShelf::preferred_height() const
{
    return SHELF_FADE_HEIGHT + static_cast<int>(m_items.size()) * SHELF_ROW_HEIGHT + 4;
}

QRect DownloadsShelf::row_rect(int index) const
{
    // Items are newest-first; the newest sits at the bottom, nearest the button, as in Arc.
    auto top = height() - 4 - (index + 1) * SHELF_ROW_HEIGHT;
    return QRect(0, top, width(), SHELF_ROW_HEIGHT);
}

int DownloadsShelf::row_at(QPoint position) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (row_rect(i).contains(position))
            return i;
    }
    return -1;
}

void DownloadsShelf::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // Fade from clear to the sidebar color, so the rows above appear to dissolve.
    auto background = ChromeStyle::chrome_background(palette());
    auto clear = background;
    clear.setAlpha(0);
    QLinearGradient fade(0, 0, 0, SHELF_FADE_HEIGHT);
    fade.setColorAt(0, clear);
    fade.setColorAt(1, background);
    painter.fillRect(QRect(0, 0, width(), SHELF_FADE_HEIGHT), fade);
    painter.fillRect(QRect(0, SHELF_FADE_HEIGHT, width(), height() - SHELF_FADE_HEIGHT), background);

    static QFileIconProvider icon_provider;
    auto text = ChromeStyle::chrome_text(palette());
    auto muted = ChromeStyle::chrome_muted_text(palette());
    auto dark = ChromeStyle::is_dark(palette());

    for (int i = 0; i < m_items.size(); ++i) {
        auto const& item = m_items[i];
        auto row = row_rect(i);

        if (i == m_hovered_row) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 10));
            painter.drawRoundedRect(QRectF(row).adjusted(0, 2, -2, -2), 9, 9);
        }

        auto icon_rect = QRect(row.left() + 6, row.center().y() - SHELF_ICON_SIZE / 2, SHELF_ICON_SIZE, SHELF_ICON_SIZE);
        icon_provider.icon(QFileInfo(item.path)).paint(&painter, icon_rect);

        auto text_left = icon_rect.right() + 12;
        auto text_width = row.right() - 8 - text_left;

        auto name_font = font();
        name_font.setPixelSize(14);
        painter.setFont(name_font);
        painter.setPen(text);
        auto name = QFontMetrics(name_font).elidedText(item.name, Qt::ElideRight, text_width);
        painter.drawText(QRect(text_left, row.top() + 9, text_width, 20), Qt::AlignLeft | Qt::AlignVCenter, name);

        auto detail_font = font();
        detail_font.setPixelSize(12);
        painter.setFont(detail_font);
        painter.setPen(muted);
        QString detail;
        if (item.in_progress)
            detail = item.percent >= 0 ? QString("Downloading… %1%").arg(item.percent) : QString("Downloading…");
        else
            detail = relative_time(item.time);
        painter.drawText(QRect(text_left, row.top() + 29, text_width, 18), Qt::AlignLeft | Qt::AlignVCenter, detail);

        if (item.in_progress && item.percent >= 0) {
            auto bar = QRectF(text_left, row.bottom() - 6, text_width, 3);
            painter.setPen(Qt::NoPen);
            painter.setBrush(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 20));
            painter.drawRoundedRect(bar, 1.5, 1.5);
            painter.setBrush(ChromeStyle::chrome_accent());
            painter.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * item.percent / 100.0, bar.height()), 1.5, 1.5);
        }
    }
}

void DownloadsShelf::mouseMoveEvent(QMouseEvent* event)
{
    auto position = event->position().toPoint();

    // Drag a file out of the shelf, e.g. into a Finder window or an upload field.
    if (m_pressed_row >= 0 && (event->buttons() & Qt::LeftButton)
        && (position - m_press_position).manhattanLength() >= QApplication::startDragDistance()) {
        auto const& item = m_items[m_pressed_row];
        if (!item.in_progress) {
            auto* mime = new QMimeData;
            mime->setUrls({ QUrl::fromLocalFile(item.path) });
            auto* drag = new QDrag(this);
            drag->setMimeData(mime);
            static QFileIconProvider icon_provider;
            drag->setPixmap(icon_provider.icon(QFileInfo(item.path)).pixmap(SHELF_ICON_SIZE, SHELF_ICON_SIZE));
            m_pressed_row = -1;
            drag->exec(Qt::CopyAction);
            return;
        }
    }

    auto row = row_at(position);
    if (row != m_hovered_row) {
        m_hovered_row = row;
        setCursor(row >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void DownloadsShelf::mousePressEvent(QMouseEvent* event)
{
    auto row = row_at(event->position().toPoint());
    if (event->button() == Qt::RightButton) {
        if (row >= 0)
            show_menu(row, event->globalPosition().toPoint());
        return;
    }
    m_pressed_row = row;
    m_press_position = event->position().toPoint();
}

void DownloadsShelf::mouseReleaseEvent(QMouseEvent* event)
{
    auto row = row_at(event->position().toPoint());
    if (event->button() == Qt::LeftButton && row >= 0 && row == m_pressed_row && !m_items[row].in_progress)
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_items[row].path));
    m_pressed_row = -1;
}

void DownloadsShelf::leaveEvent(QEvent* event)
{
    m_hovered_row = -1;
    update();
    QWidget::leaveEvent(event);
}

void DownloadsShelf::show_menu(int index, QPoint global_position)
{
    auto path = m_items[index].path;
    QMenu menu(this);
    menu.addAction("Open", [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
#if defined(AK_OS_MACOS)
    menu.addAction("Show in Finder", [path] { QProcess::startDetached("open", { "-R", path }); });
#else
    menu.addAction("Show in Folder", [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
#endif
    menu.addAction("Open Downloads Folder", [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)));
    });
    KeepSidebarOpen keep_open(this);
    menu.exec(global_position);
}

}
