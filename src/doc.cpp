/////////////////////////////////////////////////////////////////////////////
// Name:        doc.cpp
// Author:      Laurent Pugin
// Created:     2005
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#include "doc.h"

//----------------------------------------------------------------------------

#include <assert.h>
#include <math.h>

//----------------------------------------------------------------------------

#include "attcomparison.h"
#include "barline.h"
#include "chord.h"
#include "functorparams.h"
#include "glyph.h"
#include "keysig.h"
#include "label.h"
#include "layer.h"
#include "mdiv.h"
#include "measure.h"
#include "mensur.h"
#include "metersig.h"
#include "mrest.h"
#include "multirest.h"
#include "note.h"
#include "page.h"
#include "pages.h"
#include "pgfoot.h"
#include "pgfoot2.h"
#include "pghead.h"
#include "pghead2.h"
#include "rpt.h"
#include "runningelement.h"
#include "score.h"
#include "slur.h"
#include "smufl.h"
#include "staff.h"
#include "staffdef.h"
#include "staffgrp.h"
#include "syl.h"
#include "system.h"
#include "verse.h"
#include "vrv.h"

//----------------------------------------------------------------------------

#include "MidiFile.h"

namespace vrv {

//----------------------------------------------------------------------------
// Doc
//----------------------------------------------------------------------------

Doc::Doc() : Object("doc-")
{
    m_options = new Options();

    Reset();
}

Doc::~Doc()
{
    delete m_options;
}

void Doc::Reset()
{
    Object::Reset();

    m_type = Raw;
    m_pageWidth = -1;
    m_pageHeight = -1;
    m_pageMarginBottom = 0;
    m_pageMarginRight = 0;
    m_pageMarginLeft = 0;
    m_pageMarginTop = 0;

    m_drawingPage = NULL;
    m_currentScoreDefDone = false;
    m_drawingPreparationDone = false;
    m_hasMidiTimemap = false;
    m_hasAnalyticalMarkup = false;

    m_scoreDef.Reset();

    m_drawingSmuflFontSize = 0;
    m_drawingLyricFontSize = 0;
}

void Doc::SetType(DocType type)
{
    m_type = type;
}

void Doc::AddChild(Object *child)
{
    if (child->Is(MDIV)) {
        assert(dynamic_cast<Mdiv *>(child));
    }
    else {
        LogError("Adding '%s' to a '%s'", child->GetClassName().c_str(), this->GetClassName().c_str());
        assert(false);
    }

    child->SetParent(this);
    m_children.push_back(child);
    Modify();
}

void Doc::Refresh()
{
    RefreshViews();
}

bool Doc::GenerateDocumentScoreDef()
{
    Measure *measure = dynamic_cast<Measure *>(this->FindChildByType(MEASURE));
    if (!measure) {
        LogError("No measure found for generating a scoreDef");
        return false;
    }

    ArrayOfObjects staves;
    AttComparison matchType(STAFF);
    measure->FindAllChildByAttComparison(&staves, &matchType);

    if (staves.empty()) {
        LogError("No staff found for generating a scoreDef");
        return false;
    }

    m_scoreDef.Reset();
    StaffGrp *staffGrp = new StaffGrp();
    ArrayOfObjects::iterator iter;
    for (iter = staves.begin(); iter != staves.end(); iter++) {
        Staff *staff = dynamic_cast<Staff *>(*iter);
        assert(staff);
        StaffDef *staffDef = new StaffDef();
        staffDef->SetN(staff->GetN());
        staffDef->SetLines(5);
        if (!measure->IsMeasuredMusic()) staffDef->SetNotationtype(NOTATIONTYPE_mensural);
        staffGrp->AddChild(staffDef);
    }
    m_scoreDef.AddChild(staffGrp);

    LogMessage("ScoreDef generated");

    return true;
}

bool Doc::GenerateHeaderAndFooter()
{
    if (m_scoreDef.FindChildByType(PGHEAD) || m_scoreDef.FindChildByType(PGFOOT)) {
        return false;
    }

    PgHead *pgHead = new PgHead();
    // We mark it as generated for not having it written in the output
    pgHead->IsGenerated(true);
    pgHead->GenerateFromMEIHeader(m_header);
    m_scoreDef.AddChild(pgHead);

    PgFoot *pgFoot = new PgFoot();
    pgFoot->IsGenerated(true);
    pgFoot->LoadFooter();
    m_scoreDef.AddChild(pgFoot);

    PgHead2 *pgHead2 = new PgHead2();
    pgHead2->IsGenerated(true);
    pgHead2->AddPageNum(HORIZONTALALIGNMENT_center, VERTICALALIGNMENT_top);
    m_scoreDef.AddChild(pgHead2);

    PgFoot2 *pgFoot2 = new PgFoot2();
    pgFoot2->IsGenerated(true);
    pgFoot2->LoadFooter();
    m_scoreDef.AddChild(pgFoot2);

    return true;
}

bool Doc::HasMidiTimemap()
{
    return m_hasMidiTimemap;
}

void Doc::CalculateMidiTimemap()
{
    m_hasMidiTimemap = false;

    // This happens if the document was never cast off (no-layout option in the toolkit)
    if (!m_drawingPage && GetPageCount() == 1) {
        Page *page = this->SetDrawingPage(0);
        if (!page) {
            return;
        }
        this->CollectScoreDefs();
        page->LayOutHorizontally();
    }

    int tempo = 120;

    // Set tempo
    if (m_scoreDef.HasMidiBpm()) {
        tempo = m_scoreDef.GetMidiBpm();
    }

    // We first calculate the maximum duration of each measure
    CalcMaxMeasureDurationParams calcMaxMeasureDurationParams;
    calcMaxMeasureDurationParams.m_currentTempo = tempo;
    Functor calcMaxMeasureDuration(&Object::CalcMaxMeasureDuration);
    this->Process(&calcMaxMeasureDuration, &calcMaxMeasureDurationParams);

    // Then calculate the onset and offset times (w.r.t. the measure) for every note
    CalcOnsetOffsetParams calcOnsetOffsetParams;
    Functor calcOnsetOffset(&Object::CalcOnsetOffset);
    Functor calcOnsetOffsetEnd(&Object::CalcOnsetOffsetEnd);
    this->Process(&calcOnsetOffset, &calcOnsetOffsetParams, &calcOnsetOffsetEnd);

    // Adjust the duration of tied notes
    Functor resolveMIDITies(&Object::ResolveMIDITies);
    this->Process(&resolveMIDITies, NULL, NULL, NULL, UNLIMITED_DEPTH, BACKWARD);

    m_hasMidiTimemap = true;
}

void Doc::ExportMIDI(MidiFile *midiFile)
{

    if (!Doc::HasMidiTimemap()) {
        // generate MIDI timemap before progressing
        CalculateMidiTimemap();
    }
    if (!Doc::HasMidiTimemap()) {
        LogWarning("Calculation of MIDI timemap failed, not exporting MidiFile.");
    }

    int tempo = 120;

    // Set tempo
    if (m_scoreDef.HasMidiBpm()) {
        tempo = m_scoreDef.GetMidiBpm();
    }
    midiFile->addTempo(0, 0, tempo);

    // We need to populate processing lists for processing the document by Layer (by Verse will not be used)
    PrepareProcessingListsParams prepareProcessingListsParams;
    // Alternate solution with StaffN_LayerN_VerseN_t (see also Verse::PrepareDrawing)
    // StaffN_LayerN_VerseN_t staffLayerVerseTree;
    // params.push_back(&staffLayerVerseTree);

    // We first fill a tree of int with [staff/layer] and [staff/layer/verse] numbers (@n) to be process
    Functor prepareProcessingLists(&Object::PrepareProcessingLists);
    this->Process(&prepareProcessingLists, &prepareProcessingListsParams);

    // The tree is used to process each staff/layer/verse separatly
    // For this, we use a array of AttNIntegerComparison that looks for each object if it is of the type
    // and with @n specified

    IntTree_t::iterator staves;
    IntTree_t::iterator layers;

    // Process notes and chords, rests, spaces layer by layer
    // track 0 (included by default) is reserved for meta messages common to all tracks
    int midiTrack = 1;
    std::vector<AttComparison *> filters;
    for (staves = prepareProcessingListsParams.m_layerTree.child.begin();
         staves != prepareProcessingListsParams.m_layerTree.child.end(); ++staves) {

        int transSemi = 0;
        // Get the transposition (semi-tone) value for the staff
        if (StaffDef *staffDef = this->m_scoreDef.GetStaffDef(staves->first)) {
            if (staffDef->HasTransSemi()) transSemi = staffDef->GetTransSemi();
            midiTrack = staffDef->GetN();
            midiFile->addTrack();
            Label *label = dynamic_cast<Label *>(staffDef->FindChildByType(LABEL, 1));
            if (!label) {
                StaffGrp *staffGrp = dynamic_cast<StaffGrp *>(staffDef->GetFirstParent(STAFFGRP));
                assert(staffGrp);
                label = dynamic_cast<Label *>(staffGrp->FindChildByType(LABEL, 1));
            }
            if (label) {
                std::string trackName = UTF16to8(label->GetText(label)).c_str();
                if (!trackName.empty()) midiFile->addTrackName(midiTrack, 0, trackName);
            }
        }

        for (layers = staves->second.child.begin(); layers != staves->second.child.end(); ++layers) {
            filters.clear();
            // Create ad comparison object for each type / @n
            AttNIntegerComparison matchStaff(STAFF, staves->first);
            AttNIntegerComparison matchLayer(LAYER, layers->first);
            filters.push_back(&matchStaff);
            filters.push_back(&matchLayer);

            GenerateMIDIParams generateMIDIParams(midiFile);
            generateMIDIParams.m_midiTrack = midiTrack;
            generateMIDIParams.m_transSemi = transSemi;
            generateMIDIParams.m_currentTempo = tempo;
            Functor generateMIDI(&Object::GenerateMIDI);

            // LogDebug("Exporting track %d ----------------", midiTrack);
            this->Process(&generateMIDI, &generateMIDIParams, NULL, &filters);
        }
    }
}

bool Doc::ExportTimemap(string &output)
{
    if (!Doc::HasMidiTimemap()) {
        // generate MIDI timemap before progressing
        CalculateMidiTimemap();
    }
    if (!Doc::HasMidiTimemap()) {
        LogWarning("Calculation of MIDI timemap failed, not exporting MidiFile.");
        output = "";
        return false;
    }
    GenerateTimemapParams generateTimemapParams;
    Functor generateTimemap(&Object::GenerateTimemap);
    this->Process(&generateTimemap, &generateTimemapParams);

    PrepareJsonTimemap(output, generateTimemapParams.realTimeToScoreTime, generateTimemapParams.realTimeToOnElements,
        generateTimemapParams.realTimeToOffElements, generateTimemapParams.realTimeToTempo);

    return true;
}

void Doc::PrepareJsonTimemap(std::string &output, std::map<int, double> &realTimeToScoreTime,
    std::map<int, vector<string> > &realTimeToOnElements, std::map<int, vector<string> > &realTimeToOffElements,
    std::map<int, int> &realTimeToTempo)
{

    int currentTempo = -1000;
    int newTempo;
    int mapsize = (int)realTimeToScoreTime.size();
    output = "";
    output.reserve(mapsize * 100); // Estimate 100 characters for each entry.
    output += "[\n";
    auto lastit = realTimeToScoreTime.end();
    lastit--;
    for (auto it = realTimeToScoreTime.begin(); it != realTimeToScoreTime.end(); it++) {
        output += "\t{\n";
        output += "\t\t\"tstamp\":\t";
        output += to_string(it->first);
        output += ",\n";
        output += "\t\t\"qstamp\":\t";
        output += to_string(it->second);

        auto ittempo = realTimeToTempo.find(it->first);
        if (ittempo != realTimeToTempo.end()) {
            newTempo = ittempo->second;
            if (newTempo != currentTempo) {
                currentTempo = newTempo;
                output += ",\n\t\t\"tempo\":\t";
                output += to_string(currentTempo);
            }
        }

        auto iton = realTimeToOnElements.find(it->first);
        if (iton != realTimeToOnElements.end()) {
            output += ",\n\t\t\"on\":\t[";
            for (int ion = 0; ion < (int)iton->second.size(); ion++) {
                output += "\"";
                output += iton->second[ion];
                output += "\"";
                if (ion < (int)iton->second.size() - 1) {
                    output += ", ";
                }
            }
            output += "]";
        }

        auto itoff = realTimeToOffElements.find(it->first);
        if (itoff != realTimeToOffElements.end()) {
            output += ",\n\t\t\"off\":\t[";
            for (int ioff = 0; ioff < (int)itoff->second.size(); ioff++) {
                output += "\"";
                output += itoff->second[ioff];
                output += "\"";
                if (ioff < (int)itoff->second.size() - 1) {
                    output += ", ";
                }
            }
            output += "]";
        }

        output += "\n\t}";
        if (it == lastit) {
            output += "\n";
        }
        else {
            output += ",\n";
        }
    }
    output += "]\n";
}

void Doc::PrepareDrawing()
{
    if (m_drawingPreparationDone) {
        Functor resetDrawing(&Object::ResetDrawing);
        this->Process(&resetDrawing, NULL);
    }

    /************ Resolve @starid / @endid ************/

    // Try to match all spanning elements (slur, tie, etc) by processing backwards
    PrepareTimeSpanningParams prepareTimeSpanningParams;
    Functor prepareTimeSpanning(&Object::PrepareTimeSpanning);
    Functor prepareTimeSpanningEnd(&Object::PrepareTimeSpanningEnd);
    this->Process(
        &prepareTimeSpanning, &prepareTimeSpanningParams, &prepareTimeSpanningEnd, NULL, UNLIMITED_DEPTH, BACKWARD);

    // First we try backwards because normally the spanning elements are at the end of
    // the measure. However, in some case, one (or both) end points will appear afterwards
    // in the encoding. For these, the previous iteration will not have resolved the link and
    // the spanning elements will remain in the timeSpanningElements array. We try again forwards
    // but this time without filling the list (that is only will the remaining elements)
    if (!prepareTimeSpanningParams.m_timeSpanningInterfaces.empty()) {
        prepareTimeSpanningParams.m_fillList = false;
        this->Process(&prepareTimeSpanning, &prepareTimeSpanningParams);
    }

    /************ Resolve @starid (only) ************/

    // Try to match all time pointing elements (tempo, fermata, etc) by processing backwards
    PrepareTimePointingParams prepareTimePointingParams;
    Functor prepareTimePointing(&Object::PrepareTimePointing);
    Functor prepareTimePointingEnd(&Object::PrepareTimePointingEnd);
    this->Process(
        &prepareTimePointing, &prepareTimePointingParams, &prepareTimePointingEnd, NULL, UNLIMITED_DEPTH, BACKWARD);

    /************ Resolve @tstamp / tstamp2 ************/

    // Now try to match the @tstamp and @tstamp2 attributes.
    PrepareTimestampsParams prepareTimestampsParams;
    prepareTimestampsParams.m_timeSpanningInterfaces = prepareTimeSpanningParams.m_timeSpanningInterfaces;
    Functor prepareTimestamps(&Object::PrepareTimestamps);
    Functor prepareTimestampsEnd(&Object::PrepareTimestampsEnd);
    this->Process(&prepareTimestamps, &prepareTimestampsParams, &prepareTimestampsEnd);

    // If some are still there, then it is probably an issue in the encoding
    if (!prepareTimestampsParams.m_timeSpanningInterfaces.empty()) {
        LogWarning("%d time spanning element(s) could not be matched",
            prepareTimestampsParams.m_timeSpanningInterfaces.size());
    }

    /************ Resolve @plist ************/

    // Try to match all pointing elements using @plist
    PreparePlistParams preparePlistParams;
    Functor preparePlist(&Object::PreparePlist);
    this->Process(&preparePlist, &preparePlistParams);

    // If we have some left process again backward.
    if (!preparePlistParams.m_interfaceUuidPairs.empty()) {
        preparePlistParams.m_fillList = false;
        this->Process(&preparePlist, &preparePlistParams, NULL, NULL, UNLIMITED_DEPTH, BACKWARD);
    }

    // If some are still there, then it is probably an issue in the encoding
    if (!preparePlistParams.m_interfaceUuidPairs.empty()) {
        LogWarning(
            "%d element(s) with a @plist could match the target", preparePlistParams.m_interfaceUuidPairs.size());
    }

    /************ Resolve cross staff ************/

    // Prepare the cross-staff pointers
    PrepareCrossStaffParams prepareCrossStaffParams;
    Functor prepareCrossStaff(&Object::PrepareCrossStaff);
    Functor prepareCrossStaffEnd(&Object::PrepareCrossStaffEnd);
    this->Process(&prepareCrossStaff, &prepareCrossStaffParams, &prepareCrossStaffEnd);

    /************ Prepare processing by staff/layer/verse ************/

    // We need to populate processing lists for processing the document by Layer (for matching @tie) and
    // by Verse (for matching syllable connectors)
    PrepareProcessingListsParams prepareProcessingListsParams;
    // Alternate solution with StaffN_LayerN_VerseN_t (see also Verse::PrepareDrawing)
    // StaffN_LayerN_VerseN_t staffLayerVerseTree;
    // params.push_back(&staffLayerVerseTree);

    // We first fill a tree of ints with [staff/layer] and [staff/layer/verse] numbers (@n) to be processed
    // LogElapsedTimeStart();
    Functor prepareProcessingLists(&Object::PrepareProcessingLists);
    this->Process(&prepareProcessingLists, &prepareProcessingListsParams);

    // The tree is used to process each staff/layer/verse separately
    // For this, we use an array of AttNIntegerComparison that looks for each object if it is of the type
    // and with @n specified

    IntTree_t::iterator staves;
    IntTree_t::iterator layers;
    IntTree_t::iterator verses;

    /************ Resolve some pointers by layer ************/

    std::vector<AttComparison *> filters;
    for (staves = prepareProcessingListsParams.m_layerTree.child.begin();
         staves != prepareProcessingListsParams.m_layerTree.child.end(); ++staves) {
        for (layers = staves->second.child.begin(); layers != staves->second.child.end(); ++layers) {
            filters.clear();
            // Create ad comparison object for each type / @n
            AttNIntegerComparison matchStaff(STAFF, staves->first);
            AttNIntegerComparison matchLayer(LAYER, layers->first);
            filters.push_back(&matchStaff);
            filters.push_back(&matchLayer);

            PreparePointersByLayerParams preparePointersByLayerParams;
            Functor preparePointersByLayer(&Object::PreparePointersByLayer);
            this->Process(&preparePointersByLayer, &preparePointersByLayerParams, NULL, &filters);
        }
    }

    /************ Resolve lyric connectors ************/

    // Same for the lyrics, but Verse by Verse since Syl are TimeSpanningInterface elements for handling connectors
    for (staves = prepareProcessingListsParams.m_verseTree.child.begin();
         staves != prepareProcessingListsParams.m_verseTree.child.end(); ++staves) {
        for (layers = staves->second.child.begin(); layers != staves->second.child.end(); ++layers) {
            for (verses = layers->second.child.begin(); verses != layers->second.child.end(); ++verses) {
                // std::cout << staves->first << " => " << layers->first << " => " << verses->first << '\n';
                filters.clear();
                // Create ad comparison object for each type / @n
                AttNIntegerComparison matchStaff(STAFF, staves->first);
                AttNIntegerComparison matchLayer(LAYER, layers->first);
                AttNIntegerComparison matchVerse(VERSE, verses->first);
                filters.push_back(&matchStaff);
                filters.push_back(&matchLayer);
                filters.push_back(&matchVerse);

                // The first pass sets m_drawingFirstNote and m_drawingLastNote for each syl
                // m_drawingLastNote is set only if the syl has a forward connector
                PrepareLyricsParams prepareLyricsParams;
                Functor prepareLyrics(&Object::PrepareLyrics);
                Functor prepareLyricsEnd(&Object::PrepareLyricsEnd);
                this->Process(&prepareLyrics, &prepareLyricsParams, &prepareLyricsEnd, &filters);
            }
        }
    }

    /************ Fill control event spanning ************/

    // Once <slur>, <ties> and @ties are matched but also syl connectors, we need to set them as running
    // TimeSpanningInterface to each staff they are extended. This does not need to be done staff by staff because we
    // can just check the staff->GetN to see where we are (see Staff::FillStaffCurrentTimeSpanning)
    FillStaffCurrentTimeSpanningParams fillStaffCurrentTimeSpanningParams;
    Functor fillStaffCurrentTimeSpanning(&Object::FillStaffCurrentTimeSpanning);
    Functor fillStaffCurrentTimeSpanningEnd(&Object::FillStaffCurrentTimeSpanningEnd);
    this->Process(&fillStaffCurrentTimeSpanning, &fillStaffCurrentTimeSpanningParams, &fillStaffCurrentTimeSpanningEnd);

    // Something must be wrong in the encoding because a TimeSpanningInterface was left open
    if (!fillStaffCurrentTimeSpanningParams.m_timeSpanningElements.empty()) {
        LogDebug("%d time spanning elements could not be set as running",
            fillStaffCurrentTimeSpanningParams.m_timeSpanningElements.size());
    }

    /************ Resolve mRpt ************/

    // Process by staff for matching mRpt elements and setting the drawing number
    for (staves = prepareProcessingListsParams.m_layerTree.child.begin();
         staves != prepareProcessingListsParams.m_layerTree.child.end(); ++staves) {
        for (layers = staves->second.child.begin(); layers != staves->second.child.end(); ++layers) {
            filters.clear();
            // Create ad comparison object for each type / @n
            AttNIntegerComparison matchStaff(STAFF, staves->first);
            AttNIntegerComparison matchLayer(LAYER, layers->first);
            filters.push_back(&matchStaff);
            filters.push_back(&matchLayer);

            // We set multiNumber to NONE for indicated we need to look at the staffDef when reaching the first staff
            PrepareRptParams prepareRptParams(&m_scoreDef);
            Functor prepareRpt(&Object::PrepareRpt);
            this->Process(&prepareRpt, &prepareRptParams, NULL, &filters);
        }
    }

    /************ Resolve endings ************/

    // Prepare the endings (pointers to the measure after and before the boundaries
    PrepareBoundariesParams prepareEndingsParams;
    Functor prepareEndings(&Object::PrepareBoundaries);
    this->Process(&prepareEndings, &prepareEndingsParams);

    /************ Resolve floating groups for vertical alignment ************/

    // Prepare the floating drawing groups
    PrepareFloatingGrpsParams prepareFloatingGrpsParams;
    Functor prepareFloatingGrps(&Object::PrepareFloatingGrps);
    this->Process(&prepareFloatingGrps, &prepareFloatingGrpsParams);

    /************ Resolve cue size ************/

    // Prepare the drawing cue size
    Functor prepareDrawingCueSize(&Object::PrepareDrawingCueSize);
    this->Process(&prepareDrawingCueSize, NULL);

    /************ Instanciate LayerElement parts (stemp, flag, dots, etc) ************/

    Functor prepareLayerElementParts(&Object::PrepareLayerElementParts);
    this->Process(&prepareLayerElementParts, NULL);

    /*
    // Alternate solution with StaffN_LayerN_VerseN_t
    StaffN_LayerN_VerseN_t::iterator staves;
    LayerN_VerserN_t::iterator layers;
    VerseN_t::iterator verses;
    std::vector<AttComparison*> filters;
    for (staves = staffLayerVerseTree.begin(); staves != staffLayerVerseTree.end(); ++staves) {
        for (layers = staves->second.begin(); layers != staves->second.end(); ++layers) {
            for (verses= layers->second.begin(); verses != layers->second.end(); ++verses) {
                std::cout << staves->first << " => " << layers->first << " => " << verses->first << '\n';
                filters.clear();
                AttNIntegerComparison matchStaff(&typeid(Staff), staves->first);
                AttNIntegerComparison matchLayer(&typeid(Layer), layers->first);
                AttNIntegerComparison matchVerse(&typeid(Verse), verses->first);
                filters.push_back(&matchStaff);
                filters.push_back(&matchLayer);
                filters.push_back(&matchVerse);

                FunctorParams paramsLyrics;
                Functor prepareLyrics(&Object::PrepareLyrics);
                this->Process(&prepareLyrics, paramsLyrics, NULL, &filters);
            }
        }
    }
    */

    // LogElapsedTimeEnd ("Preparing drawing");

    m_drawingPreparationDone = true;
}

void Doc::CollectScoreDefs(bool force)
{
    if (m_currentScoreDefDone && !force) {
        return;
    }

    if (m_currentScoreDefDone) {
        Functor unsetCurrentScoreDef(&Object::UnsetCurrentScoreDef);
        this->Process(&unsetCurrentScoreDef, NULL);
    }

    ScoreDef upcomingScoreDef = m_scoreDef;
    SetCurrentScoreDefParams setCurrentScoreDefParams(this, &upcomingScoreDef);
    Functor setCurrentScoreDef(&Object::SetCurrentScoreDef);

    // First process the current scoreDef in order to fill the staffDef with
    // the appropriate drawing values
    upcomingScoreDef.Process(&setCurrentScoreDef, &setCurrentScoreDefParams);

    // LogElapsedTimeStart();
    this->Process(&setCurrentScoreDef, &setCurrentScoreDefParams);
    // LogElapsedTimeEnd ("Setting scoreDefs");

    m_currentScoreDefDone = true;
}

void Doc::CastOffDoc()
{
    this->CollectScoreDefs();

    Pages *pages = this->GetPages();
    assert(pages);

    Page *contentPage = this->SetDrawingPage(0);
    assert(contentPage);
    contentPage->LayOutHorizontally();

    System *contentSystem = dynamic_cast<System *>(contentPage->DetachChild(0));
    assert(contentSystem);

    System *currentSystem = new System();
    contentPage->AddChild(currentSystem);
    CastOffSystemsParams castOffSystemsParams(contentSystem, contentPage, currentSystem);
    castOffSystemsParams.m_systemWidth = this->m_drawingPageWidth - this->m_drawingPageMarginLeft
        - this->m_drawingPageMarginRight - currentSystem->m_systemLeftMar - currentSystem->m_systemRightMar;
    castOffSystemsParams.m_shift = -contentSystem->GetDrawingLabelsWidth();
    castOffSystemsParams.m_currentScoreDefWidth
        = contentPage->m_drawingScoreDef.GetDrawingWidth() + contentSystem->GetDrawingAbbrLabelsWidth();

    Functor castOffSystems(&Object::CastOffSystems);
    Functor castOffSystemsEnd(&Object::CastOffSystemsEnd);
    contentSystem->Process(&castOffSystems, &castOffSystemsParams, &castOffSystemsEnd);
    delete contentSystem;

    // Reset the scoreDef at the beginning of each system
    this->CollectScoreDefs(true);

    // Here we redo the alignment because of the new scoreDefs
    // We can actually optimise this and have a custom version that does not redo all the calculation
    contentPage->LayOutVertically();

    // Detach the contentPage
    pages->DetachChild(0);
    assert(contentPage && !contentPage->GetParent());
    this->ResetDrawingPage();

    Page *currentPage = new Page();
    CastOffPagesParams castOffPagesParams(contentPage, this, currentPage);
    CastOffRunningElements(&castOffPagesParams);
    castOffPagesParams.m_pageHeight = this->m_drawingPageHeight - this->m_drawingPageMarginBot;
    Functor castOffPages(&Object::CastOffPages);
    pages->AddChild(currentPage);
    contentPage->Process(&castOffPages, &castOffPagesParams);
    delete contentPage;

    // LogDebug("Layout: %d pages", this->GetChildCount());

    // We need to reset the drawing page to NULL
    // because idx will still be 0 but contentPage is dead!
    this->CollectScoreDefs(true);
}

void Doc::CastOffRunningElements(CastOffPagesParams *params)
{
    Pages *pages = this->GetPages();
    assert(pages);
    assert(pages->GetChildCount() == 0);

    Page *page1 = new Page();
    pages->AddChild(page1);
    this->SetDrawingPage(0);
    page1->LayOutVertically();

    if (page1->GetHeader()) {
        params->m_pgHeadHeight = page1->GetHeader()->GetTotalHeight();
    }
    if (page1->GetFooter()) {
        params->m_pgFootHeight = page1->GetFooter()->GetTotalHeight();
    }

    Page *page2 = new Page();
    pages->AddChild(page2);
    this->SetDrawingPage(1);
    page2->LayOutVertically();

    if (page2->GetHeader()) {
        params->m_pgHead2Height = page2->GetHeader()->GetTotalHeight();
    }
    if (page2->GetFooter()) {
        params->m_pgFoot2Height = page2->GetFooter()->GetTotalHeight();
    }

    pages->DeleteChild(page1);
    pages->DeleteChild(page2);

    this->ResetDrawingPage();
}

void Doc::UnCastOffDoc()
{
    Pages *pages = this->GetPages();
    assert(pages);

    Page *contentPage = new Page();
    System *contentSystem = new System();
    contentPage->AddChild(contentSystem);

    UnCastOffParams unCastOffParams(contentSystem);

    Functor unCastOff(&Object::UnCastOff);
    this->Process(&unCastOff, &unCastOffParams);

    pages->ClearChildren();

    pages->AddChild(contentPage);

    // LogDebug("ContinousLayout: %d pages", this->GetChildCount());

    // We need to reset the drawing page to NULL
    // because idx will still be 0 but contentPage is dead!
    this->ResetDrawingPage();
    this->CollectScoreDefs(true);
}

void Doc::CastOffEncodingDoc()
{
    this->CollectScoreDefs();

    Pages *pages = this->GetPages();
    assert(pages);

    Page *contentPage = this->SetDrawingPage(0);
    assert(contentPage);

    contentPage->LayOutHorizontally();

    System *contentSystem = dynamic_cast<System *>(contentPage->FindChildByType(SYSTEM));
    assert(contentSystem);

    // Detach the contentPage
    pages->DetachChild(0);
    assert(contentPage && !contentPage->GetParent());

    Page *page = new Page();
    pages->AddChild(page);
    System *system = new System();
    page->AddChild(system);

    CastOffEncodingParams castOffEncodingParams(this, page, system, contentSystem);

    Functor castOffEncoding(&Object::CastOffEncoding);
    contentSystem->Process(&castOffEncoding, &castOffEncodingParams);
    delete contentPage;

    // We need to reset the drawing page to NULL
    // because idx will still be 0 but contentPage is dead!
    this->ResetDrawingPage();
    this->CollectScoreDefs(true);
}

void Doc::ConvertToPageBasedDoc()
{
    Score *score = this->GetScore();
    assert(score);

    Pages *pages = new Pages();
    pages->ConvertFrom(score);
    Page *page = new Page();
    pages->AddChild(page);
    System *system = new System();
    page->AddChild(system);

    ConvertToPageBasedParams convertToPageBasedParams(system);
    Functor convertToPageBased(&Object::ConvertToPageBased);
    Functor convertToPageBasedEnd(&Object::ConvertToPageBasedEnd);
    score->Process(&convertToPageBased, &convertToPageBasedParams, &convertToPageBasedEnd);

    score->ClearRelinquishedChildren();
    assert(score->GetChildCount() == 0);

    Mdiv *mdiv = dynamic_cast<Mdiv *>(score->GetParent());
    assert(mdiv);

    mdiv->ReplaceChild(score, pages);
    delete score;

    this->ResetDrawingPage();
}

void Doc::ConvertAnalyticalMarkupDoc(bool permanent)
{
    if (!m_hasAnalyticalMarkup) return;

    LogMessage("Converting analytical markup...");

    /************ Prepare processing by staff/layer/verse ************/

    // We need to populate processing lists for processing the document by Layer (for matching @tie) and
    // by Verse (for matching syllable connectors)
    PrepareProcessingListsParams prepareProcessingListsParams;

    // We first fill a tree of ints with [staff/layer] and [staff/layer/verse] numbers (@n) to be processed
    Functor prepareProcessingLists(&Object::PrepareProcessingLists);
    this->Process(&prepareProcessingLists, &prepareProcessingListsParams);

    IntTree_t::iterator staves;
    IntTree_t::iterator layers;

    /************ Resolve ties ************/

    // Process by layer for matching @tie attribute - we process notes and chords, looking at
    // GetTie values and pitch and oct for matching notes
    std::vector<AttComparison *> filters;
    for (staves = prepareProcessingListsParams.m_layerTree.child.begin();
         staves != prepareProcessingListsParams.m_layerTree.child.end(); ++staves) {
        for (layers = staves->second.child.begin(); layers != staves->second.child.end(); ++layers) {
            filters.clear();
            // Create ad comparison object for each type / @n
            AttNIntegerComparison matchStaff(STAFF, staves->first);
            AttNIntegerComparison matchLayer(LAYER, layers->first);
            filters.push_back(&matchStaff);
            filters.push_back(&matchLayer);

            ConvertAnalyticalMarkupParams convertAnalyticalMarkupParams(permanent);
            Functor convertAnalyticalMarkup(&Object::ConvertAnalyticalMarkup);
            Functor convertAnalyticalMarkupEnd(&Object::ConvertAnalyticalMarkupEnd);
            this->Process(
                &convertAnalyticalMarkup, &convertAnalyticalMarkupParams, &convertAnalyticalMarkupEnd, &filters);

            // After having processed one layer, we check if we have open ties - if yes, we
            // must reset them and they will be ignored.
            if (!convertAnalyticalMarkupParams.m_currentNotes.empty()) {
                std::vector<Note *>::iterator iter;
                for (iter = convertAnalyticalMarkupParams.m_currentNotes.begin();
                     iter != convertAnalyticalMarkupParams.m_currentNotes.end(); iter++) {
                    LogWarning("Unable to match @tie of note '%s', skipping it", (*iter)->GetUuid().c_str());
                }
            }
        }
    }
}

bool Doc::HasPage(int pageIdx)
{
    Pages *pages = this->GetPages();
    assert(pages);
    return ((pageIdx >= 0) && (pageIdx < pages->GetChildCount()));
}

Score *Doc::GetScore()
{
    return dynamic_cast<Score *>(this->FindChildByType(SCORE));
}

Pages *Doc::GetPages()
{
    return dynamic_cast<Pages *>(this->FindChildByType(PAGES));
}

int Doc::GetPageCount()
{
    Pages *pages = this->GetPages();
    assert(pages);
    return pages->GetChildCount();
}

int Doc::GetGlyphHeight(wchar_t code, int staffSize, bool graceSize) const
{
    int x, y, w, h;
    Glyph *glyph = Resources::GetGlyph(code);
    assert(glyph);
    glyph->GetBoundingBox(x, y, w, h);
    h = h * m_drawingSmuflFontSize / glyph->GetUnitsPerEm();
    if (graceSize) h = h * this->m_options->m_graceFactor.GetValue();
    h = h * staffSize / 100;
    return h;
}

int Doc::GetGlyphWidth(wchar_t code, int staffSize, bool graceSize) const
{
    int x, y, w, h;
    Glyph *glyph = Resources::GetGlyph(code);
    assert(glyph);
    glyph->GetBoundingBox(x, y, w, h);
    w = w * m_drawingSmuflFontSize / glyph->GetUnitsPerEm();
    if (graceSize) w = w * this->m_options->m_graceFactor.GetValue();
    w = w * staffSize / 100;
    return w;
}

int Doc::GetGlyphAdvX(wchar_t code, int staffSize, bool graceSize) const
{
    Glyph *glyph = Resources::GetGlyph(code);
    assert(glyph);
    int advX = glyph->GetHorizAdvX();
    advX = advX * m_drawingSmuflFontSize / glyph->GetUnitsPerEm();
    if (graceSize) advX = advX * this->m_options->m_graceFactor.GetValue();
    advX = advX * staffSize / 100;
    return advX;
}

Point Doc::ConvertFontPoint(const Glyph *glyph, const Point &fontPoint, int staffSize, bool graceSize) const
{
    assert(glyph);

    Point point;
    point.x = fontPoint.x * m_drawingSmuflFontSize / glyph->GetUnitsPerEm();
    point.y = fontPoint.y * m_drawingSmuflFontSize / glyph->GetUnitsPerEm();
    if (graceSize) {
        point.x = point.x * this->m_options->m_graceFactor.GetValue();
        point.y = point.y * this->m_options->m_graceFactor.GetValue();
    }
    if (staffSize != 100) {
        point.x = point.x * staffSize / 100;
        point.y = point.y * staffSize / 100;
    }
    return point;
}

int Doc::GetGlyphDescender(wchar_t code, int staffSize, bool graceSize) const
{
    int x, y, w, h;
    Glyph *glyph = Resources::GetGlyph(code);
    assert(glyph);
    glyph->GetBoundingBox(x, y, w, h);
    y = y * m_drawingSmuflFontSize / glyph->GetUnitsPerEm();
    if (graceSize) y = y * this->m_options->m_graceFactor.GetValue();
    y = y * staffSize / 100;
    return y;
}

int Doc::GetTextGlyphHeight(wchar_t code, FontInfo *font, bool graceSize) const
{
    assert(font);

    int x, y, w, h;
    Glyph *glyph = Resources::GetTextGlyph(code);
    assert(glyph);
    glyph->GetBoundingBox(x, y, w, h);
    h = h * font->GetPointSize() / glyph->GetUnitsPerEm();
    if (graceSize) h = h * this->m_options->m_graceFactor.GetValue();
    return h;
}

int Doc::GetTextGlyphWidth(wchar_t code, FontInfo *font, bool graceSize) const
{
    assert(font);

    int x, y, w, h;
    Glyph *glyph = Resources::GetTextGlyph(code);
    assert(glyph);
    glyph->GetBoundingBox(x, y, w, h);
    w = w * font->GetPointSize() / glyph->GetUnitsPerEm();
    if (graceSize) w = w * this->m_options->m_graceFactor.GetValue();
    return w;
}

int Doc::GetTextGlyphDescender(wchar_t code, FontInfo *font, bool graceSize) const
{
    assert(font);

    int x, y, w, h;
    Glyph *glyph = Resources::GetTextGlyph(code);
    assert(glyph);
    glyph->GetBoundingBox(x, y, w, h);
    y = y * font->GetPointSize() / glyph->GetUnitsPerEm();
    if (graceSize) y = y * this->m_options->m_graceFactor.GetValue();
    return y;
}

int Doc::GetDrawingUnit(int staffSize) const
{
    return m_options->m_unit.GetValue() * staffSize / 100;
}

int Doc::GetDrawingDoubleUnit(int staffSize) const
{
    return m_options->m_unit.GetValue() * 2 * staffSize / 100;
}

int Doc::GetDrawingStaffSize(int staffSize) const
{
    return m_options->m_unit.GetValue() * 8 * staffSize / 100;
}

int Doc::GetDrawingOctaveSize(int staffSize) const
{
    return m_options->m_unit.GetValue() * 7 * staffSize / 100;
}

int Doc::GetDrawingBrevisWidth(int staffSize) const
{
    return m_drawingBrevisWidth * staffSize / 100;
}

int Doc::GetDrawingBarLineWidth(int staffSize) const
{
    return m_options->m_barLineWidth.GetValue() * GetDrawingUnit(staffSize);
}

int Doc::GetDrawingStaffLineWidth(int staffSize) const
{
    return m_options->m_staffLineWidth.GetValue() * GetDrawingUnit(staffSize);
}

int Doc::GetDrawingStemWidth(int staffSize) const
{
    return m_options->m_stemWidth.GetValue() * GetDrawingUnit(staffSize);
}

int Doc::GetDrawingDynamHeight(int staffSize, bool withMargin) const
{
    int height = GetGlyphHeight(SMUFL_E522_dynamicForte, staffSize, false);
    // This should be styled
    if (withMargin) height += GetDrawingUnit(staffSize);
    return height;
}

int Doc::GetDrawingHairpinSize(int staffSize, bool withMargin) const
{
    int size = m_options->m_hairpinSize.GetValue() * GetDrawingUnit(staffSize);
    // This should be styled
    if (withMargin) size += GetDrawingUnit(staffSize);
    return size;
}

int Doc::GetDrawingBeamWidth(int staffSize, bool graceSize) const
{
    int value = m_drawingBeamWidth * staffSize / 100;
    if (graceSize) value = value * this->m_options->m_graceFactor.GetValue();
    return value;
}

int Doc::GetDrawingBeamWhiteWidth(int staffSize, bool graceSize) const
{
    int value = m_drawingBeamWhiteWidth * staffSize / 100;
    if (graceSize) value = value * this->m_options->m_graceFactor.GetValue();
    return value;
}

int Doc::GetDrawingLedgerLineLength(int staffSize, bool graceSize) const
{
    int value = m_drawingLedgerLine * staffSize / 100;
    if (graceSize) value = value * this->m_options->m_graceFactor.GetValue();
    return value;
}

int Doc::GetCueSize(int value) const
{
    return value * this->m_options->m_graceFactor.GetValue();
}

FontInfo *Doc::GetDrawingSmuflFont(int staffSize, bool graceSize)
{
    m_drawingSmuflFont.SetFaceName(m_options->m_font.GetValue().c_str());
    int value = m_drawingSmuflFontSize * staffSize / 100;
    if (graceSize) value = value * this->m_options->m_graceFactor.GetValue();
    m_drawingSmuflFont.SetPointSize(value);
    return &m_drawingSmuflFont;
}

FontInfo *Doc::GetDrawingLyricFont(int staffSize)
{
    m_drawingLyricFont.SetPointSize(m_drawingLyricFontSize * staffSize / 100);
    return &m_drawingLyricFont;
}

double Doc::GetLeftMargin(const ClassId classId) const
{
    if (classId == ACCID) return m_options->m_leftMarginAccid.GetValue();
    if (classId == BARLINE) return m_options->m_leftMarginBarLine.GetValue();
    if (classId == BARLINE_ATTR_LEFT) return m_options->m_leftMarginLeftBarLine.GetValue();
    if (classId == BARLINE_ATTR_RIGHT) return m_options->m_leftMarginRightBarLine.GetValue();
    if (classId == BEATRPT) return m_options->m_leftMarginBeatRpt.GetValue();
    if (classId == CHORD) return m_options->m_leftMarginChord.GetValue();
    if (classId == CLEF) return m_options->m_leftMarginClef.GetValue();
    if (classId == KEYSIG) return m_options->m_leftMarginKeySig.GetValue();
    if (classId == MENSUR) return m_options->m_leftMarginMensur.GetValue();
    if (classId == METERSIG) return m_options->m_leftMarginMeterSig.GetValue();
    if (classId == MREST) return m_options->m_leftMarginMRest.GetValue();
    if (classId == MRPT2) return m_options->m_leftMarginMRpt2.GetValue();
    if (classId == MULTIREST) return m_options->m_leftMarginMultiRest.GetValue();
    if (classId == MULTIRPT) return m_options->m_leftMarginMultiRpt.GetValue();
    if (classId == NOTE) return m_options->m_leftMarginNote.GetValue();
    if (classId == REST) return m_options->m_leftMarginRest.GetValue();
    return m_options->m_defaultLeftMargin.GetValue();
}

double Doc::GetRightMargin(const ClassId classId) const
{
    if (classId == ACCID) return m_options->m_rightMarginAccid.GetValue();
    if (classId == BARLINE) return m_options->m_rightMarginBarLine.GetValue();
    if (classId == BARLINE_ATTR_LEFT) return m_options->m_rightMarginLeftBarLine.GetValue();
    if (classId == BARLINE_ATTR_RIGHT) return m_options->m_rightMarginRightBarLine.GetValue();
    if (classId == BEATRPT) return m_options->m_rightMarginBeatRpt.GetValue();
    if (classId == CHORD) return m_options->m_rightMarginChord.GetValue();
    if (classId == CLEF) return m_options->m_rightMarginClef.GetValue();
    if (classId == KEYSIG) return m_options->m_rightMarginKeySig.GetValue();
    if (classId == MENSUR) return m_options->m_rightMarginMensur.GetValue();
    if (classId == METERSIG) return m_options->m_rightMarginMeterSig.GetValue();
    if (classId == MREST) return m_options->m_rightMarginMRest.GetValue();
    if (classId == MRPT2) return m_options->m_rightMarginMRpt2.GetValue();
    if (classId == MULTIREST) return m_options->m_rightMarginMultiRest.GetValue();
    if (classId == MULTIRPT) return m_options->m_rightMarginMultiRpt.GetValue();
    if (classId == NOTE) return m_options->m_rightMarginNote.GetValue();
    if (classId == REST) return m_options->m_rightMarginRest.GetValue();
    return m_options->m_defaultRightMargin.GetValue();
}

double Doc::GetBottomMargin(const ClassId classId) const
{
    return m_options->m_defaultBottomMargin.GetValue();
}

double Doc::GetTopMargin(const ClassId classId) const
{
    return m_options->m_defaultTopMargin.GetValue();
}

double Doc::GetLeftPosition() const
{
    return m_options->m_leftPosition.GetValue();
}

Page *Doc::SetDrawingPage(int pageIdx)
{
    // out of range
    if (!HasPage(pageIdx)) {
        return NULL;
    }
    // nothing to do
    if (m_drawingPage && m_drawingPage->GetIdx() == pageIdx) {
        return m_drawingPage;
    }
    Pages *pages = this->GetPages();
    assert(pages);
    m_drawingPage = dynamic_cast<Page *>(pages->GetChild(pageIdx));
    assert(m_drawingPage);

    int glyph_size;

    // we use the page members only if set (!= -1)
    if (m_drawingPage->m_pageHeight != -1) {
        m_drawingPageHeight = m_drawingPage->m_pageHeight;
        m_drawingPageWidth = m_drawingPage->m_pageWidth;
        m_drawingPageMarginBot = m_drawingPage->m_pageMarginBottom;
        m_drawingPageMarginLeft = m_drawingPage->m_pageMarginLeft;
        m_drawingPageMarginRight = m_drawingPage->m_pageMarginRight;
        m_drawingPageMarginTop = m_drawingPage->m_pageMarginTop;
    }
    else if (this->m_pageHeight != -1) {
        m_drawingPageHeight = this->m_pageHeight;
        m_drawingPageWidth = this->m_pageWidth;
        m_drawingPageMarginBot = this->m_pageMarginBottom;
        m_drawingPageMarginLeft = this->m_pageMarginLeft;
        m_drawingPageMarginRight = this->m_pageMarginRight;
        m_drawingPageMarginTop = this->m_pageMarginTop;
    }
    else {
        m_drawingPageHeight = m_options->m_pageHeight.GetValue();
        m_drawingPageWidth = m_options->m_pageWidth.GetValue();
        m_drawingPageMarginBot = m_options->m_pageMarginBottom.GetValue();
        m_drawingPageMarginLeft = m_options->m_pageMarginLeft.GetValue();
        m_drawingPageMarginRight = m_options->m_pageMarginRight.GetValue();
        m_drawingPageMarginTop = m_options->m_pageMarginTop.GetValue();
    }

    if (this->m_options->m_landscape.GetValue()) {
        int pageHeight = m_drawingPageWidth;
        m_drawingPageWidth = m_drawingPageHeight;
        m_drawingPageHeight = pageHeight;
        int pageMarginRight = m_drawingPageMarginLeft;
        m_drawingPageMarginLeft = m_drawingPageMarginRight;
        m_drawingPageMarginRight = pageMarginRight;
    }

    // From here we could check if values have changed
    // Since  m_options->m_interlDefin stays the same, it's useless to do it
    // every time for now.

    m_drawingBeamMaxSlope = this->m_options->m_beamMaxSlope.GetValue();
    m_drawingBeamMinSlope = this->m_options->m_beamMinSlope.GetValue();
    m_drawingBeamMaxSlope /= 100;
    m_drawingBeamMinSlope /= 100;

    // values for beams
    m_drawingBeamWidth = this->m_options->m_unit.GetValue();
    m_drawingBeamWhiteWidth = this->m_options->m_unit.GetValue() / 2;

    // values for fonts
    m_drawingSmuflFontSize = CalcMusicFontSize();
    m_drawingLyricFontSize = m_options->m_unit.GetValue() * m_options->m_lyricSize.GetValue();

    glyph_size = GetGlyphWidth(SMUFL_E0A3_noteheadHalf, 100, 0);
    m_drawingLedgerLine = glyph_size * 72 / 100;

    glyph_size = GetGlyphWidth(SMUFL_E0A2_noteheadWhole, 100, 0);

    m_drawingBrevisWidth = (int)((glyph_size * 0.8) / 2);

    return m_drawingPage;
}

int Doc::CalcMusicFontSize()
{
    return m_options->m_unit.GetValue() * 8;
}

int Doc::GetAdjustedDrawingPageHeight() const
{
    assert(m_drawingPage);

    if (this->GetType() == Transcription) return m_drawingPage->m_pageHeight / DEFINITION_FACTOR;

    int contentHeight = m_drawingPage->GetContentHeight();
    return (contentHeight + m_drawingPageMarginTop + m_drawingPageMarginBot) / DEFINITION_FACTOR;
}

int Doc::GetAdjustedDrawingPageWidth() const
{
    assert(m_drawingPage);

    if (this->GetType() == Transcription) return m_drawingPage->m_pageWidth / DEFINITION_FACTOR;

    int contentWidth = m_drawingPage->GetContentWidth();
    return (contentWidth + m_drawingPageMarginLeft + m_drawingPageMarginRight) / DEFINITION_FACTOR;
}

//----------------------------------------------------------------------------
// Doc functors methods
//----------------------------------------------------------------------------

int Doc::PrepareLyricsEnd(FunctorParams *functorParams)
{
    PrepareLyricsParams *params = dynamic_cast<PrepareLyricsParams *>(functorParams);
    assert(params);

    if ((params->m_currentSyl && params->m_lastNote) && (params->m_currentSyl->GetStart() != params->m_lastNote)) {
        params->m_currentSyl->SetEnd(params->m_lastNote);
    }

    return FUNCTOR_STOP;
}

} // namespace vrv
