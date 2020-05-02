/*
 * Copyright (c) 2020 Peking University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Rock Lee
 */

#include <cmath>

#include "mem/cache/base.hh"
#include "mem/cache/cache.hh"
#include "mem/cache/prefetch_filter/ppf_trainer.hh"
#include "mem/cache/prefetch_filter/ppf.hh"
#include "mem/cache/prefetch_filter/debug_flag.hh"

namespace prefetch_filter {

std::string getTrainingTypeStr(const TrainingType type) {
    switch (type) {
    case GoodPref: return "GoodPref";
    case BadPref: return "BadPref";
    case UselessPref: return "UselessPref";
    case DemandMiss: return "DemandMiss";
    default: return "Unknown";
    }
}

int TrainingEvent::setTrainingTarget(BaseCache* cache) {
    CHECK_ARGS(cache_ == nullptr, "Can not set training target for "
            "training event already with training target");
    cache_ = cache;
    return 0;
}

int TrainingEvent::setReadyTime(const Tick& readyTime) {
    CHECK_ARGS(readyTime > tickNow_, "Training Event can not be ready "
            "already [%lu] at %lu", readyTime, tickNow_);
    readyTime_ = readyTime;
    return 0;
}

int TrainingEventDistributor::init(const uint32_t queueSize) {
    CHECK_ARGS(queueSize > 0, "Illegal event queue size.");
    queueSize_ = queueSize;
    return 0;
}

int TrainingEventDistributor::init(const std::set<BaseCache*>& caches) {
    for (auto cache : caches) {
        eventQueues_[cache] = std::list<TrainingEvent>();
    }
    return 0;
}

int TrainingEventDistributor::addTrainingEvent(TrainingEvent event) {
    if (workingTick_ != tickNow_) {
        invalidTargets_.clear();
        workingTick_ = tickNow_;
    }
    
    DEBUG_PF(2, "Add event for prefetch @0x%lx [src-%s tgt-%s] type %s",
            event.addr_, BaseCache::levelName_[event.srcCacheLevel_].c_str(),
            event.cache_->getName().c_str(),
            getTrainingTypeStr(event.type_).c_str());
    if (invalidTargets_.find(event.cache_) == invalidTargets_.end()) {
        int dismissed = 0;
        auto queueMapIter = eventQueues_.find(event.cache_);
        CHECK_ARGS(queueMapIter != eventQueues_.end(),
                "Can not find valid event queue for given target")
        std::list<TrainingEvent>& eventQueue = queueMapIter->second;
        CHECK_ARGS(eventQueue.size() <= queueSize_,
                "Unexpected event queue size");
        dismissed = eventQueue.size() == queueSize_;
        if (dismissed) {
            TrainingEvent& dismissedEvent = eventQueue.front();
            DEBUG_PF(3, "Delete event for prefetch @0x%lx [src-%s tgt-%s] "
                    "type %s as queue is full", dismissedEvent.addr_,
                    BaseCache::levelName_[dismissedEvent.srcCacheLevel_].c_str(),
                    dismissedEvent.cache_->getName().c_str(),
                    getTrainingTypeStr(dismissedEvent.type_).c_str());
            eventQueue.pop_front();
        }
        CHECK_RET(event.setReadyTime(tickNow_ +
                BasePrefetchFilter::clockPeriod_ * (eventQueue.size() + 1)),
                "Failed to set training event ready time");
        DEBUG_PF(3, "Set training event ready time %lu at tick %lu",
                event.readyTime_, tickNow_);
        eventQueue.push_back(event);
        invalidTargets_.insert(event.cache_);
        return dismissed;
    } else {
        DEBUG_PF(3, "Training event is dismissed");
        // 忽略已经添加過事件的目标对应的训练事件
        return 1;
    }
}
    
int TrainingEventDistributor::getReadyTrainingEvent(
        ReadyEventQueue* readyEvents) {
    for (auto& queueMapIter : eventQueues_) {
        std::list<TrainingEvent>& eventQueue = queueMapIter.second;
        for (auto queueIter = eventQueue.begin();
                queueIter != eventQueue.end();) {
            if (queueIter->readyTime_ < tickNow_) {
                readyEvents->push(*queueIter);
                queueIter = eventQueue.erase(queueIter);
            } else {
                break;
            }
        }
    }
    return 0;
}

int PPFTrainer::init(const std::set<BaseCache*>& caches, const int8_t tagBits,
        const uint32_t targetTableSize, const uint8_t targetTableAssoc,
        const uint32_t eventQueueSize) {
    CHECK_RET(distributor_.init(eventQueueSize),
            "Failed to init training event distributor in 1st step");
    CHECK_RET(distributor_.init(caches),
            "Failed to init training event distributor in 2nd step");
    for (auto cache : caches) {
        targetTables_[cache] = TrainingTargetTable();
        CHECK_RET(targetTables_[cache].init(tagBits, targetTableSize,
                targetTableAssoc, BasePrefetchFilter::cacheLineOffsetBits_),
                "Failed to init target table");
    }
    return 0;
}

int PPFTrainer::init(const int8_t tagBits, const uint32_t targetTableSize,
        const uint8_t targetTableAssoc, const uint32_t eventQueueSize) {
    CHECK_RET(distributor_.init(eventQueueSize),
            "Failed to init training event distributor");
    targetTables_[nullptr] = TrainingTargetTable();
    CHECK_RET(targetTables_[nullptr].init(tagBits, targetTableSize,
            targetTableAssoc, BasePrefetchFilter::cacheLineOffsetBits_),
            "Failed to init target table");
    return 0;
}

int PPFTrainer::init(const std::set<BaseCache*>& caches) {
    CHECK_RET(distributor_.init(caches),
            "Failed to init training event distributor");
    auto mapIter = targetTables_.find(nullptr);
    CHECK_ARGS(mapIter != targetTables_.end(),
            "Trying to init with incomplete settings");
    TrainingTargetTable sampleTable = mapIter->second;
    targetTables_.clear();
    for (auto cache : caches) {
        targetTables_[cache] = sampleTable;
    }
    return 0;
}

int PPFTrainer::addTrainingEvent(BaseCache* cache, const uint64_t& addr,
        const TrainingType type, std::set<BaseCache*>* fullTargets) {
    int result;
    auto tableMapIter = targetTables_.find(cache);
    CHECK_ARGS(tableMapIter != targetTables_.end(),
            "Can not find training target table for given cache");
    std::set<BaseCache*>* trainingTargets;
    CHECK_RET(result = tableMapIter->second.read(addr,
            &trainingTargets, false),
            "Failed to get target cache for given prefetch");
    if (result) {
        uint8_t cacheLevel = cache->cacheLevel_;
        for (auto targetCache : *trainingTargets) {
            CHECK_RET(result = distributor_.addTrainingEvent(
                    TrainingEvent(addr, type, cacheLevel, targetCache)),
                    "Failed to add training event to distributor");
            if (!result) {
                fullTargets->erase(targetCache);
            }
        }
    }
    return 0;
}

int PPFTrainer::completeTrainingEvent(PerceptronPrefetchFilter* prefFilter) {
    ReadyEventQueue readyQueue;
    CHECK_RET(distributor_.getReadyTrainingEvent(&readyQueue),
            "Failed to get ready training event");
    while(readyQueue.size()) {
        const TrainingEvent& event = readyQueue.top();
        CHECK_ARGS(event.cache_ != nullptr,
                "No valid target for training event");
        if (event.cache_->cacheLevel_) {
            DEBUG_PF(2, "Start completing training event for prefetch @0x%lx "
                    "[src-%s tgt-%s] type %s at %lu", event.addr_,
                    BaseCache::levelName_[event.srcCacheLevel_].c_str(),
                    event.cache_->getName().c_str(),
                    getTrainingTypeStr(event.type_).c_str(), tickNow_);
            PerceptronPrefetchFilter::Tables& workTable =
                    prefFilter->getTable(event.cache_);
            CHECK_RET(prefFilter->train(workTable, event.addr_,
                    event.srcCacheLevel_, event.type_),
                    "Faield to complete single training event");
        }
        readyQueue.pop();
    }
    return 0;
}

int PPFTrainer::addPref(BaseCache* cache, const uint64_t& addr,
        const std::set<BaseCache*>& targets) {
    int result;
    std::set<BaseCache*> fullTarget = targets;
    auto tableMapIter = targetTables_.find(cache);
    CHECK_ARGS(tableMapIter != targetTables_.end(),
            "Can not find training target table for given cache");
    std::set<BaseCache*>* oldTargets = nullptr;
    CHECK_RET(result = tableMapIter->second.read(addr, &oldTargets, false),
            "Failed to get target cache for given prefetch");
    if (result) {
        fullTarget.insert(oldTargets->begin(), oldTargets->end());
        CHECK_RET(result = tableMapIter->second.write(addr, fullTarget, false),
                "Failed to write new target to target table");
        CHECK_ARGS(result == 0 || result == 3, "Unexpected write status");
    } else {
        CHECK_RET(result = tableMapIter->second.write(addr, fullTarget, false),
                "Failed to write new target to target table");
        CHECK_ARGS(result == 1 || result == 2, "Unexpected write status");
    }
    return 0;
}

int PPFTrainer::invalidatePref(BaseCache* cache, const uint64_t& addr) {
    int result;
    auto tableMapIter = targetTables_.find(cache);
    CHECK_ARGS(tableMapIter != targetTables_.end(),
            "Can not find training target table for given cache");
    CHECK_RET(result = tableMapIter->second.invalidate(addr, false),
            "Failed to get target cache for given prefetch");
    return 0;
}

int PPFTrainer::deletePref(BaseCache* cache, const uint64_t& addr) {
    return invalidatePref(cache, addr);
}

} // namespace prefetch_filter
