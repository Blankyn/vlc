/*
 * SegmentTracker.cpp
 *****************************************************************************
 * Copyright (C) 2014 - VideoLAN and VLC authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "SegmentTracker.hpp"
#include "playlist/BasePlaylist.hpp"
#include "playlist/BaseRepresentation.h"
#include "playlist/BaseAdaptationSet.h"
#include "playlist/Segment.h"
#include "playlist/SegmentChunk.hpp"
#include "logic/AbstractAdaptationLogic.h"
#include "logic/BufferingLogic.hpp"

#include <cassert>
#include <limits>

using namespace adaptive;
using namespace adaptive::logic;
using namespace adaptive::playlist;

TrackerEvent::TrackerEvent(Type t)
{
    type = t;
}

TrackerEvent::~TrackerEvent()
{

}

TrackerEvent::Type TrackerEvent::getType() const
{
    return type;
}

DiscontinuityEvent::DiscontinuityEvent(uint64_t d)
    : TrackerEvent(Type::Discontinuity)
{
    discontinuitySequenceNumber = d;
}

SegmentGapEvent::SegmentGapEvent()
    : TrackerEvent(Type::SegmentGap)
{

}

RepresentationSwitchEvent::RepresentationSwitchEvent(BaseRepresentation *prev,
                                                     BaseRepresentation *next)
    : TrackerEvent(Type::RepresentationSwitch)
{
    this->prev = prev;
    this->next = next;
}

RepresentationUpdatedEvent::RepresentationUpdatedEvent(BaseRepresentation * rep)
    : TrackerEvent(Type::RepresentationUpdated)
{
    this->rep = rep;
}

RepresentationUpdateFailedEvent::RepresentationUpdateFailedEvent(BaseRepresentation * rep)
    : TrackerEvent(Type::RepresentationUpdateFailed)
{
    this->rep = rep;
}

FormatChangedEvent::FormatChangedEvent(const StreamFormat *f)
    : TrackerEvent(Type::FormatChange)
{
    this->format = f;
}

SegmentChangedEvent::SegmentChangedEvent(const ID &id, uint64_t sequence, mtime_t starttime,
                                         mtime_t duration, mtime_t displaytime)
    : TrackerEvent(Type::SegmentChange)
{
    this->id = &id;
    this->sequence = sequence;
    this->duration = duration;
    this->starttime = starttime;
    this->displaytime = displaytime;
}

BufferingStateUpdatedEvent::BufferingStateUpdatedEvent(const ID &id, bool enabled)
    : TrackerEvent(Type::BufferingStateUpdate)
{
    this->id = &id;
    this->enabled = enabled;
}

BufferingLevelChangedEvent::BufferingLevelChangedEvent(const ID &id,
                                                       mtime_t minimum, mtime_t maximum,
                                                       mtime_t current, mtime_t target)
    : TrackerEvent(Type::BufferingLevelChange)
{
    this->id = &id;
    this->minimum = minimum;
    this->maximum = maximum;
    this->current = current;
    this->target = target;
}

PositionChangedEvent::PositionChangedEvent(mtime_t r)
    : TrackerEvent(Type::PositionChange)
{
    resumeTime = r;
}

SegmentTracker::SegmentTracker(SharedResources *res,
        AbstractAdaptationLogic *logic_,
        const AbstractBufferingLogic *bl,
        BaseAdaptationSet *adaptSet,
        SynchronizationReferences *refs)
{
    resources = res;
    first = true;
    initializing = true;
    bufferingLogic = bl;
    setAdaptationLogic(logic_);
    adaptationSet = adaptSet;
    synchronizationReferences = refs;
    format = StreamFormat::Type::Unknown;
}

SegmentTracker::~SegmentTracker()
{
    reset();
}

SegmentTracker::Position::Position()
{
    number = std::numeric_limits<uint64_t>::max();
    rep = nullptr;
    init_sent = false;
    index_sent = false;
}

SegmentTracker::Position::Position(BaseRepresentation *rep, uint64_t number)
{
    this->rep = rep;
    this->number = number;
    init_sent = false;
    index_sent = false;
}

bool SegmentTracker::Position::isValid() const
{
    return number != std::numeric_limits<uint64_t>::max() &&
           rep != nullptr;
}

std::string SegmentTracker::Position::toString() const
{
    std::stringstream ss;
    ss.imbue(std::locale("C"));
    if(isValid())
        ss << "seg# " << number
           << " " << init_sent
           << ":" << index_sent
           << " " << rep->getID().str();
    else
        ss << "invalid";
    return ss.str();
}

SegmentTracker::Position & SegmentTracker::Position::operator ++()
{
    if(isValid())
    {
        if(index_sent)
            ++number;
        else if(init_sent)
            index_sent = true;
        else
            init_sent = true;
        return *this;
    }
    return *this;
}

void SegmentTracker::setAdaptationLogic(AbstractAdaptationLogic *logic_)
{
    logic = logic_;
    registerListener(logic);
}

StreamFormat SegmentTracker::getCurrentFormat() const
{
    BaseRepresentation *rep = current.rep;
    if(!rep)
        rep = logic->getNextRepresentation(adaptationSet, nullptr);
    if(rep)
    {
        /* Ensure ephemere content is updated/loaded */
        if(rep->needsUpdate(next.number))
            rep->scheduleNextUpdate(next.number, rep->runLocalUpdates(resources));
        return rep->getStreamFormat();
    }
    return StreamFormat();
}

