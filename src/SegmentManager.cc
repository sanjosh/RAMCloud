/* Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Common.h"
#include "LogDigest.h"
#include "LogMetadata.h"
#include "ShortMacros.h"
#include "SegmentManager.h"
#include "ServerRpcPool.h"

namespace RAMCloud {

/**
 * Construct a new segment manager.
 *
 * \param context
 *      The RAMCloud context this will run under.
 * \param logId
 *      Identifier of the log this object will manage. Used to stamp segments
 *      so they can be later identified on backups.
 * \param allocator
 *      Allocator to use to construct new log segments.
 * \param replicaManager
 *      The replica manager that will handle replication of segments this class
 *      allocates.
 * \param diskExpansionFactor
 *      Multiplication factor that determines how much extra space (if any) will
 *      be allocated on backups. This is a real number greater than of equal to
 *      1.0. For example, a factor of 2 would mean that up to twice the amount
 *      of storage in the master's DRAM will be allocated on backup disks. In
 *      conjunction with in-memory cleaning, this drives down cleaning costs.
 *      Factors larger than 2 or 3 will likely have quickly diminishing returns.
 */
SegmentManager::SegmentManager(Context& context,
                               ServerId& logId,
                               Allocator& allocator,
                               ReplicaManager& replicaManager,
                               double diskExpansionFactor)
    : context(context),
      logId(logId),
      allocator(allocator),
      replicaManager(replicaManager),
      maxSegments(static_cast<uint32_t>(static_cast<double>(
        allocator.getFreeSegmentCount()) * diskExpansionFactor)),
      numEmergencyHeads(2),
      numEmergencyHeadsAlloced(0),
      numSurvivorSegments(0),
      numSurvivorSegmentsAlloced(0),
      segments(NULL),
      states(NULL),
      freeSlots(),
      nextSegmentId(0),
      idToSlotMap(),
      allSegments(),
      segmentsByState(),
      lock(),
      logIteratorCount(0)
{
    if (diskExpansionFactor < 1.0)
        throw SegmentManagerException(HERE, "diskExpansionFactor not >= 1.0");

    if (allocator.getFreeSegmentCount() < numEmergencyHeads)
        throw SegmentManagerException(HERE, "must have at least two segments");

    assert(maxSegments >= allocator.getFreeSegmentCount());

    segments = new Tub<LogSegment>[maxSegments];
    states = new Tub<State>[maxSegments];

    for (uint32_t i = 0; i < maxSegments; i++)
        freeSlots.push_back(i);

    // TODO(anyone): Get this hack out of here.
    context.transportManager->registerMemory(
        const_cast<void*>(allocator.getBaseAddress()),
        allocator.getTotalBytes());
}

/**
 * Destroy the segment manager, freeing all segments that were allocated. Once
 * called, it is no longer safe to deference pointers to any segments that were
 * allocated by this class.
 */
SegmentManager::~SegmentManager()
{
    for (size_t i = 0; i < maxSegments; i++) {
        if (segments[i]) {
            removeFromLists(*segments[i]);
            segments[i].destroy();
        }
    }
    delete[] segments;
    delete[] states;
}

/**
 * Allocate a new segment that will serve as the head of the log. The segment
 * manager will handle the transition between the previous and next log head,
 * as well as write a segment header and the log digest. These entries will also
 * be synced to backups before the function returns. In short, the caller need
 * not do anything special.
 *
 * \param mustNotFail
 *      If true, the allocation of a new head must not fail. By specifying this,
 *      the caller may be returned an immutable head segment if memory has been
 *      exhausted.
 *
 * \return
 *      NULL if out of memory, otherwise the new head segment. If NULL is
 *      returned, then no transition took place and the previous head remains
 *      as the head of the log.
 */
