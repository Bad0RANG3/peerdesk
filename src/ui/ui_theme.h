#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;
class QPainter;
class QSize;

namespace pd::ui {

[[nodiscard]] QColor background();
[[nodiscard]] QColor surface();
[[nodiscard]] QColor surfaceRaised();
[[nodiscard]] QColor text();
[[nodiscard]] QColor textDim();
[[nodiscard]] QColor textFaint();
[[nodiscard]] QColor blue();
[[nodiscard]] QColor blueBright();
[[nodiscard]] QColor cyan();
[[nodiscard]] QColor green();
[[nodiscard]] QColor amber();
[[nodiscard]] QColor red();

[[nodiscard]] QFont uiFont(int pointSize = 10, int weight = -1);
[[nodiscard]] QFont monoFont(int pointSize = 10, int weight = -1);

void applyTheme(QApplication &application);
void paintBackdrop(QPainter &painter, const QSize &size);
[[nodiscard]] QString styleSheet();

[[nodiscard]] QString formatConnectionCode(const QString &value);
[[nodiscard]] QString compactConnectionCode(const QString &value);

} // namespace pd::ui
