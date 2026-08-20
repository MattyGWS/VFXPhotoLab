#pragma once

#include <QImage>
#include <QString>

namespace vfx {

class TgaCodec final {
public:
    static QImage read(const QString &filePath, QString *errorMessage = nullptr);
    static bool write(const QString &filePath,
                      const QImage &image,
                      QString *errorMessage = nullptr);
};

} // namespace vfx