LogSegment*
SegmentManager::allocHead(bool mustNotFail)
{
    Lock guard(lock);

    LogSegment* prevHead = getHeadSegment();
    LogSegment* newHead = alloc(ALLOC_HEAD);
    if (newHead == NULL) {
        /// Even if out of memory, we may need to allocate an emergency head
        /// segment to deal with replica failure or to let cleaning free
        /// resources.
        if (mustNotFail ||
          segmentsByState[FREEABLE_PENDING_DIGEST_AND_REFERENCES].size() > 0) {
            newHead = alloc(ALLOC_EMERGENCY_HEAD);
        } else {
            return NULL;
        }
    }

    writeHeader(newHead, Segment::INVALID_SEGMENT_ID);
    if (prevHead != NULL && !prevHead->isEmergencyHead)
        writeDigest(newHead, prevHead);
    else
        writeDigest(newHead, NULL);
    //writeMaximumVersionNumber(newHead);
    //writeWill(newHead);

    // Make the head immutable if it's an emergency head. This will prevent the
    // log from adding anything to it and let us reclaim it without cleaning
    // when the next head is allocated.
    if (newHead->isEmergencyHead)
        newHead->disableAppends();

    ReplicatedSegment* prevReplicatedSegment = NULL;
    if (prevHead != NULL)
        prevReplicatedSegment = prevHead->replicatedSegment;

    // Allocate a new ReplicatedSegment to handle backing up the new head. This
    // call will also sync the initial data (header, digest, etc) to the needed
    // number of replicas before returning.
    newHead->replicatedSegment = replicaManager.allocateHead(
        newHead->id, newHead, prevReplicatedSegment);

    // Close the old head after we've opened up the new head. This ensures that
    // we always have an open segment on backups, unless of course there was a
    // coordinated failure, in which case we can unambiguously detect it.
    if (prevHead != NULL) {
        // An exception here would be problematic.
        prevHead->replicatedSegment->close();
        Segment::OpaqueFooterEntry unused;  // TODO(ryan): Should close be sync?
        prevHead->replicatedSegment->sync(prevHead->getAppendedLength(unused));

        if (prevHead->isEmergencyHead)
            free(prevHead);
        else
            changeState(*prevHead, NEWLY_CLEANABLE);
    }

    return newHead;
}

/**
 * Allocate a new segment for the cleaner to write survivor data into.
 *
 * \param headSegmentIdDuringCleaning
 *      Identifier of the head segment when the current cleaning pass began. Any
 *      data written into this survivor segment must have pre-existed this head
 *      segment.
 *
 * \return
 *      NULL if out of memory, otherwise a pointer to the segment.
 */
LogSegment*
SegmentManager::allocSurvivor(uint64_t headSegmentIdDuringCleaning)
{
    Lock guard(lock);

    LogSegment* s = alloc(ALLOC_SURVIVOR);
    if (s == NULL)
        return NULL;

    writeHeader(s, headSegmentIdDuringCleaning);

    s->replicatedSegment = replicaManager.allocateNonHead(s->id, s);

    return s;
}

/**
 * Notify the segment manager when cleaning has completed. A list of cleaned
 * segments is pass in and the segment manager assumes that all survivor
 * segments allocated since the last call to this method will be added to the
 * log.
 *
 * \param clean
 *      List of segments that were cleaned and whose live objects, if any, have
 *      already been relocated to survivor segments.
 */
void
SegmentManager::cleaningComplete(LogSegmentVector& clean)
{
    Lock guard(lock);

    uint32_t segletsUsed = 0;
    uint32_t segletsFreed = 0;

    // New Segments we've added during cleaning need to wait
    // until the next head is written before they become part
    // of the Log.
    SegmentList& cleaningInto = segmentsByState[CLEANING_INTO];
    while (!cleaningInto.empty()) {
        LogSegment& s = cleaningInto.front();
        segletsUsed += s.getSegletsAllocated();
        changeState(s, CLEANABLE_PENDING_DIGEST);
    }

    // Increment the current epoch and save the last epoch any
    // RPC could have been a part of so we can store it with
    // the Segments to be freed.
    uint64_t epoch = ServerRpcPool<>::incrementCurrentEpoch() - 1;

    // Cleaned Segments must wait until the next digest has been
    // written (that is, the new Segments we cleaned into are part
    // of the Log and the cleaned ones are not) and no outstanding
    // RPCs could reference the data in those Segments before they
    // may be cleaned.
    foreach (LogSegment* s, clean) {
        segletsFreed += s->getSegletsAllocated();
        s->cleanedEpoch = epoch;
        changeState(*s, FREEABLE_PENDING_DIGEST_AND_REFERENCES);
    }

    LOG(NOTICE, "Cleaning used %u seglets to free %u seglets",
        segletsUsed, segletsFreed);
    assert(segletsUsed <= segletsFreed);
}