void SegmentTracker::getCodecsDesc(CodecDescriptionList *descs) const
{
    BaseRepresentation *rep = current.rep;
    if(!rep)
        rep = logic->getNextRepresentation(adaptationSet, nullptr);
    if(rep)
        rep->getCodecsDesc(descs);
}

const Role & SegmentTracker::getStreamRole() const
{
    return adaptationSet->getRole();
}

void SegmentTracker::reset()
{
    notify(RepresentationSwitchEvent(current.rep, nullptr));
    current = Position();
    next = Position();
    resetChunksSequence();
    initializing = true;
    format = StreamFormat::Type::Unknown;
}

SegmentTracker::ChunkEntry::ChunkEntry()
{
    chunk = nullptr;
}

SegmentTracker::ChunkEntry::ChunkEntry(SegmentChunk *c, Position p, mtime_t s, mtime_t d, mtime_t dt)
{
    chunk = c;
    pos = p;
    duration = d;
    starttime = s;
    displaytime = dt;
}

bool SegmentTracker::ChunkEntry::isValid() const
{
    return chunk && pos.isValid();
}

SegmentTracker::ChunkEntry
SegmentTracker::prepareChunk(bool switch_allowed, Position pos,
                             AbstractConnectionManager *connManager) const
{
    if(!adaptationSet)
        return ChunkEntry();

    /* starting */
    if(!pos.isValid())
    {
        pos = getStartPosition();
        if(!pos.isValid())
            return ChunkEntry();
    }
    else /* continuing, or seek */
    {
        if(!adaptationSet->isSegmentAligned() || !pos.init_sent || !pos.index_sent)
            switch_allowed = false;

        if(switch_allowed)
        {
            Position temp;
            temp.rep = logic->getNextRepresentation(adaptationSet, pos.rep);
            if(temp.rep && temp.rep != pos.rep)
            {
                /* Convert our segment number if we need to */
                temp.number = temp.rep->translateSegmentNumber(pos.number, pos.rep);

                /* Ensure ephemere content is updated/loaded */
                if(temp.rep->needsUpdate(temp.number))
                    temp.rep->scheduleNextUpdate(temp.number, temp.rep->runLocalUpdates(resources));

                /* could have been std::numeric_limits<uint64_t>::max() if not found because not avail */
                if(!temp.isValid()) /* try again */
                    temp.number = temp.rep->translateSegmentNumber(pos.number, pos.rep);

                /* cancel switch that would go past playlist */
                if(temp.isValid() && temp.rep->getMinAheadTime(temp.number) == 0)
                    temp = Position();
            }
            if(temp.isValid())
                pos = temp;
        }
    }

    bool b_gap = true;
    ISegment *datasegment = pos.rep->getNextMediaSegment(pos.number, &pos.number, &b_gap);

    if(!datasegment)
        return ChunkEntry();

    ISegment *segment = nullptr;
    if(!pos.init_sent)
    {
        segment = pos.rep->getInitSegment();
        if(!segment)
            ++pos;
    }

    if(!segment && !pos.index_sent)
    {
        if(pos.rep->needsIndex())
            segment = pos.rep->getIndexSegment();
        if(!segment)
            ++pos;
    }

    if(!segment)
        segment = datasegment;

    SegmentChunk *segmentChunk = segment->toChunk(resources, connManager, pos.number, pos.rep);
    if(!segmentChunk)
        return ChunkEntry();

    mtime_t startTime = VLC_TS_INVALID;
    mtime_t duration = 0;
    mtime_t displayTime = datasegment->getDisplayTime();
    /* timings belong to timeline and are not set on the segment or need profile timescale */
    if(pos.rep->getPlaybackTimeDurationBySegmentNumber(pos.number, &startTime, &duration))
        startTime += VLC_TS_0;

    return ChunkEntry(segmentChunk, pos, startTime, duration, displayTime);
}

