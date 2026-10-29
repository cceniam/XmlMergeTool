#pragma once

#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QTreeView>

class QRubberBand;

namespace xm::ui {
enum Column { Check, SuitePath, Status, Default, ConflictSize, StartTime, File, ColumnCount };
enum Role { CandidateIdRole = Qt::UserRole + 1, NodeKindRole, FullPathRole };

class CandidateFilter final : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    explicit CandidateFilter(QObject *parent = nullptr);
    void setFilters(bool conflicts, bool selected, QString status, QString search);
    void refresh();

  protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override;

  private:
    bool groupMatches(int row, const QModelIndex &parent) const;
    bool conflicts_ = false, selected_ = false;
    QString status_ = "ALL", search_;
};

class CandidateView final : public QTreeView {
    Q_OBJECT
  public:
    explicit CandidateView(QWidget *parent = nullptr);
    // Used by drag selection and its regression tests; spans visible tree rows.
    void selectRubberRange(const QModelIndex &anchor, const QPoint &position,
                           bool additive = false);
  signals:
    void filesDropped(const QStringList &paths);

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

  private:
    QModelIndex rowAt(const QPoint &position) const;
    void updateRubber();
    QRubberBand *rubber_;
    QPersistentModelIndex anchor_;
    QPoint origin_, current_;
    bool pressed_ = false, active_ = false, additive_ = false;
    QTimer scrollTimer_;
};
} // namespace xm::ui