/**
 * Obtain a list of new segments that may now be cleaned. This is used by the
 * cleaner to learn of closed segments recently added to the log. Each call will
 * return the list of segments added since the previous call.
 *
 * \param[out] out
 *      List to append the newly cleanable segments to.
 */
void
SegmentManager::cleanableSegments(LogSegmentVector& out)
{
    Lock guard(lock);

    SegmentList& newlyCleanable = segmentsByState[NEWLY_CLEANABLE];
    while (!newlyCleanable.empty()) {
        LogSegment& s = newlyCleanable.front();
        out.push_back(&s);
        changeState(s, CLEANABLE);
    }
}

/**
 * Called whenever a LogIterator is created. The point is that the segment
 * manager keeps track of when iterators exist and ensures that cleaning does
 * not permute the log until after all iteration has completed.
 */
void
SegmentManager::logIteratorCreated()
{
    Lock guard(lock);
    logIteratorCount++;
}

/**
 * Called whenever a LogIterator is destroyed.
 */
void
SegmentManager::logIteratorDestroyed()
{
    Lock guard(lock);
    logIteratorCount--;
}

/**
 * Get a list of active segments (segments that are currently part of the log)
 * that have an identifier greater than or equal to the given value. This is
 * used by the log iterator to discover segments to iterate over.
 *
 * Note that segments are not returned in any particular order.
 *
 * \param minSegmentId
 *      Return segments with identifiers greater than or equal to this value.
 *      This allows the caller to avoid revisiting a segment it learned about
 *      previously.
 * \param[out] outList
 *      Appropriate segments, if any, are returned here.
 */
void
SegmentManager::getActiveSegments(uint64_t minSegmentId,
                                  LogSegmentVector& outList)
{
    if (logIteratorCount == 0)
        throw SegmentManagerException(HERE, "cannot call outside of iteration");

    // Walk the lists to collect the closed Segments. Since the cleaner
    // is locked out of inserting survivor segments and freeing cleaned
    // ones so long as any iterators exist, we need only consider what
    // is presently part of the log.
    State activeSegmentStates[] = {
        NEWLY_CLEANABLE,
        CLEANABLE,
        FREEABLE_PENDING_DIGEST_AND_REFERENCES
    };

    for (size_t i = 0; i < arrayLength(activeSegmentStates); i++) {
        foreach (LogSegment &s, segmentsByState[activeSegmentStates[i]]) {
            if (s.id >= minSegmentId)
                outList.push_back(&s);
        }
    }

    if (getHeadSegment() != NULL && getHeadSegment()->id >= minSegmentId)
        outList.push_back(getHeadSegment());
}

/**
 * Called by the cleaner, typically once at construction time, to specify how
 * many segments to reserve for it for cleaning. These segments will not be
 * allocated for heads of the log. The SegmentManager will ensure that if the
 * reserve is not full, any freed segments are returned to the reserve, rather
 * than used as log heads.
 *
 * As the name implies, this method may only be called to increase the number
 * of segments reserved. Specifying fewer segments will not shrink the current
 * reserve.
 *
 * \param numSegments
 *      The total number of full segments to reserve for the cleaner.
 * \return
 *      True if the number of requested segments were reserved, false if the
 *      request was too large and the reserve was left at its prior value.
 */
bool
SegmentManager::increaseSurvivorReserve(uint32_t numSegments)
{
    Lock guard(lock);

    // Avoid having to deal with shrinking the reserve, which could cause a
    // temporary situation where we have more allocated than reserved, which
    // could underflow calculating how many reserves have yet to be allocated.
    if (numSegments < numSurvivorSegments)
        return false;

    if (numSegments > (allocator.getFreeSegmentCount() - numEmergencyHeads))
        return false;

    numSurvivorSegments = numSegments;
    return true;
}

/**
 * Given a slot number (an index into the segment manager's table of segments),
 * return a reference to the associated segment. This is used by clients of the
 * log to gain access to data that was previously appended. See Log::getEntry,
 * which is the primary user of this method.
 *
 * \param slot
 *      Index into the array of segments specifying which segment to return.
 */