void SegmentTracker::resetChunksSequence()
{
    while(!chunkssequence.empty())
    {
        delete chunkssequence.front().chunk;
        chunkssequence.pop_front();
    }
}

ChunkInterface * SegmentTracker::getNextChunk(bool switch_allowed,
                                            AbstractConnectionManager *connManager)
{
    if(!adaptationSet || !next.isValid())
        return nullptr;

    if(chunkssequence.empty())
    {
        ChunkEntry chunk = prepareChunk(switch_allowed, next, connManager);
        chunkssequence.push_back(chunk);
    }

    ChunkEntry chunk = chunkssequence.front();
    if(!chunk.isValid())
    {
        chunkssequence.pop_front();
        delete chunk.chunk;
        return nullptr;
    }

    /* here next == wanted chunk pos */
    bool b_gap = (next.number != chunk.pos.number);
    const bool b_switched = (next.rep != chunk.pos.rep) || !current.rep;
    bool b_discontinuity = chunk.chunk->discontinuity && current.isValid();
    if(b_discontinuity && current.number == next.number)
    {
        /* if we are on the same segment and indexes have been sent, then discontinuity was */
        b_discontinuity = false;
    }
    const uint64_t discontinuitySequenceNumber = chunk.chunk->discontinuitySequenceNumber;

    if(b_switched)
    {
        notify(RepresentationSwitchEvent(next.rep, chunk.pos.rep));
        initializing = true;
    }

    /* advance or don't trigger duplicate events */
    next = current = chunk.pos;

    if(format == StreamFormat(StreamFormat::Type::Unsupported))
        return nullptr; /* Can't return chunk because no demux will be created */

    /* From this point chunk must be returned */
    ChunkInterface *returnedChunk;
    StreamFormat chunkformat = chunk.chunk->getStreamFormat();

    /* Wrap and probe format */
    if(chunkformat == StreamFormat(StreamFormat::Type::Unknown))
    {
        ProbeableChunk *wrappedck = new ProbeableChunk(chunk.chunk);
        const uint8_t *p_peek;
        size_t i_peek = wrappedck->peek(&p_peek);
        chunkformat = StreamFormat(p_peek, i_peek);
        /* fallback on Mime type */
        if(chunkformat == StreamFormat(StreamFormat::Type::Unknown))
            format = StreamFormat(chunk.chunk->getContentType());
        chunk.chunk->setStreamFormat(chunkformat);
        returnedChunk = wrappedck;
    }
    else returnedChunk = chunk.chunk;

    if(chunkformat != format &&
       chunkformat != StreamFormat(StreamFormat::Type::Unknown))
    {
        format = chunkformat;
        notify(FormatChangedEvent(&format));
    }

    /* pop position and return our chunk */
    chunkssequence.pop_front();
    chunk.chunk = nullptr;

    if(initializing)
    {
        b_gap = false;
        /* stop initializing after 1st chunk */
        initializing = false;
    }

    if(b_gap)
        notify(SegmentGapEvent());

    /* Handle both implicit and explicit discontinuities */
    if(b_discontinuity)
        notify(DiscontinuityEvent(discontinuitySequenceNumber));

    /* Notify new segment length for stats / logic */
    notify(SegmentChangedEvent(adaptationSet->getID(),
                               discontinuitySequenceNumber,
                               chunk.starttime, chunk.duration, chunk.displaytime));

    if(!b_gap)
        ++next;

    return returnedChunk;
}

bool SegmentTracker::setPositionByTime(mtime_t time, bool restarted, bool tryonly)
{
    Position pos = Position(current.rep, current.number);
    if(!pos.isValid())
        pos.rep = logic->getNextRepresentation(adaptationSet, nullptr);

    if(!pos.rep)
        return false;

    /* Stream might not have been loaded at all (HLS) or expired */
    if(pos.rep->needsUpdate(pos.number))
    {
        if(!pos.rep->runLocalUpdates(resources))
        {
            msg_Err(adaptationSet->getPlaylist()->getVLCObject(),
                    "Failed to update Representation %s",
                    pos.rep->getID().str().c_str());
            return false;
        }
        pos.rep->scheduleNextUpdate(pos.number, true);
        notify(RepresentationUpdatedEvent(pos.rep));
    }

    if(pos.rep->getSegmentNumberByTime(time, &pos.number))
    {
        if(!tryonly)
            setPosition(pos, restarted);
        return true;
    }
    return false;
}

