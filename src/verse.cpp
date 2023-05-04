/////////////////////////////////////////////////////////////////////////////
// Name:        verse.cpp
// Author:      Laurent Pugin
// Created:     2014
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#include "verse.h"

//----------------------------------------------------------------------------

#include <cassert>

//----------------------------------------------------------------------------

#include "comparison.h"
#include "doc.h"
#include "editorial.h"
#include "functor.h"
#include "functorparams.h"
#include "label.h"
#include "labelabbr.h"
#include "layer.h"
#include "staff.h"
#include "syl.h"
#include "verticalaligner.h"
#include "vrv.h"

namespace vrv {

//----------------------------------------------------------------------------
// Verse
//----------------------------------------------------------------------------

static const ClassRegistrar<Verse> s_factory("verse", VERSE);

Verse::Verse() : LayerElement(VERSE, "verse-"), AttColor(), AttLang(), AttNInteger(), AttTypography()
{
    this->RegisterAttClass(ATT_COLOR);
    this->RegisterAttClass(ATT_LANG);
    this->RegisterAttClass(ATT_NINTEGER);
    this->RegisterAttClass(ATT_TYPOGRAPHY);

    this->Reset();
}

Verse::~Verse() {}

void Verse::Reset()
{
    LayerElement::Reset();
    this->ResetColor();
    this->ResetLang();
    this->ResetNInteger();
    this->ResetTypography();

    m_drawingLabelAbbr = NULL;
}

bool Verse::IsSupportedChild(Object *child)
{
    if (child->Is(LABEL)) {
        assert(dynamic_cast<Label *>(child));
    }
    else if (child->Is(LABELABBR)) {
        assert(dynamic_cast<LabelAbbr *>(child));
    }
    else if (child->Is(SYL)) {
        assert(dynamic_cast<Syl *>(child));
    }
    else if (child->IsEditorialElement()) {
        assert(dynamic_cast<EditorialElement *>(child));
    }
    else {
        return false;
    }
    return true;
}

int Verse::AdjustPosition(int &overlap, int freeSpace, const Doc *doc)
{
    assert(doc);

    int nextFreeSpace = 0;

    if (overlap > 0) {
        // We have enough space to absorb the overlay completely
        if (freeSpace > overlap) {
            this->SetDrawingXRel(this->GetDrawingXRel() - overlap);
            // The space is set to 0. This means that consecutive overlaps will not be recursively absorbed.
            // Only the first preceding syl will be moved.
            overlap = 0;
        }
        else if (freeSpace > 0) {
            this->SetDrawingXRel(this->GetDrawingXRel() - freeSpace);
            overlap -= freeSpace;
        }
    }
    else {
        nextFreeSpace = std::min(-overlap, 3 * doc->GetDrawingUnit(100));
    }

    return nextFreeSpace;
}

//----------------------------------------------------------------------------
// Verse functor methods
//----------------------------------------------------------------------------

FunctorCode Verse::Accept(MutableFunctor &functor)
{
    return functor.VisitVerse(this);
}

FunctorCode Verse::Accept(ConstFunctor &functor) const
{
    return functor.VisitVerse(this);
}

FunctorCode Verse::AcceptEnd(MutableFunctor &functor)
{
    return functor.VisitVerseEnd(this);
}

FunctorCode Verse::AcceptEnd(ConstFunctor &functor) const
{
    return functor.VisitVerseEnd(this);
}

} // namespace vrv