LogSegment&
SegmentManager::operator[](uint32_t slot)
{
    // There's no need to lock here. The 'segments' array is static and
    // we assume that callers are well-behaved and won't use an old
    // slot number. Even if we did take a lock, it still wouldn't prevent
    // them from doing what I just said.
    if (slot >= maxSegments || !segments[slot])
        throw SegmentManagerException(HERE, "invalid segment slot");
    return *segments[slot];
}

/**
 * Check if a segment with a particular id exists in the system. This is mainly
 * used in conjunction with tombstones to determine when its safe to garbage
 * collect them.
 *
 * \return
 *      True if the segment exists, otherwise false.
 */
bool
SegmentManager::doesIdExist(uint64_t id)
{
    Lock guard(lock);
    return contains(idToSlotMap, id);
}

/**
 * Return the total number of allocated segments in the system.
 */
uint32_t
SegmentManager::getAllocatedSegmentCount()
{
    Lock guard(lock);
    return downCast<uint32_t>(allSegments.size());
}

/**
 * Return the total number of free segments in the system.
 */
uint32_t
SegmentManager::getFreeSegmentCount()
{
    Lock guard(lock);
    return allocator.getFreeSegmentCount();
}

/**
 * Return the number of free survivor segments left in the reserve. The caller
 * may subsequently call allocSurvivor() this many times with a guarantee that
 * a survivor segment will be returned.
 */
uint32_t
SegmentManager::getFreeSurvivorCount()
{
    Lock guard(lock);
    return downCast<uint32_t>(numSurvivorSegments - numSurvivorSegmentsAlloced);
}

/**
 * Return the maximum number of segments that may ever exist in the system at
 * one time.
 */
uint32_t
SegmentManager::getMaximumSegmentCount()
{
    return maxSegments;
}

/**
 * Return the size of each seglet in bytes. Segments are composed of multiple
 * seglets.
 */
uint32_t
SegmentManager::getSegletSize()
{
    return allocator.getSegletSize();
}

/**
 * Return the size of each full segment in bytes.
 */
uint32_t
SegmentManager::getSegmentSize()
{
    return allocator.getSegmentSize();
}

/******************************************************************************
 * PRIVATE METHODS
 ******************************************************************************/

/**
 * Write the SegmentHeader to a segment that's presumably the new head of the
 * log. This method is only really useful in allocHead() and allocSurvivor().
 *
 * \param segment
 *      Pointer to the segment the header should be written to. 
 * \param headSegmentIdDuringCleaning
 *      If the segment is a cleaner-generated survivor segment, this value is
 *      the identifier of the head segment when cleaning started. Stamping the
 *      segment with this value lets us logically order cleaner segments and
 *      previous head segments.  If the segment is a head segment, however, this
 *      value should be Segment::INVALID_SEGMENT_ID.
 */
void
SegmentManager::writeHeader(LogSegment* segment,
                            uint64_t headSegmentIdDuringCleaning)
{
    SegmentHeader header(*logId,
                         segment->id,
                         allocator.getSegmentSize(),
                         headSegmentIdDuringCleaning);
    bool success = segment->append(LOG_ENTRY_TYPE_SEGHEADER,
                                   &header, sizeof(header));
    if (!success)
        throw FatalError(HERE, "Could not append segment header");
}

/**
 * Write the LogDigest to the new head of the log. This method should only be
 * called by allocHead(), since it will modify segment states in a way that
 * is not idempotent.
 *
 * \param newHead
 *      Pointer to the segment the digest should be written into. Generally
 *      this is the new head segment.
 * \param prevHead
 *      Pointer to the segment that was, or will soon be, the previous head of
 *      the log. If NULL, it will not be included. This parameter exists so
 *      that the caller may exclude the previous head if it happened to be
 *      an emergency head segment that will be immediately reclaimed.
 */
