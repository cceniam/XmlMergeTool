#include "candidate_view.hpp"
#include <QApplication>
#include <QDir>
#include <QDropEvent>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QMouseEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QUrl>

namespace xm::ui {
CandidateFilter::CandidateFilter(QObject *parent) : QSortFilterProxyModel(parent) {
    // A checked-only filter must not remove the row while itemChanged handlers
    // and native accessibility observers are still dispatching that edit.
    setDynamicSortFilter(false);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}
void CandidateFilter::setFilters(bool conflicts, bool selected, QString status, QString search) {
    conflicts_ = conflicts;
    selected_ = selected;
    status_ = status.trimmed().toUpper();
    search_ = search.trimmed().toCaseFolded();
    invalidate();
}
void CandidateFilter::refresh() {
    invalidate();
}
bool CandidateFilter::groupMatches(int row, const QModelIndex &parent) const {
    if (search_.isEmpty())
        return false;
    auto index = sourceModel()->index(row, SuitePath, parent);
    return (index.data().toString() + " " + index.data(FullPathRole).toString())
        .toCaseFolded()
        .contains(search_);
}
bool CandidateFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
    auto *source = sourceModel();
    auto check = source->index(row, Check, parent);
    if (check.data(NodeKindRole).toString() == "group") {
        if (groupMatches(row, parent))
            return true;
        for (int child = 0; child < source->rowCount(check); ++child)
            if (filterAcceptsRow(child, check))
                return true;
        return false;
    }
    if (selected_ && check.data(Qt::CheckStateRole).toInt() != Qt::Checked)
        return false;
    if (conflicts_ && source->index(row, ConflictSize, parent).data().toInt() < 2)
        return false;
    if (status_ != "ALL" &&
        source->index(row, Status, parent).data().toString().trimmed().toUpper() != status_)
        return false;
    if (search_.isEmpty())
        return true;
    const QString haystack = source->index(row, SuitePath, parent).data().toString() + " " +
                             source->index(row, File, parent).data().toString();
    if (haystack.toCaseFolded().contains(search_))
        return true;
    for (auto ancestor = parent; ancestor.isValid(); ancestor = ancestor.parent())
        if (groupMatches(ancestor.row(), ancestor.parent()))
            return true;
    return false;
}

CandidateView::CandidateView(QWidget *parent)
    : QTreeView(parent), rubber_(new QRubberBand(QRubberBand::Rectangle, viewport())) {
    setAcceptDrops(true);
    setDragEnabled(false);
    setDragDropMode(DropOnly);
    setSelectionMode(ExtendedSelection);
    setSelectionBehavior(SelectRows);
    setAlternatingRowColors(true);
    setUniformRowHeights(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    scrollTimer_.setInterval(50);
    connect(&scrollTimer_, &QTimer::timeout, this, [this] {
        if (!active_)
            return;
        const int edge = 20;
        int delta =
            current_.y() < edge ? -1 : (current_.y() >= viewport()->height() - edge ? 1 : 0);
        if (delta)
            verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
    });
}
QModelIndex CandidateView::rowAt(const QPoint &position) const {
    if (!model() || !model()->rowCount())
        return {};
    int column = 0;
    while (column < model()->columnCount() && isColumnHidden(column))
        ++column;
    if (column == model()->columnCount())
        return {};
    const int x = qBound(0, columnViewportPosition(column) + qMin(8, columnWidth(column) - 1),
                         qMax(0, viewport()->width() - 1));
    const int y = qBound(0, position.y(), qMax(0, viewport()->height() - 1));
    auto result = indexAt(QPoint(x, y));
    if (result.isValid())
        return result.siblingAtColumn(0);
    // Below the final row, select through the final currently displayed row.
    result = model()->index(0, 0);
    for (auto next = indexBelow(result); next.isValid(); next = indexBelow(result)) {
        if (visualRect(next).top() > y)
            break;
        result = next;
    }
    return result;
}
void CandidateView::selectRubberRange(const QModelIndex &anchor, const QPoint &position,
                                      bool additive) {
    if (!model() || !selectionModel() || !anchor.isValid())
        return;
    auto current = rowAt(position);
    if (!current.isValid())
        return;
    QList<QModelIndex> visibleRows;
    auto index = model()->index(0, 0);
    int first = -1, last = -1;
    while (index.isValid()) {
        if (index == anchor.siblingAtColumn(0))
            first = visibleRows.size();
        if (index == current)
            last = visibleRows.size();
        visibleRows.push_back(index);
        index = indexBelow(index);
    }
    if (first < 0 || last < 0)
        return;
    if (first > last)
        std::swap(first, last);
    QItemSelection selection;
    for (int row = first; row <= last; ++row)
        selection.select(visibleRows[row],
                         visibleRows[row].siblingAtColumn(model()->columnCount() - 1));
    selectionModel()->select(selection, QItemSelectionModel::Rows |
                                            (additive ? QItemSelectionModel::Select
                                                      : QItemSelectionModel::ClearAndSelect));
}
void CandidateView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        pressed_ = true;
        active_ = false;
        origin_ = current_ = event->position().toPoint();
        anchor_ = rowAt(origin_);
        additive_ = event->modifiers().testFlag(Qt::ControlModifier) ||
                    event->modifiers().testFlag(Qt::MetaModifier);
    }
    QTreeView::mousePressEvent(event);
}
void CandidateView::updateRubber() {
    if (!active_)
        return;
    rubber_->setGeometry(QRect(origin_, current_).normalized());
    selectRubberRange(anchor_, current_, additive_);
}
void CandidateView::mouseMoveEvent(QMouseEvent *event) {
    if (pressed_ && event->buttons().testFlag(Qt::LeftButton)) {
        current_ = event->position().toPoint();
        if (!active_ &&
            (current_ - origin_).manhattanLength() >= QApplication::startDragDistance()) {
            active_ = true;
            rubber_->show();
            scrollTimer_.start();
        }
        if (active_) {
            updateRubber();
            event->accept();
            return;
        }
    }
    QTreeView::mouseMoveEvent(event);
}
void CandidateView::mouseReleaseEvent(QMouseEvent *event) {
    const bool wasActive = active_;
    if (event->button() == Qt::LeftButton) {
        pressed_ = active_ = false;
        anchor_ = QPersistentModelIndex();
        rubber_->hide();
        scrollTimer_.stop();
    }
    if (wasActive)
        event->accept();
    else
        QTreeView::mouseReleaseEvent(event);
}
void CandidateView::scrollContentsBy(int dx, int dy) {
    QTreeView::scrollContentsBy(dx, dy);
    updateRubber();
}
void CandidateView::dragEnterEvent(QDragEnterEvent *event) {
    if (isEnabled() && event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}
void CandidateView::dragMoveEvent(QDragMoveEvent *event) {
    if (isEnabled() && event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        event->ignore();
}
void CandidateView::dropEvent(QDropEvent *event) {
    QStringList paths;
    if (!isEnabled()) {
        event->ignore();
        return;
    }
    for (const auto &url : event->mimeData()->urls()) {
        auto path = url.toLocalFile();
        if (QFileInfo(path).isDir()) {
            for (const auto &entry : QDir(path).entryInfoList(QDir::Files, QDir::Name))
                if (entry.suffix().compare("xml", Qt::CaseInsensitive) == 0)
                    paths.push_back(entry.absoluteFilePath());
        } else if (path.endsWith(".xml", Qt::CaseInsensitive))
            paths.push_back(path);
    }
    if (paths.isEmpty())
        event->ignore();
    else {
        emit filesDropped(paths);
        event->acceptProposedAction();
    }
}
} // namespace xm::ui
