/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "ui_blur_config.h"
#include <KCModule>
#include <QWidget>
#include <KContextualHelpButton>

namespace BBDXReal {

class BlurEffectConfig : public KCModule {
    Q_OBJECT

private:
    ::Ui::BlurEffectConfig ui;

    void setContextualHelp(
        KContextualHelpButton *const contextualHelpButton,
        const QString &text,
        QWidget *const heightHintWidget = nullptr
    );
    void setupContextualHelp();
    void setupSpinboxSliderSync();

    /**
     * Fixed compile time constraints
     */
    void setupConstraints();

public:
    explicit BlurEffectConfig(QObject *parent, const KPluginMetaData &data);
    ~BlurEffectConfig() override;

    void save() override;

public Q_SLOTS:
    void slotRefractionModeChanged(int index);
};

} // namespace BBDXReal