void
SegmentManager::writeDigest(LogSegment* newHead, LogSegment* prevHead)
{
    LogDigest digest;

    // Only include new survivor segments if no log iteration in progress.
    if (logIteratorCount == 0) {
        while (!segmentsByState[CLEANABLE_PENDING_DIGEST].empty()) {
            LogSegment& s = segmentsByState[CLEANABLE_PENDING_DIGEST].front();
            changeState(s, NEWLY_CLEANABLE);
        }
    }

    foreach (LogSegment& s, segmentsByState[CLEANABLE])
        digest.addSegmentId(s.id);

    foreach (LogSegment& s, segmentsByState[NEWLY_CLEANABLE])
        digest.addSegmentId(s.id);

    if (prevHead != NULL)
        digest.addSegmentId(prevHead->id);

    digest.addSegmentId(newHead->id);

    // Only preclude/free cleaned segments if no log iteration in progress.
    if (logIteratorCount == 0) {
        SegmentList& list = segmentsByState[
            FREEABLE_PENDING_DIGEST_AND_REFERENCES];
        while (!list.empty()) {
            LogSegment& s = list.front();
            changeState(s, FREEABLE_PENDING_REFERENCES);
        }
    } else {
        SegmentList& list = segmentsByState[
            FREEABLE_PENDING_DIGEST_AND_REFERENCES];
        foreach (LogSegment& s, list)
            digest.addSegmentId(s.id);
    }

    Buffer buffer;
    digest.appendToBuffer(buffer);
    bool success = newHead->append(LOG_ENTRY_TYPE_LOGDIGEST, buffer);
    if (!success) {
        throw FatalError(HERE, format("Could not append log digest of %u bytes "
            "to segment", buffer.getTotalLength()));
    }
}

/**
 * Return a pointer to the current head segment. If there is no head segment
 * yet, NULL is returned.
 */
LogSegment*
SegmentManager::getHeadSegment()
{
    if (segmentsByState[HEAD].size() == 0)
        return NULL;

    assert(segmentsByState[HEAD].size() == 1);
    return &segmentsByState[HEAD].front();
}

/**
 * Transition a segment from one state to another. This involves changing the
 * state associated with the segment and moving it to the appropriate new list.
 *
 * \param s
 *      Segment whose state is to be changed.
 * \param newState
 *      New state of the segment.
 */
void
SegmentManager::changeState(LogSegment& s, State newState)
{
    removeFromLists(s);
    *states[s.slot] = newState;
    addToLists(s);
}

/**
 * Determine whether or not an allocation request can be fulfilled, generally
 * due to insufficient memory. Only intended for invocation by alloc().
 *
 * \param forCleaner
 *      If true, check whether allocation of a cleaner survivor segment can be
 *      fulfilled. If false, check if a new head segment allocation can be
 *      fulfilled.
 * \return
 *      True if the allocation is permitted, else false.
 */
bool
SegmentManager::mayAlloc(AllocationType type)
{
    assert(numEmergencyHeadsAlloced <= numEmergencyHeads);
    assert(numSurvivorSegmentsAlloced <= numSurvivorSegments);

    size_t emergencyHeadsReserved = numEmergencyHeads -
                                      numEmergencyHeadsAlloced;
    size_t survivorSegmentsReserved = numSurvivorSegments -
                                        numSurvivorSegmentsAlloced;
    size_t totalReserved = emergencyHeadsReserved + survivorSegmentsReserved;

    assert(allocator.getFreeSegmentCount() >= totalReserved);

    if (type == ALLOC_EMERGENCY_HEAD) {
        // So long as the internal SegmentManager code does what it should,
        // (that is, allocates no more than two emergency heads before freeing
        // one), there will always be a free reservation when this is called.
        assert(emergencyHeadsReserved > 0);
        assert(freeSlots.size() > 0);
        assert(allocator.getFreeSegmentCount() > 0);
        return true;
    }
    
    if (type == ALLOC_SURVIVOR) {
        if (survivorSegmentsReserved == 0)
            return false;
        assert(freeSlots.size() > 0);
        assert(allocator.getFreeSegmentCount() > 0);
        return true;
    }

    if (allocator.getFreeSegmentCount() <= totalReserved)
        return false;

    // Should always have at least as many slots free as segments.
    assert(freeSlots.size() > 0);

    return true;
}