void SegmentTracker::setPosition(const Position &pos, bool restarted)
{
    if(restarted)
        initializing = true;
    current = Position();
    next = pos;
    resetChunksSequence();
    notify(PositionChangedEvent(getPlaybackTime(true)));
}

SegmentTracker::Position SegmentTracker::getStartPosition() const
{
    Position pos;
    pos.rep = logic->getNextRepresentation(adaptationSet, nullptr);
    if(pos.rep)
    {
        /* Ensure ephemere content is updated/loaded */
        bool b_updated = pos.rep->needsUpdate(pos.number) && pos.rep->runLocalUpdates(resources);
        pos.number = bufferingLogic->getStartSegmentNumber(pos.rep);
        pos.rep->scheduleNextUpdate(pos.number, b_updated);
        if(b_updated)
            notify(RepresentationUpdatedEvent(pos.rep));
    }
    return pos;
}

bool SegmentTracker::setStartPosition()
{
    if(next.isValid())
        return true;

    Position pos = getStartPosition();
    if(!pos.isValid())
        return false;

    next = pos;
    return true;
}

mtime_t SegmentTracker::getPlaybackTime(bool b_next) const
{
    mtime_t time, duration;

    BaseRepresentation *rep = current.rep;
    if(!rep)
        rep = logic->getNextRepresentation(adaptationSet, nullptr);

    if(rep &&
       rep->getPlaybackTimeDurationBySegmentNumber(b_next ? next.number : current.number, &time, &duration))
    {
        return time;
    }
    return 0;
}

bool SegmentTracker::getMediaPlaybackRange(mtime_t *start, mtime_t *end,
                                           mtime_t *length) const
{
    if(!current.rep)
        return false;
    return current.rep->getMediaPlaybackRange(start, end, length);
}

mtime_t SegmentTracker::getMinAheadTime() const
{
    BaseRepresentation *rep = current.rep;
    if(!rep)
        rep = logic->getNextRepresentation(adaptationSet, nullptr);
    if(rep)
    {
        /* Ensure ephemere content is updated/loaded */
        if(rep->needsUpdate(next.number))
        {
            bool b_updated = rep->runLocalUpdates(resources);
            rep->scheduleNextUpdate(next.number, b_updated);
            if(b_updated)
                notify(RepresentationUpdatedEvent(rep));
        }
        uint64_t startnumber = current.number;
        if(startnumber == std::numeric_limits<uint64_t>::max())
            startnumber = bufferingLogic->getStartSegmentNumber(rep);
        if(startnumber != std::numeric_limits<uint64_t>::max())
            return rep->getMinAheadTime(startnumber);
    }
    return 0;
}

bool SegmentTracker::getSynchronizationReference(uint64_t discontinuitysequence,
                                                 mtime_t time,
                                                 SynchronizationReference &r) const
{
    return synchronizationReferences->getReference(discontinuitysequence, time, r);
}

void SegmentTracker::updateSynchronizationReference(uint64_t discontinuitysequence,
                                                    const Times &t)
{
    synchronizationReferences->addReference(discontinuitysequence, t);
}

void SegmentTracker::notifyBufferingState(bool enabled) const
{
    notify(BufferingStateUpdatedEvent(adaptationSet->getID(), enabled));
}

void SegmentTracker::notifyBufferingLevel(mtime_t min, mtime_t max,
                                          mtime_t current, mtime_t target) const
{
    notify(BufferingLevelChangedEvent(adaptationSet->getID(), min, max, current, target));
}

void SegmentTracker::registerListener(SegmentTrackerListenerInterface *listener)
{
    listeners.push_back(listener);
}

bool SegmentTracker::bufferingAvailable() const
{
    if(adaptationSet->getPlaylist()->isLive())
        return getMinAheadTime() > 0;
    return true;
}

void SegmentTracker::updateSelected()
{
    if(current.rep && current.rep->needsUpdate(next.number))
    {
        bool b_updated = current.rep->runLocalUpdates(resources);
        current.rep->scheduleNextUpdate(current.number, b_updated);
        if(b_updated)
            notify(RepresentationUpdatedEvent(current.rep));
    }

    if(current.rep && current.rep->canNoLongerUpdate())
        notify(RepresentationUpdateFailedEvent(current.rep));
}

void SegmentTracker::notify(const TrackerEvent &event) const
{
    std::list<SegmentTrackerListenerInterface *>::const_iterator it;
    for(it=listeners.begin();it != listeners.end(); ++it)
        (*it)->trackerEvent(event);
}
