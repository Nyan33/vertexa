// SPDX-License-Identifier: GPL-3.0-or-later
#include "FlaImport.h"

namespace vx::io {

bool importXfl(const QString&, Document&, ImportReport&, QString* error)
{
    if (error) *error = QStringLiteral("XFL import is not available yet");
    return false;
}

} // namespace vx::io