/**
 * Allocate a new segment, if possible, and set its initial state appropriately.
 *
 * \param forCleaner
 *      If true, allocate a segment for the cleaner. If false, the allocation is
 *      for a new head segment.
 * \return
 *      NULL if the allocation failed, otherwise a pointer to the newly
 *      allocated segment.
 */
LogSegment*
SegmentManager::alloc(AllocationType type)
{
    TEST_LOG((type == ALLOC_SURVIVOR) ? "for cleaner" : ((type == ALLOC_HEAD) ?
        "for head of log" : "for emergency head of log"));

    freeUnreferencedSegments();

    if (!mayAlloc(type))
        return NULL;

    uint64_t id = nextSegmentId++;
    uint32_t slot = freeSlots.back();
    freeSlots.pop_back();
    assert(!segments[slot]);

    State state = (type == ALLOC_SURVIVOR) ? CLEANING_INTO : HEAD;
    segments[slot].construct(allocator,
                             id,
                             slot,
                             (type == ALLOC_EMERGENCY_HEAD));
    states[slot].construct(state);
    idToSlotMap[id] = slot;

    LogSegment& s = *segments[slot];
    addToLists(s);

    if (type == ALLOC_SURVIVOR)
        numSurvivorSegmentsAlloced++;
    else if (type == ALLOC_EMERGENCY_HEAD)
        numEmergencyHeadsAlloced++;

    return &s;
}

/**
 * Free the given segment, returning its memory to the allocator and removing
 * it from all lists. This should only be called once the segment manager
 * determines that its safe to destroy a segment and reuse the memory. This
 * means that the segment must no longer part of the log and no references to
 * it may still exist.
 * 
 * \param s
 *      The segment to be freed. The segment should either have been an
 *      emergency head segment, or have been in the FREEABLE_PENDING_REFERENCES
 *      state at the time this is called.
 */
void
SegmentManager::free(LogSegment* s)
{
    uint32_t slot = s->slot;
    uint64_t id = segments[slot]->id;
    bool isEmergencyHead = s->isEmergencyHead;

    freeSlots.push_back(slot);
    idToSlotMap.erase(id);

    removeFromLists(*s);
    states[slot].destroy();
    segments[slot].destroy();

    // Update these after freeing the segment/seglets to avoid a race with the
    // assertion in mayAlloc that ensures our total free segment count is >= to
    // all that's been reserved.
    //
    // XXXXX- this is sufficient for the emergency head, but not for the survivor
    // segments count. We can't decrement until a pull segment's worth has been
    // freed. Should we just keep a counter around? Is it worth the trouble just
    // for an assertion (albeit a crucial one)?
    if (isEmergencyHead)
        numEmergencyHeadsAlloced--;
    else if (numSurvivorSegmentsAlloced > 0)
        numSurvivorSegmentsAlloced--;
}

/**
 * Add the given segment to the lists appropriate for its current state. This
 * assumes the state field has already been assigned in the 'states' array
 * associated with the segment's slot. The segment must also not be on any
 * list already.
 *
 * \param s
 *      Segment to be added to appropriate lists.
 */
void
SegmentManager::addToLists(LogSegment& s)
{
    allSegments.push_back(s);
    segmentsByState[*states[s.slot]].push_back(s);
}

/**
 * Remove the given segment from all lists.
 *
 * \param s
 *      The segment being removed from lists.
 */
void
SegmentManager::removeFromLists(LogSegment& s)
{
    State state = *states[s.slot];
    segmentsByState[state].erase(
        segmentsByState[state].iterator_to(s));
    allSegments.erase(allSegments.iterator_to(s));
}

/**
 * Scan potentially freeable segments and check whether any RPCs could still
 * refer to them. Any segments with no outstanding possible references are
 * immediately freed.
 */
void
SegmentManager::freeUnreferencedSegments()
{
    SegmentList& freeablePending = segmentsByState[FREEABLE_PENDING_REFERENCES];
    if (freeablePending.empty())
        return;

    uint64_t earliestEpoch =
        ServerRpcPool<>::getEarliestOutstandingEpoch(context);
    SegmentList::iterator it = freeablePending.begin();

    while (it != freeablePending.end()) {
        LogSegment& s = *it;
        ++it;
        if (s.cleanedEpoch < earliestEpoch)
            free(&s);
    }
}

} // namespace