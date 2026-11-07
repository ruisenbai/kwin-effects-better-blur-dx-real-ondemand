/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "blur.h"

namespace BBDXReal {

KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(BlurEffect,
                                      "metadata.json",
                                      return BlurEffect::supported();
                                      ,
                                      return BlurEffect::enabledByDefault();)

} // namespace BBDXReal

#include "main.moc"
