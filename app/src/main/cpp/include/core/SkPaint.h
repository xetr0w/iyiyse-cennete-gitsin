#pragma once
#include "SkBlendMode.h"
class SkPaint {
public:
    enum Style { kFill_Style, kStroke_Style };
    enum Cap { kSquare_Cap, kRound_Cap };
    enum Join { kRound_Join };

    void reset() {}
    void setAntiAlias(bool) {}
    void setStyle(Style) {}
    void setBlendMode(SkBlendMode) {}
    void setColor(int) {}
    void setAlphaf(float) {}
    void setStrokeWidth(float) {}
    void setStrokeCap(Cap) {}
    void setStrokeJoin(Join) {}
};
