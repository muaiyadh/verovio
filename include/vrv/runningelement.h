/////////////////////////////////////////////////////////////////////////////
// Name:        runningelement.h
// Author:      Laurent Pugin
// Created:     2017
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#ifndef __VRV_RUNNING_ELEMENT_H__
#define __VRV_RUNNING_ELEMENT_H__

#include "atts_shared.h"
#include "object.h"

namespace vrv {

class Page;
class Staff;
    
//----------------------------------------------------------------------------
// RunningElement
//----------------------------------------------------------------------------

/**
 * This class represents running elements (headers and footers).
 * It is not an abstract class but should not be instanciated directly.
 */
class RunningElement : public Object, public ObjectListInterface, public AttHorizontalAlign, public AttTyped {
public:
    /**
     * @name Constructors, destructors, reset methods
     * Reset method resets all attribute classes
     */
    ///@{
    RunningElement();
    RunningElement(std::string classid);
    virtual ~RunningElement();
    virtual void Reset();
    virtual ClassId GetClassId() const { return RUNNING_ELEMENT; }
    ///@}
    
    /**
     * Disable cloning of the running elements (for now?).
     * It does not make sense you carry copying the running element acrosse the systems.
     */
    virtual Object *Clone() const { return NULL; }
    
    
    /**
     * @name Methods for adding allowed content
     */
    ///@{
    virtual void AddChild(Object *object);
    ///@}
    
    /**
     * @name Get and set the X and Y drawing position
     */
    ///@{
    virtual int GetDrawingX() const;
    virtual int GetDrawingY() const;
    ///@}
    
    int GetWidth() const;
    
    /*
     * @name Setter and getter for the current drawing page
     */
    ///@{
    void SetDrawingPage(Page *page);
    Page *GetDrawingPage() { return m_drawingPage; }
    ///@}
    
    /**
     * @name Get and set the X and Y drawing relative positions
     */
    ///@{
    int GetDrawingYRel() const { return m_drawingYRel; }
    virtual void SetDrawingYRel(int drawingYRel);
    ///@}
    
    int CalcTotalHeight();
    
    bool AdjustDrawingScaling(int width);

    //----------//
    // Functors //
    //----------//
    
    /**
     * See Object::AlignVertically
     */
    ///@{
    virtual int AlignVertically(FunctorParams *functorParams);
    ///@}
    
protected:
    /**
     * Filter the list for a specific class.
     * Keep only the top <rend> and <fig>
     */
    virtual void FilterList(ListOfObjects *childList);

private:
    /**
     *
     */
    int GetAlignmentPos(data_HORIZONTALALIGNMENT h, data_VERTICALALIGNMENT v);

public:
    //
private:
    /**
     * The page we are drawing (for the x position)
     */
    Page *m_drawingPage;
    
    /**
     * The y position of the running element
     */
    int m_drawingYRel;
    
    /**
     * Stored the top <rend> or <fig> with the 9 possible positioning combinations, from
     * top-left to bottom-right (going left to right first)
     */
    ArrayOfObjects m_positionnedObjects[9];
    
    /**
     *
     */
    int m_drawingScalingPercent[3];
};

} // namespace vrv

#endif
